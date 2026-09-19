#ifdef _WIN32
#include "DualSenseAudio.h"
#include "Log.h"
#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
// Windows SDK 10.0.26100 no longer reliably supplies DEFINE_PROPERTYKEY
// before FunctionDiscoveryKeys_devpkey.h in every include combination.
// Include the property-system definitions explicitly and first.
#include <propkeydef.h>
#include <propsys.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring lower(std::wstring s){
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    return s;
}
std::string narrow(const std::wstring& s){return std::string(s.begin(),s.end());}

WAVEFORMATEXTENSIBLE makeDualSenseFormat(){
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels=4;
    f.Format.nSamplesPerSec=48000;
    f.Format.wBitsPerSample=16;
    f.Format.nBlockAlign=static_cast<WORD>(f.Format.nChannels*f.Format.wBitsPerSample/8);
    f.Format.nAvgBytesPerSec=f.Format.nSamplesPerSec*f.Format.nBlockAlign;
    f.Format.cbSize=sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample=16;
    // 0x33: Front L/R + Back/Surround L/R. The rear pair are the two haptic actuators.
    f.dwChannelMask=KSAUDIO_SPEAKER_QUAD;
    f.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

bool quadCapable(IMMDevice* device){
    ComPtr<IAudioClient> client;
    if(FAILED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client)))return false;

    bool mixHasFour=false;
    WAVEFORMATEX* mix=nullptr;
    if(SUCCEEDED(client->GetMixFormat(&mix))&&mix){
        mixHasFour=mix->nChannels>=4;
        CoTaskMemFree(mix);
    }

    auto desired=makeDualSenseFormat();
    WAVEFORMATEX* closest=nullptr;
    const HRESULT support=client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,&desired.Format,&closest);
    if(closest)CoTaskMemFree(closest);
    return mixHasFour || support==S_OK;
}
}

bool DualSenseAudio::Start(){
    if(thread_.joinable())return running_;
    stop_=false;ready_=false;startOk_=false;
    thread_=std::thread(&DualSenseAudio::threadMain,this);
    std::unique_lock lk(readyMutex_);
    readyCv_.wait_for(lk,std::chrono::seconds(5),[&]{return ready_;});
    return startOk_;
}

void DualSenseAudio::Stop(){
    stop_=true;
    if(thread_.joinable())thread_.join();
    running_=false;
}

void DualSenseAudio::threadMain(){
    bool signaled=false;
    auto signal=[&](bool ok){
        if(signaled)return;
        std::lock_guard lk(readyMutex_);
        startOk_=ok;ready_=true;signaled=true;readyCv_.notify_all();
    };

    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const bool co=SUCCEEDED(hr);
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE){signal(false);return;}

    bool missingLogged=false;
    while(!stop_){
        ComPtr<IMMDeviceEnumerator> en;
        hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en));
        ComPtr<IMMDeviceCollection> col;
        if(SUCCEEDED(hr))hr=en->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&col);
        if(FAILED(hr)){
            if(!missingLogged){Log::Warn("Cannot enumerate Windows audio endpoints; retrying.");missingLogged=true;}
            signal(false);
            for(int i=0;i<10&&!stop_;++i)Sleep(100);
            continue;
        }

        UINT count=0;col->GetCount(&count);
        ComPtr<IMMDevice> chosen;std::wstring chosenName;
        const auto needle=lower(cfg_.audio.endpointContains);
        for(UINT i=0;i<count;i++){
            ComPtr<IMMDevice> d;if(FAILED(col->Item(i,&d)))continue;
            ComPtr<IPropertyStore> ps;if(FAILED(d->OpenPropertyStore(STGM_READ,&ps)))continue;
            PROPVARIANT pv;PropVariantInit(&pv);
            std::wstring name;
            if(SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName,&pv))&&pv.vt==VT_LPWSTR&&pv.pwszVal)
                name=pv.pwszVal;
            PropVariantClear(&pv);
            if(lower(name).find(needle)==std::wstring::npos)continue;
            if(quadCapable(d.Get())){chosen=d;chosenName=name;break;}
        }

        if(!chosen){
            if(!missingLogged){
                Log::Warn("No active quad DualSense audio endpoint found; USB audio will be retried in the background.");
                missingLogged=true;
            }
            signal(false);
            for(int i=0;i<10&&!stop_;++i)Sleep(100);
            continue;
        }
        missingLogged=false;

        ComPtr<IAudioClient> client;
        hr=chosen->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client);
        if(FAILED(hr)){
            Log::Warn("Could not activate the DualSense WASAPI render client; retrying.");
            signal(false);for(int i=0;i<5&&!stop_;++i)Sleep(100);continue;
        }

        auto fmt=makeDualSenseFormat();
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event){
            Log::Error("Could not create DualSense WASAPI render event.");
            signal(false);for(int i=0;i<5&&!stop_;++i)Sleep(100);continue;
        }

        hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0,0,&fmt.Format,nullptr);
        if(SUCCEEDED(hr))hr=client->SetEventHandle(event);

        UINT32 bufferFrames=0;
        if(SUCCEEDED(hr))hr=client->GetBufferSize(&bufferFrames);
        ComPtr<IAudioRenderClient> render;
        if(SUCCEEDED(hr))hr=client->GetService(IID_PPV_ARGS(&render));

        if(SUCCEEDED(hr)&&bufferFrames){
            BYTE* first=nullptr;
            if(SUCCEEDED(render->GetBuffer(bufferFrames,&first)))
                render->ReleaseBuffer(bufferFrames,AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if(SUCCEEDED(hr))hr=client->Start();
        if(FAILED(hr)){
            std::ostringstream os;os<<"DualSense WASAPI render init failed HRESULT=0x"<<std::hex<<(unsigned)hr<<"; retrying.";
            Log::Warn(os.str());
            CloseHandle(event);signal(false);
            for(int i=0;i<5&&!stop_;++i)Sleep(100);
            continue;
        }

        constexpr uint32_t sr=48000;
        constexpr uint16_t channels=4;
        std::vector<float> lr(static_cast<size_t>(std::max<UINT32>(bufferFrames,256))*2);
        running_=true;signal(true);
        Log::Info("DualSense haptic audio output started: "+narrow(chosenName)+
                  ", 48000 Hz, 16-bit quad PCM (rear L/R = haptics).");

        bool streamLost=false;
        while(!stop_&&!streamLost){
            const DWORD wr=WaitForSingleObject(event,50);
            if(wr==WAIT_TIMEOUT)continue;
            if(wr!=WAIT_OBJECT_0){streamLost=true;break;}

            UINT32 padding=0;
            if(FAILED(client->GetCurrentPadding(&padding))){streamLost=true;break;}
            const UINT32 frames=bufferFrames>padding?bufferFrames-padding:0;
            if(frames==0)continue;

            BYTE* data=nullptr;
            if(FAILED(render->GetBuffer(frames,&data))){streamLost=true;break;}
            if(lr.size()<static_cast<size_t>(frames)*2)lr.resize(static_cast<size_t>(frames)*2);
            engine_.RenderBlock(lr.data(),frames,sr);

            auto* out=reinterpret_cast<int16_t*>(data);
            for(UINT32 i=0;i<frames;i++){
                out[static_cast<size_t>(i)*channels+0]=0;
                out[static_cast<size_t>(i)*channels+1]=0;
                out[static_cast<size_t>(i)*channels+2]=static_cast<int16_t>(
                    std::clamp(lr[i*2],-1.0f,1.0f)*32767.0f);
                out[static_cast<size_t>(i)*channels+3]=static_cast<int16_t>(
                    std::clamp(lr[i*2+1],-1.0f,1.0f)*32767.0f);
            }
            if(FAILED(render->ReleaseBuffer(frames,0)))streamLost=true;
        }

        client->Stop();running_=false;
        CloseHandle(event);
        if(!stop_){
            Log::Warn("DualSense haptic audio endpoint was lost; waiting for USB reconnect.");
            for(int i=0;i<5&&!stop_;++i)Sleep(100);
        }
    }

    running_=false;
    signal(false);
    if(co)CoUninitialize();
}
#endif
