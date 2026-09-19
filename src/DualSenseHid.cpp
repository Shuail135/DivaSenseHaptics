#ifdef _WIN32
#include "DualSenseHid.h"
#include "Log.h"
#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <algorithm>
#include <chrono>
#include <sstream>

#pragma comment(lib,"hid.lib")
#pragma comment(lib,"setupapi.lib")

namespace {
constexpr USHORT SONY=0x054c;
constexpr USHORT DS5=0x0ce6;
constexpr USHORT DS5_EDGE=0x0df2;
bool supportedPid(USHORT p){return p==DS5||p==DS5_EDGE;}
}

bool DualSenseHid::Start(ReportCallback cb) {
    if(thread_.joinable())return running_;
    callback_=std::move(cb);stop_=false;ready_=false;startOk_=false;
    thread_=std::thread(&DualSenseHid::threadMain,this);
    std::unique_lock lk(readyMutex_);
    readyCv_.wait_for(lk,std::chrono::seconds(4),[&]{return ready_;});
    return startOk_;
}
void DualSenseHid::Stop(){stop_=true;if(thread_.joinable())thread_.join();closeController();running_=false;}

bool DualSenseHid::HasRecentInput(unsigned maxAgeMs) const {
    const auto last=lastInputTick_.load(std::memory_order_acquire);
    if(!running_.load(std::memory_order_acquire) || last==0)return false;
    const auto now=GetTickCount64();
    return now>=last && (now-last)<=maxAgeMs;
}

bool DualSenseHid::openController() {
    GUID guid{};HidD_GetHidGuid(&guid);
    HDEVINFO set=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)return false;
    SP_DEVICE_INTERFACE_DATA ifd{};ifd.cbSize=sizeof(ifd);
    bool found=false;
    for(DWORD i=0;SetupDiEnumDeviceInterfaces(set,nullptr,&guid,i,&ifd);++i) {
        DWORD need=0;SetupDiGetDeviceInterfaceDetailW(set,&ifd,nullptr,0,&need,nullptr);
        std::vector<BYTE> mem(need);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(mem.data());
        detail->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&ifd,detail,need,nullptr,nullptr))continue;
        // Probe with zero desired access first. This lets us inspect the HID
        // collection even when another process has a more restrictive handle.
        HANDLE probe=CreateFileW(detail->DevicePath,0,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(probe==INVALID_HANDLE_VALUE)continue;
        HIDD_ATTRIBUTES attr{};attr.Size=sizeof(attr);
        if(HidD_GetAttributes(probe,&attr)&&attr.VendorID==SONY&&supportedPid(attr.ProductID)) {
            PHIDP_PREPARSED_DATA prep=nullptr;
            bool gamepadCollection=false;
            if(HidD_GetPreparsedData(probe,&prep)) {
                HIDP_CAPS caps{};
                if(HidP_GetCaps(prep,&caps)==HIDP_STATUS_SUCCESS) {
                    inputReportBytes_=caps.InputReportByteLength;
                    outputReportBytes_=caps.OutputReportByteLength;
                    gamepadCollection=(caps.UsagePage==0x01 && caps.Usage==0x05);
                }
                HidD_FreePreparsedData(prep);
            }
            CloseHandle(probe);probe=INVALID_HANDLE_VALUE;
            if(!gamepadCollection)continue;

            // Input is intentionally READ ONLY. A separate best-effort write
            // handle is used for the audio-haptics mode packet.
            HANDLE input=CreateFileW(detail->DevicePath,GENERIC_READ,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(input==INVALID_HANDLE_VALUE)continue;
            HANDLE control=CreateFileW(detail->DevicePath,GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);

            inputHandle_=input;
            controlHandle_=control;
            useSetOutputReport_=false;
            lastInputTick_.store(0,std::memory_order_release);
            found=true;
            Log::Info(attr.ProductID==DS5_EDGE?"DualSense Edge USB HID input opened.":"DualSense USB HID input opened.");
            if(control==INVALID_HANDLE_VALUE)
                Log::Warn("DualSense HID control handle is unavailable; button haptics will still work, but legacy-rumble recovery is disabled.");
            break;
        }
        if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
void DualSenseHid::closeController(){
    HANDLE input=reinterpret_cast<HANDLE>(inputHandle_);
    if(input&&input!=INVALID_HANDLE_VALUE){CancelIoEx(input,nullptr);CloseHandle(input);}
    HANDLE control=reinterpret_cast<HANDLE>(controlHandle_);
    if(control&&control!=INVALID_HANDLE_VALUE)CloseHandle(control);
    inputHandle_=reinterpret_cast<void*>(-1);
    controlHandle_=reinterpret_cast<void*>(-1);
    lastInputTick_.store(0,std::memory_order_release);
}
bool DualSenseHid::sendAudioHapticsEnable(unsigned long& error){
    HANDLE h=reinterpret_cast<HANDLE>(controlHandle_);
    error=ERROR_SUCCESS;
    if(h==INVALID_HANDLE_VALUE){error=ERROR_INVALID_HANDLE;return false;}
    std::vector<unsigned char> report(std::max(48u,outputReportBytes_),0);
    report[0]=0x02;
    // valid_flag0 bit 0 = COMPATIBLE_VIBRATION. With zero motor values and
    // HAPTICS_SELECT (bit 1) left clear, the controller exits classic rumble
    // selection and returns the voice coils to the USB audio-haptics path.
    report[1]=0x01;
    report[3]=0x00; // right compatibility motor
    report[4]=0x00; // left compatibility motor
    // Continuous output belongs on the HID interrupt/output path. Some drivers
    // only support SetOutputReport, so retain it as a compatibility fallback.
    if(!useSetOutputReport_){
        OVERLAPPED write{};
        write.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!write.hEvent){error=GetLastError();return false;}
        DWORD written=0;
        BOOL ok=WriteFile(h,report.data(),static_cast<DWORD>(report.size()),&written,&write);
        if(!ok && GetLastError()==ERROR_IO_PENDING){
            if(WaitForSingleObject(write.hEvent,100)==WAIT_OBJECT_0){
                ok=GetOverlappedResult(h,&write,&written,FALSE);
            }else{
                CancelIoEx(h,&write);
                // Keep the buffer, event and OVERLAPPED alive until cancellation completes.
                GetOverlappedResult(h,&write,&written,TRUE);
                CloseHandle(write.hEvent);
                error=ERROR_TIMEOUT;
                return false;
            }
        }
        const DWORD writeError=ok ? ERROR_WRITE_FAULT : GetLastError();
        CloseHandle(write.hEvent);
        if(ok && written==report.size())return true;
        error=writeError;
    }
    if(HidD_SetOutputReport(h,report.data(),static_cast<ULONG>(report.size()))){
        useSetOutputReport_=true;
        return true;
    }
    error=GetLastError();
    useSetOutputReport_=false;
    return false;
}

void DualSenseHid::threadMain(){
    bool signaled=false;
    auto ready=[&](bool ok){
        if(signaled)return;
        std::lock_guard lk(readyMutex_);
        startOk_=ok;ready_=true;signaled=true;readyCv_.notify_all();
    };

    bool missingLogged=false;
    while(!stop_) {
        if(!openController()) {
            running_=false;
            if(!missingLogged){
                Log::Warn("No USB DualSense/Edge HID found; retrying in the background.");
                missingLogged=true;
            }
            ready(false);
            for(int i=0;i<10&&!stop_;++i)Sleep(100);
            continue;
        }
        missingLogged=false;
        ready(true);
        running_=true;

        HANDLE h=reinterpret_cast<HANDLE>(inputHandle_);
        const DWORD readSize=static_cast<DWORD>(std::max(64u,inputReportBytes_));
        std::vector<unsigned char> report(readSize,0);
        OVERLAPPED ov{};
        ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!ov.hEvent){
            Log::Error("Could not create DualSense HID read event.");
            running_=false;closeController();
            for(int i=0;i<10&&!stop_;++i)Sleep(100);
            continue;
        }

        // HID input and the best-effort output/control path are independent.
        // Never tear down a healthy input stream just because a control packet
        // is rejected by Windows/Steam/the controller firmware.
        const bool controlAvailable=reinterpret_cast<HANDLE>(controlHandle_)!=INVALID_HANDLE_VALUE;
        bool controlFailureLogged=false;
        auto lastUnmute=std::chrono::steady_clock::now()-std::chrono::seconds(1);
        bool controlConfirmed=false;

        bool disconnected=false;
        bool firstReport=true;
        bool readPending=false;
        DWORD disconnectError=ERROR_SUCCESS;

        auto consumeReport=[&](DWORD got){
            if(got>=11){
                lastInputTick_.store(GetTickCount64(),std::memory_order_release);
                if(firstReport){
                    Log::Info("DualSense USB input reports are active; gameplay haptics input is online.");
                    firstReport=false;
                }
                if(callback_)callback_(report.data(),got);
            }
        };

        while(!stop_ && !disconnected) {
            // Keep a single overlapped read alive until it completes. A quiet
            // 25 ms interval is normal and must NOT be treated as a disconnect
            // or cancelled/reused immediately; doing so races CancelIoEx with
            // the next ReadFile and can yield ERROR_OPERATION_ABORTED.
            if(!readPending) {
                ResetEvent(ov.hEvent);
                DWORD got=0;
                BOOL ok=ReadFile(h,report.data(),readSize,&got,&ov);
                if(ok) {
                    consumeReport(got);
                } else {
                    const DWORD err=GetLastError();
                    if(err==ERROR_IO_PENDING) {
                        readPending=true;
                    } else {
                        disconnectError=err;
                        disconnected=true;
                    }
                }
            }

            if(readPending && !disconnected && !stop_) {
                const DWORD wr=WaitForSingleObject(ov.hEvent,25);
                if(wr==WAIT_OBJECT_0) {
                    DWORD got=0;
                    BOOL ok=GetOverlappedResult(h,&ov,&got,FALSE);
                    readPending=false;
                    if(ok) {
                        consumeReport(got);
                    } else {
                        const DWORD err=GetLastError();
                        // Cancellation is expected only during shutdown. Any
                        // other aborted/failed read is a real stream failure.
                        if(!(stop_ && err==ERROR_OPERATION_ABORTED)) {
                            disconnectError=err;
                            disconnected=true;
                        }
                    }
                } else if(wr==WAIT_TIMEOUT) {
                    // Normal idle interval. Leave the exact same read pending.
                } else {
                    disconnectError=GetLastError();
                    disconnected=true;
                }
            }

            if(cfg_.controller.forceAudioHaptics && controlAvailable && !disconnected) {
                const auto now=std::chrono::steady_clock::now();
                if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastUnmute).count()>=
                   (controlFailureLogged ? 1000 : std::max(10,cfg_.controller.unmuteIntervalMs))) {
                    unsigned long error=ERROR_SUCCESS;
                    if(!sendAudioHapticsEnable(error)) {
                        // Do not poison the read path. We can still receive
                        // buttons and the separate WASAPI endpoint can still
                        // carry haptic audio.
                        if(!controlFailureLogged)
                            Log::Warn("DualSense audio-haptics control failed (Win32 error " + std::to_string(error) +
                                      "); retrying every second. HID input and USB audio remain active, but controller haptics mode is unconfirmed.");
                        controlFailureLogged=true;
                    }else{
                        if(!controlConfirmed || controlFailureLogged)
                            Log::Info(std::string("DualSense audio-haptics control accepted via ")+
                                      (useSetOutputReport_ ? "SetOutputReport." : "WriteFile."));
                        controlConfirmed=true;
                        controlFailureLogged=false;
                    }
                    lastUnmute=now;
                }
            }
        }

        if(readPending) {
            CancelIoEx(h,&ov);
            // Wait briefly for the driver's cancellation completion.
            WaitForSingleObject(ov.hEvent,100);
        }
        running_=false;
        // Close the HID handle before destroying the event referenced by the
        // OVERLAPPED structure. Closing the handle is the final cancellation
        // barrier if a driver did not signal promptly.
        closeController();
        CloseHandle(ov.hEvent);
        if(!stop_) {
            if(disconnectError!=ERROR_SUCCESS) {
                Log::Warn("DualSense USB HID read failed (Win32 error " + std::to_string(disconnectError) + "); waiting for reconnect.");
            } else {
                Log::Warn("DualSense USB HID disconnected; waiting for reconnect.");
            }
            for(int i=0;i<10&&!stop_;++i)Sleep(100);
        }
    }
    ready(false);
}
#endif
