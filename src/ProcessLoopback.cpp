#ifdef _WIN32
#include "ProcessLoopback.h"
#include "Log.h"
#include <Windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <chrono>
#include <vector>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivateHandler() { event_=CreateEventW(nullptr,FALSE,FALSE,nullptr); }
    ~ActivateHandler() { if(event_)CloseHandle(event_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IActivateAudioInterfaceCompletionHandler))
            *out=static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if(iid==__uuidof(IAgileObject))*out=static_cast<IAgileObject*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override {auto r=--refs_;if(!r)delete this;return r;}
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        ComPtr<IUnknown> u;
        HRESULT activateHr=E_FAIL;
        hr_=op->GetActivateResult(&activateHr,&u);
        if(SUCCEEDED(hr_))hr_=activateHr;
        if(SUCCEEDED(hr_))hr_=u.As(&client_);
        SetEvent(event_);return S_OK;
    }
    HRESULT Wait(ComPtr<IAudioClient>& out) {
        if(WaitForSingleObject(event_,5000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(SUCCEEDED(hr_))out=client_;
        return hr_;
    }
private:
    std::atomic<ULONG> refs_{1};
    HANDLE event_{};
    HRESULT hr_=E_FAIL;
    ComPtr<IAudioClient> client_;
};
}

bool ProcessLoopback::Start(unsigned long pid) {
    if(thread_.joinable())return running_;
    stop_=false;ready_=false;startOk_=false;
    thread_=std::thread(&ProcessLoopback::threadMain,this,pid);
    std::unique_lock lk(readyMutex_);
    readyCv_.wait_for(lk,std::chrono::seconds(6),[&]{return ready_;});
    return startOk_;
}
void ProcessLoopback::Stop() {
    stop_=true;
    if(thread_.joinable())thread_.join();
    running_=false;
}

void ProcessLoopback::threadMain(unsigned long pid) {
    auto signalReady=[&](bool ok){
        std::lock_guard lk(readyMutex_);startOk_=ok;ready_=true;readyCv_.notify_all();
    };
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    bool co=SUCCEEDED(hr);
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE){Log::Error("Process loopback: CoInitializeEx failed");signalReady(false);return;}

    HANDLE packetEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!packetEvent){signalReady(false);if(co)CoUninitialize();return;}

    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId=pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode=PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt=VT_BLOB;pv.blob.cbSize=sizeof(ap);pv.blob.pBlobData=reinterpret_cast<BYTE*>(&ap);

    auto* handler=new ActivateHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    hr=ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),&pv,handler,&op);
    ComPtr<IAudioClient> client;
    if(SUCCEEDED(hr))hr=handler->Wait(client);
    handler->Release();
    if(FAILED(hr)) {
        std::ostringstream os;os<<"Process loopback activation failed HRESULT=0x"<<std::hex<<(unsigned)hr;
        Log::Error(os.str());CloseHandle(packetEvent);signalReady(false);if(co)CoUninitialize();return;
    }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels=2;
    fmt.nSamplesPerSec=48000;
    fmt.wBitsPerSample=32;
    fmt.nBlockAlign=fmt.nChannels*fmt.wBitsPerSample/8;
    fmt.nAvgBytesPerSec=fmt.nSamplesPerSec*fmt.nBlockAlign;

    hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_EVENTCALLBACK|
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0,0,&fmt,nullptr);
    if(SUCCEEDED(hr))hr=client->SetEventHandle(packetEvent);
    ComPtr<IAudioCaptureClient> capture;
    if(SUCCEEDED(hr))hr=client->GetService(IID_PPV_ARGS(&capture));
    if(SUCCEEDED(hr))hr=client->Start();
    if(FAILED(hr)) {
        std::ostringstream os;os<<"Process loopback initialize failed HRESULT=0x"<<std::hex<<(unsigned)hr;
        Log::Error(os.str());CloseHandle(packetEvent);signalReady(false);if(co)CoUninitialize();return;
    }

    running_=true;signalReady(true);
    Log::Info("Process-only WASAPI loopback capture started (48 kHz stereo float).");
    std::vector<float> silence;

    while(!stop_) {
        DWORD wr=WaitForSingleObject(packetEvent,50);
        if(wr!=WAIT_OBJECT_0)continue;
        UINT32 next=0;
        while(SUCCEEDED(capture->GetNextPacketSize(&next))&&next>0) {
            BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;UINT64 pos=0,qpc=0;
            hr=capture->GetBuffer(&data,&frames,&flags,&pos,&qpc);
            if(FAILED(hr))break;
            if(flags&AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign(static_cast<size_t>(frames)*2,0.0f);
                engine_.PushAudioFloatStereo(silence.data(),frames,48000);
            } else {
                engine_.PushAudioFloatStereo(reinterpret_cast<const float*>(data),frames,48000);
            }
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    running_=false;
    CloseHandle(packetEvent);
    if(co)CoUninitialize();
}
#endif
