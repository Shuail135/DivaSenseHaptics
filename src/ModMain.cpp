#ifdef _WIN32
#include "ChartAwareness.h"
#include "ChartCapture.h"
#include "ChartMemoryScan.h"
#include "Config.h"
#include "DualSenseAudio.h"
#include "DualSenseHid.h"
#include "HapticEngine.h"
#include "InputHaptics.h"
#include "JudgementHaptics.h"
#include "JudgementHook.h"
#include "Log.h"
#include "MenuHaptics.h"
#include "ProcessLoopback.h"
#include <Windows.h>
#include <Xinput.h>
#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <memory>
#include <mutex>

namespace {
HMODULE gModule=nullptr;

std::filesystem::path moduleDirectory(){
    wchar_t path[32768]{};
    const DWORD cap=static_cast<DWORD>(sizeof(path)/sizeof(path[0]));
    const DWORD n=GetModuleFileNameW(gModule,path,cap);
    if(n==0 || n>=cap)return std::filesystem::current_path();
    return std::filesystem::path(path).parent_path();
}

struct App;
App& GetApp();

struct App {
    ModConfig cfg{};
    HapticEngine engine;
    std::unique_ptr<ChartAwareness> chart;
    std::unique_ptr<ChartCapture> chartCapture;
    std::unique_ptr<ChartMemoryScan> chartMemoryScan;
    std::unique_ptr<JudgementHaptics> judgement;
    std::unique_ptr<JudgementHook> judgementHook;
    std::unique_ptr<MenuHaptics> menu;
    std::unique_ptr<InputHaptics> input;
    std::unique_ptr<DualSenseHid> hid;
    std::unique_ptr<DualSenseAudio> output;
    std::unique_ptr<ProcessLoopback> capture;
    std::atomic<bool> initialized{false};
    bool wasGameplay=false;
    std::mutex inputMutex;
    std::chrono::steady_clock::time_point lastMemoryScanRequest{};

    using XInputGetStateFn=DWORD (WINAPI*)(DWORD,XINPUT_STATE*);
    HMODULE xinputModule=nullptr;
    XInputGetStateFn xinputGetState=nullptr;
    int xinputIndex=-1;
    bool xinputFallbackLogged=false;
    bool xinputAttempted=false;

    static uint8_t axisByte(SHORT v){
        const int n=static_cast<int>(v)+32768;
        return static_cast<uint8_t>((n*255+32767)/65535);
    }

    void initXInput(){
        if(xinputGetState || xinputAttempted)return;
        xinputAttempted=true;
        static const wchar_t* dlls[]={L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll"};
        for(auto* dll:dlls){
            HMODULE m=LoadLibraryW(dll);
            if(!m)continue;
            auto fn=reinterpret_cast<XInputGetStateFn>(GetProcAddress(m,"XInputGetState"));
            if(fn){xinputModule=m;xinputGetState=fn;break;}
            FreeLibrary(m);
        }
        if(!xinputGetState)Log::Warn("XInput fallback is unavailable; raw DualSense HID input will be used exclusively.");
    }

    void pollXInputFallback(){
        std::lock_guard lock(inputMutex);
        if(!input)return;
        // Raw USB HID, XInput fall back
        if(hid && hid->HasRecentInput(350))return;
        if(!xinputGetState)initXInput();
        if(!xinputGetState)return;

        XINPUT_STATE state{};
        DWORD result=ERROR_DEVICE_NOT_CONNECTED;
        if(xinputIndex>=0)result=xinputGetState(static_cast<DWORD>(xinputIndex),&state);
        if(result!=ERROR_SUCCESS){
            xinputIndex=-1;
            for(DWORD i=0;i<XUSER_MAX_COUNT;i++){
                if(xinputGetState(i,&state)==ERROR_SUCCESS){xinputIndex=static_cast<int>(i);result=ERROR_SUCCESS;break;}
            }
        }
        if(result!=ERROR_SUCCESS)return;

        if(!xinputFallbackLogged){
            Log::Info("Using XInput/Steam-input fallback for gameplay button haptics (raw DualSense HID reports are not active).");
            xinputFallbackLogged=true;
        }

        std::array<uint8_t,11> d{};
        d[0]=0x01;
        d[1]=axisByte(state.Gamepad.sThumbLX);
        d[2]=static_cast<uint8_t>(255-axisByte(state.Gamepad.sThumbLY));
        d[3]=axisByte(state.Gamepad.sThumbRX);
        d[4]=static_cast<uint8_t>(255-axisByte(state.Gamepad.sThumbRY));
        d[5]=state.Gamepad.bLeftTrigger;
        d[6]=state.Gamepad.bRightTrigger;

        const WORD b=state.Gamepad.wButtons;
        const bool up=(b&XINPUT_GAMEPAD_DPAD_UP)!=0;
        const bool right=(b&XINPUT_GAMEPAD_DPAD_RIGHT)!=0;
        const bool down=(b&XINPUT_GAMEPAD_DPAD_DOWN)!=0;
        const bool left=(b&XINPUT_GAMEPAD_DPAD_LEFT)!=0;
        uint8_t hat=8;
        if(up&&right)hat=1; else if(right&&down)hat=3; else if(down&&left)hat=5; else if(left&&up)hat=7;
        else if(up)hat=0; else if(right)hat=2; else if(down)hat=4; else if(left)hat=6;
        d[8]=hat;
        if(b&XINPUT_GAMEPAD_X)d[8]|=0x10; // Square
        if(b&XINPUT_GAMEPAD_A)d[8]|=0x20; // Cross
        if(b&XINPUT_GAMEPAD_B)d[8]|=0x40; // Circle
        if(b&XINPUT_GAMEPAD_Y)d[8]|=0x80; // Triangle
        if(b&XINPUT_GAMEPAD_LEFT_SHOULDER)d[9]|=0x01;
        if(b&XINPUT_GAMEPAD_RIGHT_SHOULDER)d[9]|=0x02;
        if(state.Gamepad.bLeftTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD)d[9]|=0x04;
        if(state.Gamepad.bRightTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD)d[9]|=0x08;
        if(b&XINPUT_GAMEPAD_BACK)d[9]|=0x10;
        if(b&XINPUT_GAMEPAD_START)d[9]|=0x20;
        if(b&XINPUT_GAMEPAD_LEFT_THUMB)d[9]|=0x40;
        if(b&XINPUT_GAMEPAD_RIGHT_THUMB)d[9]|=0x80;

        // Size 11 deliberately disables touch parsing for this synthesized
        // report; touchpad input remains a raw-HID-only feature.
        input->OnUsbReport(d.data(),d.size());
    }

    void init(){
        if(initialized.exchange(true))return;
        const auto modDir=moduleDirectory();
        Log::Init(modDir / "DualSenseHaptics.log");
        Log::Info("DivaSenseHaptics 0.1.0 starting (judgement cleanup, mixed-chord fix, HID output recovery).");
        cfg=ModConfig::Load(modDir / "haptics.ini");
        engine.Configure(cfg);

        chart=std::make_unique<ChartAwareness>();
        chartCapture=std::make_unique<ChartCapture>([this](DscChart parsed){ if(chart) chart->SetChart(std::move(parsed)); });
        chartCapture->Start();
        chartMemoryScan=std::make_unique<ChartMemoryScan>([this](DscChart parsed){ if(chart) chart->SetChart(std::move(parsed)); });
        chartMemoryScan->Start();
        judgement=std::make_unique<JudgementHaptics>(engine,cfg,chart.get());
        judgementHook=std::make_unique<JudgementHook>(*judgement);
        const bool judgementInstalled=cfg.judgement.enabled && judgementHook->Start();
        judgement->SetHookAvailable(judgementInstalled);
        if(judgementInstalled)
            Log::Info("Gameplay haptics are judgement-driven; when a DSC chart is captured, chart TARGET groups determine exact 1/2/3/4-note, slide, Success-note and Challenge state independent of physical key/macros.");
        else if(cfg.judgement.enabled)
            Log::Warn("Judgement hook unavailable; strict validation suppresses gameplay event haptics instead of guessing from raw input.");

        menu=std::make_unique<MenuHaptics>(engine,cfg,judgement.get());
        input=std::make_unique<InputHaptics>(engine,cfg,judgement.get(),
            [this](MenuAction action){ if(menu)menu->Submit(action); });

        if(cfg.controller.enabled){
            hid=std::make_unique<DualSenseHid>(cfg);
            if(!hid->Start([this](const unsigned char* d,size_t n){std::lock_guard lock(inputMutex);if(input)input->OnUsbReport(d,n);}))
                Log::Warn("USB HID input unavailable; music haptics can still work if the audio endpoint is available.");
        }

        output=std::make_unique<DualSenseAudio>(engine,cfg);
        if(!output->Start())
            Log::Warn("DualSense haptic audio endpoint is not ready yet; the output thread will keep retrying for USB reconnect.");

        if(cfg.audio.enabled){
            capture=std::make_unique<ProcessLoopback>(engine);
            if(!capture->Start(GetCurrentProcessId()))Log::Error("Mega Mix+ process audio capture failed to start.");
        }
        std::atexit([]{GetApp().shutdown();});
    }
    void d3dInit(IDXGISwapChain* s,ID3D11Device* d,ID3D11DeviceContext* c){
        if(!initialized)init();
        if(menu)menu->Init(s,d,c);
    }
    void frame(IDXGISwapChain* s){
        pollXInputFallback();
        if(judgement)judgement->Tick();

        const bool gameplay=judgement && judgement->InGameplay();
        const auto now=std::chrono::steady_clock::now();
        if(wasGameplay && !gameplay && chart) {
            // The previous song is over. Drop its timing state so a subsequent
            // chart capture/memory recovery cannot accidentally reuse it.
            chart->ClearChart();
        }
        wasGameplay=gameplay;
        if(gameplay && chart && !chart->HasChart() && chartMemoryScan) {
            if(lastMemoryScanRequest==std::chrono::steady_clock::time_point{} ||
               now-lastMemoryScanRequest>=std::chrono::milliseconds(5000)) {
                chartMemoryScan->Request();
                lastMemoryScanRequest=now;
            }
        }

        if(menu)menu->Tick(s);
    }
    void shutdown(){
        if(!initialized.exchange(false))return;
        if(judgementHook){judgementHook->Stop();judgementHook.reset();}
        if(chartMemoryScan){chartMemoryScan->Stop();chartMemoryScan.reset();}
        if(chartCapture){chartCapture->Stop();chartCapture.reset();}
        if(judgement)judgement->SetHookAvailable(false);
        if(capture){capture->Stop();capture.reset();}
        if(output){output->Stop();output.reset();}
        if(hid){hid->Stop();hid.reset();}
        menu.reset();input.reset();judgement.reset();chart.reset();
        if(xinputModule){FreeLibrary(xinputModule);xinputModule=nullptr;xinputGetState=nullptr;}
        Log::Info("DivaSenseHaptics stopped.");
    }
};
App& GetApp(){static App a;return a;}
}

BOOL WINAPI DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        gModule=module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" {
__declspec(dllexport) void Init(){GetApp().init();}
__declspec(dllexport) void D3DInit(IDXGISwapChain* s,ID3D11Device* d,ID3D11DeviceContext* c){GetApp().d3dInit(s,d,c);}
__declspec(dllexport) void OnFrame(IDXGISwapChain* s){GetApp().frame(s);}
}
#endif
