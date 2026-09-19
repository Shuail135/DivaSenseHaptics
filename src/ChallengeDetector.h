#pragma once
#ifdef _WIN32
#include "Config.h"
#include "HapticEngine.h"
#include <chrono>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>

class JudgementHaptics;

class ChallengeDetector {
public:
    ChallengeDetector(HapticEngine& e,const ModConfig& c,JudgementHaptics* judgement=nullptr)
        :engine_(e),cfg_(c),judgement_(judgement){}
    void Init(IDXGISwapChain* swap,ID3D11Device* device,ID3D11DeviceContext* context);
    void Tick(IDXGISwapChain* swap);
    bool Active()const{return active_;}
private:
    bool ensureStaging(IDXGISwapChain* swap);
    float rowLuma(const unsigned char* p,size_t pitch,unsigned row,unsigned width,DXGI_FORMAT fmt)const;
    void clearOutsideGameplay();

    HapticEngine& engine_;ModConfig cfg_;JudgementHaptics* judgement_=nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    unsigned sampleWidth_=0;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    bool active_=false;
    int enterCount_=0,exitCount_=0;
    std::chrono::steady_clock::time_point lastSample_{};
};
#endif
