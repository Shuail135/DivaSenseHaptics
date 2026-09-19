#ifdef _WIN32
#include "MenuHaptics.h"
#include "JudgementHaptics.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <string>

void MenuHaptics::Init(IDXGISwapChain*,ID3D11Device* d,ID3D11DeviceContext* c){
    device_=d;context_=c;
    lastSample_=Clock::now()-std::chrono::seconds(1);
    stableSince_={};
}

void MenuHaptics::Reset(){
    std::lock_guard lock(mutex_);
    pending_.clear();
    previous_.clear();
    generation_=0;
    motionEma_=0.0f;
    stableSince_={};
}

bool MenuHaptics::ensureStaging(IDXGISwapChain* swap){
    if(!device_||!swap)return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back))))return false;
    D3D11_TEXTURE2D_DESC bd{};back->GetDesc(&bd);
    if(bd.Width<32||bd.Height<32)return false;
    const unsigned w=std::min(bd.Width,std::min(256u,std::max(96u,bd.Width*3/4)));
    if(staging_&&sampleWidth_==w&&format_==bd.Format)return true;
    if(bd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&bd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&
       bd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&bd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return false;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width=w;sd.Height=kRows;sd.MipLevels=1;sd.ArraySize=1;sd.Format=bd.Format;
    sd.SampleDesc.Count=1;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    staging_.Reset();
    if(FAILED(device_->CreateTexture2D(&sd,nullptr,&staging_)))return false;
    sampleWidth_=w;format_=bd.Format;
    previous_.clear();
    return true;
}

bool MenuHaptics::captureSignature(IDXGISwapChain* swap,std::vector<float>& out){
    if(!ensureStaging(swap)||!context_)return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back))))return false;
    D3D11_TEXTURE2D_DESC bd{};back->GetDesc(&bd);
    const unsigned x=(bd.Width-sampleWidth_)/2;
    for(unsigned row=0;row<kRows;row++){
        unsigned y=static_cast<unsigned>((static_cast<uint64_t>(row+1)*bd.Height)/(kRows+1));
        y=std::min(std::max(1u,y),bd.Height-2);
        D3D11_BOX box{x,y,0,x+sampleWidth_,y+1,1};
        context_->CopySubresourceRegion(staging_.Get(),0,0,row,0,back.Get(),0,&box);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if(FAILED(context_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&m)))return false;
    out.resize(static_cast<size_t>(sampleWidth_)*kRows);
    const auto* p=reinterpret_cast<const unsigned char*>(m.pData);
    const bool bgra=format_==DXGI_FORMAT_B8G8R8A8_UNORM||format_==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for(unsigned row=0;row<kRows;row++){
        const auto* q=p+m.RowPitch*row;
        for(unsigned xx=0;xx<sampleWidth_;xx++){
            const unsigned char r=q[xx*4+(bgra?2:0)];
            const unsigned char g=q[xx*4+1];
            const unsigned char b=q[xx*4+(bgra?0:2)];
            out[static_cast<size_t>(row)*sampleWidth_+xx]=
                static_cast<float>((0.2126*r+0.7152*g+0.0722*b)/255.0);
        }
    }
    context_->Unmap(staging_.Get(),0);
    return true;
}

float MenuHaptics::signatureDelta(const std::vector<float>& a,const std::vector<float>& b)const{
    if(a.size()!=b.size()||a.empty())return 0.0f;
    double sum=0.0;
    size_t changed=0;
    float peak=0.0f;
    for(size_t i=0;i<a.size();++i){
        const float d=std::abs(a[i]-b[i]);
        sum+=d;peak=std::max(peak,d);
        if(d>0.035f)++changed;
    }
    const float mean=static_cast<float>(sum/a.size());
    const float fraction=static_cast<float>(changed)/static_cast<float>(a.size());
    // A selection highlight may touch only a modest part of the sampled rows,
    // so combine global mean with changed-pixel fraction and peak response.
    return mean + fraction*0.070f + peak*0.020f;
}

HapticEvent MenuHaptics::eventFor(MenuAction action)const{
    switch(action){
    case MenuAction::Up:return HapticEvent::MenuUp;
    case MenuAction::Down:return HapticEvent::MenuDown;
    case MenuAction::Left:return HapticEvent::MenuLeft;
    case MenuAction::Right:return HapticEvent::MenuRight;
    case MenuAction::Confirm:return HapticEvent::MenuConfirm;
    }
    return HapticEvent::MenuConfirm;
}

float MenuHaptics::minimumDelta(MenuAction action)const{
    return action==MenuAction::Confirm?cfg_.menu.confirmMinDelta:cfg_.menu.navMinDelta;
}

const char* MenuHaptics::actionName(MenuAction action)const{
    switch(action){
    case MenuAction::Up:return "UP";
    case MenuAction::Down:return "DOWN";
    case MenuAction::Left:return "LEFT";
    case MenuAction::Right:return "RIGHT";
    case MenuAction::Confirm:return "CONFIRM";
    }
    return "UNKNOWN";
}

void MenuHaptics::Submit(MenuAction action){
    if(!cfg_.menu.enabled)return;
    if(judgement_ && (!judgement_->HookAvailable() || judgement_->InGameplay()))return;
    const auto now=Clock::now();
    std::lock_guard lock(mutex_);
    bool settled=stableSince_!=TP{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now-stableSince_).count()>=cfg_.menu.settleMs;
    pending_.push_back({action,now,generation_,motionEma_,settled});
    while(pending_.size()>6)pending_.pop_front();
}

void MenuHaptics::Tick(IDXGISwapChain* swap){
    if(!cfg_.menu.enabled||!device_||!context_||!swap)return;
    if(judgement_ && judgement_->InGameplay()){
        std::lock_guard lock(mutex_);
        pending_.clear();previous_.clear();motionEma_=0.0f;stableSince_={};
        return;
    }

    const auto now=Clock::now();
    if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastSample_).count()<cfg_.menu.sampleIntervalMs)return;
    lastSample_=now;

    std::vector<float> current;
    if(!captureSignature(swap,current))return;
    if(previous_.empty()){
        previous_=std::move(current);
        stableSince_=now;
        return;
    }

    const float delta=signatureDelta(previous_,current);
    previous_=std::move(current);

    MenuAction acceptedAction=MenuAction::Down;
    bool accepted=false;
    bool logSuppressed=false;
    MenuAction suppressedAction=MenuAction::Down;
    float thresholdForLog=0.0f;

    {
        std::lock_guard lock(mutex_);
        ++generation_;

        if(delta<=cfg_.menu.settleMaxDelta){
            if(stableSince_==TP{})stableSince_=now;
        }else{
            stableSince_={};
        }

        while(!pending_.empty()){
            auto& p=pending_.front();
            const auto age=std::chrono::duration_cast<std::chrono::milliseconds>(now-p.when).count();
            if(age>cfg_.menu.validationWindowMs){
                if(cfg_.menu.logEvents){logSuppressed=true;suppressedAction=p.action;}
                pending_.pop_front();
                continue;
            }
            if(generation_<=p.generation)break;

            // Loading/transition scenes generally have a high moving baseline.
            // A valid menu action must come from a settled screen and then cause
            // a change clearly above that pre-input baseline.
            const float threshold=std::max(minimumDelta(p.action),
                p.baselineMotion*cfg_.menu.motionFactor+cfg_.menu.motionMargin);
            thresholdForLog=threshold;
            if(p.settled && p.baselineMotion<=cfg_.menu.maxBaselineMotion && delta>=threshold){
                acceptedAction=p.action;accepted=true;
                pending_.pop_front();
            }
            break;
        }

        // Do not teach the baseline a likely menu response at full strength.
        const float alpha=pending_.empty()?0.14f:0.035f;
        motionEma_=(generation_<=1)?delta:(motionEma_*(1.0f-alpha)+delta*alpha);
    }

    if(accepted){
        engine_.Trigger(eventFor(acceptedAction));
        if(cfg_.menu.logEvents)
            Log::Info(std::string("Validated menu action: ")+actionName(acceptedAction)+
                      " frame_delta="+std::to_string(delta)+
                      " threshold="+std::to_string(thresholdForLog));
    }else if(logSuppressed){
        Log::Info(std::string("Suppressed unconfirmed menu action: ")+actionName(suppressedAction));
    }
}

#endif
