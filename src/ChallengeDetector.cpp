#ifdef _WIN32
#include "ChallengeDetector.h"
#include "Log.h"
#include "JudgementHaptics.h"
#include <algorithm>

void ChallengeDetector::Init(IDXGISwapChain*,ID3D11Device* d,ID3D11DeviceContext* c){
    device_=d;context_=c;lastSample_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
}
bool ChallengeDetector::ensureStaging(IDXGISwapChain* swap){
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back))))return false;
    D3D11_TEXTURE2D_DESC bd{};back->GetDesc(&bd);
    unsigned w=std::min(320u,std::max(32u,bd.Width/3));
    if(staging_&&sampleWidth_==w&&format_==bd.Format)return true;
    if(bd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&bd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&
       bd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&bd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return false;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width=w;sd.Height=3;sd.MipLevels=1;sd.ArraySize=1;sd.Format=bd.Format;
    sd.SampleDesc.Count=1;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    staging_.Reset();
    if(FAILED(device_->CreateTexture2D(&sd,nullptr,&staging_)))return false;
    sampleWidth_=w;format_=bd.Format;return true;
}
float ChallengeDetector::rowLuma(const unsigned char* p,size_t pitch,unsigned row,unsigned width,DXGI_FORMAT fmt)const{
    const auto* q=p+pitch*row;double sum=0;
    bool bgra=fmt==DXGI_FORMAT_B8G8R8A8_UNORM||fmt==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for(unsigned x=0;x<width;x++){
        unsigned char r=q[x*4+(bgra?2:0)],g=q[x*4+1],b=q[x*4+(bgra?0:2)];
        sum+=(0.2126*r+0.7152*g+0.0722*b)/255.0;
    }
    return static_cast<float>(sum/std::max(1u,width));
}

void ChallengeDetector::clearOutsideGameplay(){
    enterCount_=exitCount_=0;
    if(active_){
        active_=false;
        engine_.SetChallengeActive(false);
        // Do not play ChallengeEnd here: leaving gameplay/loading is not a
        // Challenge Time completion event.
    }
}
void ChallengeDetector::Tick(IDXGISwapChain* swap){
    if(!cfg_.challenge.enabled||!cfg_.challenge.visualDetection||!device_||!context_||!swap)return;
    if(judgement_ && !judgement_->InGameplay()){
        clearOutsideGameplay();
        return;
    }
    auto now=std::chrono::steady_clock::now();
    if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastSample_).count()<cfg_.challenge.sampleIntervalMs)return;
    lastSample_=now;
    if(!ensureStaging(swap))return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back))))return;
    D3D11_TEXTURE2D_DESC bd{};back->GetDesc(&bd);
    const unsigned x=(bd.Width-sampleWidth_)/2;
    const unsigned ys[3]={std::max(1u,bd.Height*6/100),bd.Height/2,std::min(bd.Height-2,bd.Height*94/100)};
    for(unsigned row=0;row<3;row++){
        D3D11_BOX box{x,ys[row],0,x+sampleWidth_,ys[row]+1,1};
        context_->CopySubresourceRegion(staging_.Get(),0,0,row,0,back.Get(),0,&box);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if(FAILED(context_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&m)))return;
    float top=rowLuma(reinterpret_cast<const unsigned char*>(m.pData),m.RowPitch,0,sampleWidth_,format_);
    float mid=rowLuma(reinterpret_cast<const unsigned char*>(m.pData),m.RowPitch,1,sampleWidth_,format_);
    float bot=rowLuma(reinterpret_cast<const unsigned char*>(m.pData),m.RowPitch,2,sampleWidth_,format_);
    context_->Unmap(staging_.Get(),0);

    float band=(top+bot)*0.5f;
    bool looksLikeChallenge=top<cfg_.challenge.darkThreshold&&bot<cfg_.challenge.darkThreshold&&
        mid>cfg_.challenge.centerMinimum&&(mid-band)>cfg_.challenge.contrastMinimum;

    if(!active_){
        enterCount_=looksLikeChallenge?enterCount_+1:0;
        if(enterCount_>=cfg_.challenge.enterSamples){
            active_=true;enterCount_=0;exitCount_=0;
            engine_.SetChallengeActive(true);
            engine_.Trigger(HapticEvent::ChallengeStart);
            Log::Info("Challenge Time visual state detected: START.");
        }
    }else{
        exitCount_=looksLikeChallenge?0:exitCount_+1;
        if(exitCount_>=cfg_.challenge.exitSamples){
            active_=false;exitCount_=0;enterCount_=0;
            engine_.SetChallengeActive(false);
            engine_.Trigger(HapticEvent::ChallengeEnd);
            Log::Info("Challenge Time visual state detected: END.");
        }
    }
}
#endif
