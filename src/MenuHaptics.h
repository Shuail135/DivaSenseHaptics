#pragma once
#ifdef _WIN32

#include "Config.h"
#include "HapticEngine.h"
#include "InputHaptics.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>

class JudgementHaptics;

class MenuHaptics {
public:
    MenuHaptics(HapticEngine& engine,const ModConfig& cfg,JudgementHaptics* judgement)
        :engine_(engine),cfg_(cfg),judgement_(judgement){}

    void Init(IDXGISwapChain* swap,ID3D11Device* device,ID3D11DeviceContext* context);
    void Submit(MenuAction action);
    void Tick(IDXGISwapChain* swap);
    void Reset();

private:
    using Clock=std::chrono::steady_clock;
    using TP=Clock::time_point;

    struct Pending {
        MenuAction action=MenuAction::Down;
        TP when{};
        uint64_t generation=0;
        float baselineMotion=0.0f;
        bool settled=false;
    };

    bool ensureStaging(IDXGISwapChain* swap);
    bool captureSignature(IDXGISwapChain* swap,std::vector<float>& out);
    float signatureDelta(const std::vector<float>& a,const std::vector<float>& b)const;
    HapticEvent eventFor(MenuAction action)const;
    float minimumDelta(MenuAction action)const;
    const char* actionName(MenuAction action)const;

    HapticEngine& engine_;
    ModConfig cfg_;
    JudgementHaptics* judgement_=nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    unsigned sampleWidth_=0;
    static constexpr unsigned kRows=12;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;

    std::mutex mutex_;
    std::deque<Pending> pending_;
    uint64_t generation_=0;
    float motionEma_=0.0f;
    TP stableSince_{};

    std::vector<float> previous_;
    TP lastSample_{};
};

#endif
