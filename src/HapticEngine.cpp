#include "HapticEngine.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
int popcount4(uint8_t x) {
    x &= 0x0f;
    int n=0; for(int i=0;i<4;i++) if(x&(1u<<i)) ++n;
    return n;
}
}

void HapticEngine::Configure(const ModConfig& cfg) {
    cfg_ = cfg;
    audio_.clear();
    filterL_ = {};
    filterR_ = {};
}

float HapticEngine::softClip(float x, float drive) {
    drive = std::max(0.01f, drive);
    const float norm = std::tanh(drive);
    return std::tanh(x * drive) / (norm > 0.0001f ? norm : 1.0f);
}

float HapticEngine::filterSample(float x, FilterState& s, uint32_t sr) {
    const float sampleRate = static_cast<float>(std::max(8000u, sr));
    const float hpHz = std::clamp(cfg_.audio.highPassHz, 1.0f, sampleRate*0.40f);
    const float lpHz = std::clamp(cfg_.audio.lowPassHz, hpHz+1.0f, sampleRate*0.45f);

    const float dt = 1.0f / sampleRate;
    const float rcHp = 1.0f / (2.0f * static_cast<float>(kPi) * hpHz);
    const float aHp = rcHp / (rcHp + dt);
    s.hp = aHp * (s.hp + x - s.prevX);
    s.prevX = x;

    const float rcLp = 1.0f / (2.0f * static_cast<float>(kPi) * lpHz);
    const float aLp = dt / (rcLp + dt);
    s.lp += aLp * (s.hp - s.lp);
    return s.lp;
}

void HapticEngine::PushAudioFloatStereo(const float* p, uint32_t frames, uint32_t sampleRate) {
    if (!cfg_.audio.enabled || !p) return;
    for (uint32_t i=0;i<frames;i++) {
        float l = filterSample(p[i*2], filterL_, sampleRate) * cfg_.audio.musicGain;
        float r = filterSample(p[i*2+1], filterR_, sampleRate) * cfg_.audio.musicGain;
        l = softClip(l, cfg_.audio.softClipDrive);
        r = softClip(r, cfg_.audio.softClipDrive);
        (void)audio_.push(l,r); // overload drops newest audio rather than adding latency
    }
}

void HapticEngine::Trigger(HapticEvent e, float gainScale) {
    const bool menuEvent = e==HapticEvent::MenuUp || e==HapticEvent::MenuDown ||
        e==HapticEvent::MenuLeft || e==HapticEvent::MenuRight || e==HapticEvent::MenuConfirm;
    if (challengeActive_.load(std::memory_order_acquire) && !menuEvent &&
        e!=HapticEvent::ChallengeStart && e!=HapticEvent::ChallengeEnd)
        gainScale *= cfg_.effects.challengeNoteMultiplier;
    std::lock_guard lock(voicesMutex_);
    voices_.push_back({e,0.0,gainScale});
    if (voices_.size() > 64) voices_.erase(voices_.begin(), voices_.begin()+16);
}

void HapticEngine::SetHoldMask(uint8_t mask) { holdMask_.store(mask & 0x0f, std::memory_order_release); }
void HapticEngine::SetChainMask(uint8_t mask) { chainMask_.store(mask & 0x03, std::memory_order_release); }
void HapticEngine::SetChallengeActive(bool active) { challengeActive_.store(active,std::memory_order_release); }

float HapticEngine::envExp(double age, double decay) {
    return static_cast<float>(std::exp(-age * decay));
}
float HapticEngine::sine(double hz, double t) {
    return static_cast<float>(std::sin(2.0*kPi*hz*t));
}
float HapticEngine::chirp(double f0, double f1, double t, double duration) {
    const double k=(f1-f0)/std::max(0.001,duration);
    const double phase=2.0*kPi*(f0*t+0.5*k*t*t);
    return static_cast<float>(std::sin(phase));
}

void HapticEngine::voiceSample(const Voice& v, float& l, float& r) const {
    l = r = 0.0f;
    const double t=v.age;
    float x=0.0f, panL=1.0f, panR=1.0f, g=1.0f;
    double duration=0.10;

    switch(v.event) {
    case HapticEvent::Square:   duration=.055; x=sine(148,t)*envExp(t,58); panL=.72f; panR=1.0f; g=cfg_.effects.singleGain; break;
    case HapticEvent::Cross:    duration=.055; x=sine(164,t)*envExp(t,58); panL=.90f; panR=.90f; g=cfg_.effects.singleGain; break;
    case HapticEvent::Circle:   duration=.055; x=sine(181,t)*envExp(t,60); panL=1.0f; panR=.72f; g=cfg_.effects.singleGain; break;
    case HapticEvent::Triangle: duration=.055; x=sine(198,t)*envExp(t,62); panL=.82f; panR=.82f; g=cfg_.effects.singleGain; break;
    case HapticEvent::NoteGeneric:
        duration=.058; x=(.78f*sine(168,t)+.26f*sine(244,t))*envExp(t,58); panL=.84f; panR=.84f; g=cfg_.effects.singleGain; break;
    case HapticEvent::Multi2:
    case HapticEvent::Multi3:
    case HapticEvent::Multi4: {
        const int n=v.event==HapticEvent::Multi4 ? 4 : (v.event==HapticEvent::Multi3 ? 3 : 2);
        duration=(n==2?.075:(n==3?.085:.095));
        static constexpr double freq[4]={142.0,164.0,188.0,211.0};
        static constexpr float leftPan[4]={1.00f,.82f,.58f,.78f};
        static constexpr float rightPan[4]={.58f,.82f,1.00f,.78f};
        float sl=0.0f,sr=0.0f;
        for(int i=0;i<4;i++) {
            const float tone=sine(freq[i],t); sl+=tone*leftPan[i]; sr+=tone*rightPan[i];
        }
        const float norm=1.0f/std::sqrt(static_cast<float>(n));
        const float bodyGain=n==2?.28f:(n==3?.42f:.58f);
        const float body=bodyGain*sine(n==2?104.0:(n==3?92.0:78.0),t);
        const float punch=(n>=3 ? .18f*sine(n==3?72.0:62.0,t)*envExp(t,24.0) : 0.0f);
        float env=envExp(t,n==2?42.0:(n==3?35.0:30.0));
        const float countBoost = n==2 ? cfg_.effects.multi2Multiplier :
            (n==3 ? cfg_.effects.multi3Multiplier : cfg_.effects.multi4Multiplier);
        g=cfg_.effects.multiGain*countBoost;
        l=((sl*norm+body)*env+punch)*g*v.gain;
        r=((sr*norm+body)*env+punch)*g*v.gain;
        if(t>duration) l=r=0;
        return;
    }
    case HapticEvent::HoldStart:
        duration=.065; x=sine(112,t)*envExp(t,48); g=cfg_.effects.singleGain*.72f; break;
    case HapticEvent::HoldRelease:
        duration=.050; x=sine(225,t)*envExp(t,72); g=cfg_.effects.singleGain*.55f; break;
    case HapticEvent::SlideLeft:
        duration=.070; x=chirp(205,85,t,duration)*envExp(t,34); panL=1.0f; panR=.18f; g=cfg_.effects.slideGain; break;
    case HapticEvent::SlideRight:
        duration=.070; x=chirp(85,205,t,duration)*envExp(t,34); panL=.18f; panR=1.0f; g=cfg_.effects.slideGain; break;
    case HapticEvent::SlideDoubleLeft:
        duration=.085; x=(chirp(230,92,t,duration)+.35f*sine(128,t))*envExp(t,28); panL=1.0f; panR=.28f; g=cfg_.effects.slideGain; break;
    case HapticEvent::SlideDoubleRight:
        duration=.085; x=(chirp(92,230,t,duration)+.35f*sine(128,t))*envExp(t,28); panL=.28f; panR=1.0f; g=cfg_.effects.slideGain; break;
    case HapticEvent::SlideOutward:
        duration=.095;
        if(t<=duration){
            l=chirp(185,86,t,duration)*envExp(t,27)*cfg_.effects.slideGain*v.gain;
            r=chirp(86,185,t,duration)*envExp(t,27)*cfg_.effects.slideGain*v.gain;
        }
        return;
    case HapticEvent::SlideInward:
        duration=.095;
        if(t<=duration){
            l=chirp(86,185,t,duration)*envExp(t,27)*cfg_.effects.slideGain*v.gain;
            r=chirp(185,86,t,duration)*envExp(t,27)*cfg_.effects.slideGain*v.gain;
        }
        return;
    case HapticEvent::SlideMixed:
        duration=.090; x=(.65f*sine(105,t)+.45f*sine(210,t))*envExp(t,30); g=cfg_.effects.slideGain; break;
    case HapticEvent::JudgementCool:
        duration=.052; x=(sine(238,t)+.30f*sine(318,t))*envExp(t,78); g=cfg_.judgement.overlayGain; break;
    case HapticEvent::JudgementFine:
        duration=.062; x=(sine(184,t)+.22f*sine(246,t))*envExp(t,61); g=cfg_.judgement.overlayGain; break;
    case HapticEvent::JudgementSafe:
        duration=.074; x=(sine(128,t)+.18f*sine(164,t))*envExp(t,43); g=cfg_.judgement.overlayGain; break;
    case HapticEvent::JudgementSad:
        duration=.088; x=(sine(91,t)+.22f*sine(67,t))*envExp(t,31); g=cfg_.judgement.overlayGain; break;
    case HapticEvent::JudgementWrong:
        duration=.096; x=(.72f*sine(76,t)+.42f*sine(48,t))*envExp(t,28)*(sine(19,t)>.0f?1.0f:.72f); g=cfg_.judgement.overlayGain; break;
    case HapticEvent::JudgementWorst:
        duration=.105; x=(sine(58,t)+.20f*sine(43,t))*envExp(t,22); g=cfg_.judgement.overlayGain; break;
    case HapticEvent::MenuUp:
        duration=.052; x=chirp(112,224,t,duration)*envExp(t,58); panL=.86f;panR=.86f;g=cfg_.menu.navGain;break;
    case HapticEvent::MenuDown:
        duration=.052; x=chirp(224,112,t,duration)*envExp(t,58); panL=.86f;panR=.86f;g=cfg_.menu.navGain;break;
    case HapticEvent::MenuLeft:
        duration=.050; x=chirp(182,108,t,duration)*envExp(t,62); panL=1.0f;panR=.34f;g=cfg_.menu.navGain;break;
    case HapticEvent::MenuRight:
        duration=.050; x=chirp(108,182,t,duration)*envExp(t,62); panL=.34f;panR=1.0f;g=cfg_.menu.navGain;break;
    case HapticEvent::MenuConfirm:
        duration=.092;
        x=(t<.035 ? sine(188,t)*envExp(t,72) :
          (t>.045 && t<.088 ? .78f*sine(236,t-.045)*envExp(t-.045,68) : 0.0f));
        g=cfg_.menu.confirmGain;break;
    case HapticEvent::SuccessNote:
        duration=.145;
        x=(t<.050 ? (sine(214,t)+.25f*sine(302,t))*envExp(t,58) :
          (t>.070 && t<.140 ? .86f*(sine(278,t-.070)+.20f*sine(352,t-.070))*envExp(t-.070,52) : 0.0f));
        panL=.92f; panR=.92f; g=cfg_.effects.successNoteGain; break;
    case HapticEvent::ChallengeStart:
        duration=.230;
        x=(t<.075 ? sine(118,t)*envExp(t,36) :
          (t>.115 && t<.205 ? sine(172,t-.115)*envExp(t-.115,32) : 0.0f));
        g=cfg_.effects.challengeGain; break;
    case HapticEvent::ChallengeEnd:
        duration=.220;
        x=(t<.070 ? sine(190,t)*envExp(t,44) :
          (t>.100 && t<.185 ? sine(108,t-.100)*envExp(t-.100,36) : 0.0f));
        g=cfg_.effects.challengeGain*.78f; break;
    }

    if (t>duration) return;
    l=x*panL*g*v.gain;
    r=x*panR*g*v.gain;
}

void HapticEngine::RenderBlock(float* outLR, uint32_t frames, uint32_t outputSampleRate) {
    if (!outLR || frames==0) return;
    const uint32_t sr=std::max(8000u,outputSampleRate);

    const size_t maxBuf = static_cast<size_t>(sr) * std::max(10,cfg_.audio.maxBufferedMs) / 1000;
    const size_t target = static_cast<size_t>(sr) * std::max(1,cfg_.audio.targetBufferedMs) / 1000;
    auto avail=audio_.available();
    if (avail>maxBuf && avail>target) audio_.discard(avail-target);

    std::lock_guard voiceLock(voicesMutex_);
    for(uint32_t i=0;i<frames;i++) {
        float l=0, r=0;
        StereoRing<131072>::Frame af{};
        if (audio_.pop(af)) { l+=af.l; r+=af.r; }

        for (auto& v: voices_) {
            float vl=0,vr=0;
            voiceSample(v,vl,vr);
            l+=vl; r+=vr;
            v.age += 1.0/static_cast<double>(sr);
        }

        uint8_t hold=holdMask_.load(std::memory_order_acquire);
        if (hold) {
            int n=popcount4(hold);
            float amp=cfg_.effects.holdGain*(0.72f+0.13f*n);
            if(challengeActive_.load(std::memory_order_acquire)) amp*=cfg_.effects.challengeNoteMultiplier;
            const float trem=0.60f+0.40f*sine(7.0,holdPhase_);
            static constexpr float lp[4]={1.00f,.84f,.62f,.78f};
            static constexpr float rp[4]={.62f,.84f,1.00f,.78f};
            float pl=0,pr=0; for(int b=0;b<4;b++) if(hold&(1u<<b)){pl+=lp[b];pr+=rp[b];}
            pl/=std::max(1,n);pr/=std::max(1,n);
            const float texture=(.65f*sine(82.0+7*n,holdPhase_)+.35f*sine(151.0+3*n,holdPhase_))*amp*trem;
            l+=texture*pl; r+=texture*pr;
        }
        holdPhase_ += 1.0/static_cast<double>(sr);

        uint8_t chain=chainMask_.load(std::memory_order_acquire);
        if (chain) {
            const double chainHz=std::clamp(static_cast<double>(cfg_.effects.chainFrequencyHz),4.0,40.0);
            const float gate = sine(chainHz,chainPhase_) > 0.1f ? 1.0f : 0.30f;
            float chainGain=cfg_.effects.chainGain;
            if(challengeActive_.load(std::memory_order_acquire)) chainGain*=cfg_.effects.challengeNoteMultiplier;
            const float texture=(.72f*sine(103.0,chainPhase_)+.28f*sine(206.0,chainPhase_))*chainGain*gate;
            if (chain&1) l+=texture;
            if (chain&2) r+=texture;
            if (chain==3) { l+=texture*.25f; r+=texture*.25f; }
        }
        chainPhase_ += 1.0/static_cast<double>(sr);

        l=softClip(l*cfg_.audio.masterGain,1.15f);
        r=softClip(r*cfg_.audio.masterGain,1.15f);
        outLR[i*2]=std::clamp(l,-1.0f,1.0f);
        outLR[i*2+1]=std::clamp(r,-1.0f,1.0f);
    }
    voices_.erase(std::remove_if(voices_.begin(),voices_.end(),
        [](const Voice& v){ return v.age>0.30; }),voices_.end());
}
