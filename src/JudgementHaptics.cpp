#include "JudgementHaptics.h"
#include "ChartAwareness.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

JudgementHaptics::JudgementHaptics(HapticEngine& engine, const ModConfig& cfg, ChartAwareness* chart)
    : engine_(engine), cfg_(cfg), chart_(chart) {}

int JudgementHaptics::bitCount(uint8_t v) {
    int n=0; v &= 0x0f; while(v){ n += (v & 1u); v >>= 1; } return n;
}

bool JudgementHaptics::gradeAllowsSustain(Grade g) {
    return g==Grade::Cool || g==Grade::Fine || g==Grade::Safe || g==Grade::Sad;
}

uint8_t JudgementHaptics::slideSidesForEvent(HapticEvent e) {
    switch(e) {
    case HapticEvent::SlideLeft:
    case HapticEvent::SlideDoubleLeft: return 0x01;
    case HapticEvent::SlideRight:
    case HapticEvent::SlideDoubleRight: return 0x02;
    case HapticEvent::SlideOutward:
    case HapticEvent::SlideInward:
    case HapticEvent::SlideMixed: return 0x03;
    default: return 0;
    }
}

JudgementHaptics::Decoded JudgementHaptics::Decode(int32_t raw, int reportedMultiCount) {
    Decoded d{};
    if(raw == 21) return d; // HitState_None

    // score-mm's published Mega Mix+ hook documents the game's returned enum as:
    //   0 COOL, 1 FINE, 2 SAFE, 3 Bad/SAD, 4..7 WRONG variants, 8 MISS/WORST.
    // The public reference names one output argument multiCount. Retail testing
    // shows it can remain 1 on real chords, so Decode preserves it only as a hint;
    // final chord size is inferred later from callback grouping + matched input.
    if(raw >= 0 && raw <= 3) {
        d.grade = static_cast<Grade>(raw);
        d.multiCount = std::max(1, reportedMultiCount);
        d.valid = true;
    } else if(raw >= 4 && raw <= 7) {
        d.grade = Grade::Wrong;
        d.multiCount = std::max(1, reportedMultiCount);
        d.valid = true;
    } else if(raw == 8) {
        d.grade = Grade::Worst;
        d.multiCount = std::max(1, reportedMultiCount);
        d.valid = true;
    }
    return d;
}

const char* JudgementHaptics::GradeName(Grade g) {
    switch(g) {
    case Grade::Cool: return "COOL";
    case Grade::Fine: return "FINE";
    case Grade::Safe: return "SAFE";
    case Grade::Sad: return "SAD";
    case Grade::Wrong: return "WRONG";
    case Grade::Worst: return "WORST";
    case Grade::None: return "NONE";
    }
    return "NONE";
}

float JudgementHaptics::gradeGain(Grade g) const {
    switch(g) {
    case Grade::Cool: return cfg_.judgement.coolGain;
    case Grade::Fine: return cfg_.judgement.fineGain;
    case Grade::Safe: return cfg_.judgement.safeGain;
    case Grade::Sad: return cfg_.judgement.sadGain;
    case Grade::Wrong: return cfg_.judgement.wrongGain;
    case Grade::Worst: return cfg_.judgement.worstGain;
    case Grade::None: return 0.0f;
    }
    return 0.0f;
}

HapticEvent JudgementHaptics::gradeEvent(Grade g) const {
    switch(g) {
    case Grade::Cool: return HapticEvent::JudgementCool;
    case Grade::Fine: return HapticEvent::JudgementFine;
    case Grade::Safe: return HapticEvent::JudgementSafe;
    case Grade::Sad: return HapticEvent::JudgementSad;
    case Grade::Wrong: return HapticEvent::JudgementWrong;
    case Grade::Worst: return HapticEvent::JudgementWorst;
    case Grade::None: return HapticEvent::JudgementWorst;
    }
    return HapticEvent::JudgementWorst;
}

int JudgementHaptics::pendingCount(const Pending& p) const {
    switch(p.event) {
    case HapticEvent::Multi2: return 2;
    case HapticEvent::Multi3: return 3;
    case HapticEvent::Multi4: return 4;
    default: {
        int n=bitCount(p.detail);
        return n > 0 ? n : 1;
    }
    }
}

bool JudgementHaptics::inGameplayLocked(TP now) const {
    if(!hookAvailable_ || lastGamePoll_==TP{}) return false;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-lastGamePoll_).count();
    return ms<=cfg_.judgement.gameplayPollTimeoutMs;
}

void JudgementHaptics::purgeLocked(TP now) {
    const auto keepMs = std::max(cfg_.judgement.matchWindowMs, cfg_.judgement.pendingTimeoutMs);
    while(!pending_.empty() &&
          std::chrono::duration_cast<std::chrono::milliseconds>(now-pending_.front().when).count() > keepMs)
        pending_.pop_front();
    while(!orphan_.empty() &&
          std::chrono::duration_cast<std::chrono::milliseconds>(now-orphan_.front().when).count() > cfg_.judgement.matchWindowMs)
        orphan_.pop_front();
    while(pending_.size()>16) pending_.pop_front();
    while(orphan_.size()>16) orphan_.pop_front();
    const int targetKeepMs = std::max(50, cfg_.judgement.targetCorrelationWindowMs * 4);
    while(!targetHits_.empty() &&
          std::chrono::duration_cast<std::chrono::milliseconds>(now-targetHits_.front().when).count() > targetKeepMs)
        targetHits_.pop_front();
    while(targetHits_.size()>64) targetHits_.pop_front();

    const int faceKeepMs = std::max(80, cfg_.judgement.physicalMultiWindowMs * 4);
    while(!faceSamples_.empty() &&
          std::chrono::duration_cast<std::chrono::milliseconds>(now-faceSamples_.front().when).count() > faceKeepMs)
        faceSamples_.pop_front();
    while(faceSamples_.size()>96) faceSamples_.pop_front();
}

int JudgementHaptics::correlatedTargetCountLocked(const ConfirmBurst& burst) const {
    if(!burst.active) return 0;
    const int window = std::clamp(cfg_.judgement.targetCorrelationWindowMs, 1, 30);
    const TP begin = burst.first - std::chrono::milliseconds(window);
    const TP end = burst.last + std::chrono::milliseconds(window);
    std::array<void*, 8> unique{};
    int uniqueCount = 0;
    int matchingGrade = 0;
    for(const auto& h : targetHits_) {
        if(h.when < begin || h.when > end || !h.target || h.raw < 0 || h.raw > 8) continue;
        bool seen=false;
        for(int i=0;i<uniqueCount;++i) if(unique[static_cast<size_t>(i)]==h.target) { seen=true; break; }
        if(seen) continue;
        if(uniqueCount < static_cast<int>(unique.size())) unique[static_cast<size_t>(uniqueCount++)]=h.target;
        const auto hd=Decode(h.raw,1);
        if(hd.valid && hd.grade==burst.decoded.grade) ++matchingGrade;
    }
    if(matchingGrade>=2 && matchingGrade<=4) return matchingGrade;
    if(uniqueCount>=2 && uniqueCount<=4) return uniqueCount;
    return 0;
}

int JudgementHaptics::physicalChordCountLocked(const ConfirmBurst& burst, uint8_t& maskOut) const {
    maskOut = 0;
    if(!burst.active) return 0;

    // The game and our independent HID reader run on different threads. A confirmed
    // judgement can therefore arrive a few milliseconds before or after the report
    // containing the full simultaneous face-button mask. Search a short symmetric
    // history window and keep the mask with the highest cardinality.
    const int window = std::clamp(cfg_.judgement.physicalMultiWindowMs, 4, 50);
    const TP begin = burst.first - std::chrono::milliseconds(window);
    const TP end = burst.last + std::chrono::milliseconds(window);
    int bestCount = 0;
    TP bestWhen{};
    for(const auto& sample : faceSamples_) {
        if(sample.when < begin || sample.when > end) continue;
        const uint8_t m = static_cast<uint8_t>(sample.mask & 0x0f);
        const int n = bitCount(m);
        if(n > bestCount || (n == bestCount && n > 0 && sample.when > bestWhen)) {
            bestCount = n;
            maskOut = m;
            bestWhen = sample.when;
        }
    }
    return std::clamp(bestCount, 0, 4);
}

int JudgementHaptics::findPendingLocked(const Decoded& d, bool slide, TP now) const {
    int best=-1;
    long long bestScore=std::numeric_limits<long long>::max();
    for(size_t i=0;i<pending_.size();++i) {
        const auto age=std::llabs(std::chrono::duration_cast<std::chrono::milliseconds>(now-pending_[i].when).count());
        if(age>cfg_.judgement.matchWindowMs) continue;
        long long score=age;
        if(pending_[i].slide != slide) score += 70;
        // The public score-mm reference names this argument multiCount, but on
        // retail Mega Mix+ it can remain 1 for real chords. Only use it as a
        // matching hint when it is actually >1.
        if(d.multiCount>1 && pendingCount(pending_[i])!=d.multiCount) score += 45;
        if(score<bestScore){bestScore=score;best=static_cast<int>(i);}
    }
    return best;
}

int JudgementHaptics::findOrphanLocked(const Pending& p, TP now) const {
    int best=-1;
    long long bestScore=std::numeric_limits<long long>::max();
    for(size_t i=0;i<orphan_.size();++i) {
        const auto age=std::llabs(std::chrono::duration_cast<std::chrono::milliseconds>(now-orphan_[i].when).count());
        if(age>cfg_.judgement.matchWindowMs) continue;
        long long score=age;
        if(orphan_[i].slide != p.slide) score += 70;
        if(orphan_[i].decoded.multiCount>1 && pendingCount(p)!=orphan_[i].decoded.multiCount) score += 45;
        if(score<bestScore){bestScore=score;best=static_cast<int>(i);}
    }
    return best;
}

void JudgementHaptics::playGradeOverlay(Grade grade) {
    const float g=gradeGain(grade);
    if(g<=0.0f || cfg_.judgement.overlayGain<=0.0f) return;
    engine_.Trigger(gradeEvent(grade), g);
}

HapticEvent JudgementHaptics::eventForGameResult(const Pending* p, const Decoded& d, bool gameSlide) {
    // DIVA is authoritative. A shoulder/key/macro can physically look like one
    // thing while the in-game key configuration resolves it to another.
    if(gameSlide) {
        if(p && p->slide) return p->event; // preserve known left/right direction
        return HapticEvent::SlideMixed;    // mapped key/keyboard: confirmed slide, unknown direction
    }
    if(d.multiCount>=4) return HapticEvent::Multi4;
    if(d.multiCount==3) return HapticEvent::Multi3;
    if(d.multiCount==2) return HapticEvent::Multi2;
    if(p && !p->slide) {
        switch(p->event) {
        case HapticEvent::Square: case HapticEvent::Cross:
        case HapticEvent::Circle: case HapticEvent::Triangle:
        case HapticEvent::NoteGeneric: return p->event;
        default: break;
        }
    }
    return HapticEvent::NoteGeneric;
}

void JudgementHaptics::playConfirmed(const Pending* p, const Decoded& d, bool gameSlide, bool overlay) {
    const float g=std::clamp(gradeGain(d.grade),0.0f,2.0f);
    if(g>0.0f) {
        const HapticEvent e=eventForGameResult(p,d,gameSlide);
        const uint8_t detail=p ? p->detail : 0;
        engine_.Trigger(e,g,detail);
    }
    if(overlay) playGradeOverlay(d.grade);
}


bool JudgementHaptics::takeBurstLocked(TP now, bool force, ConfirmBurst& out) {
    if(!burst_.active) return false;
    const auto age=std::chrono::duration_cast<std::chrono::milliseconds>(now-burst_.last).count();
    // Keep confirmation latency low. Physical chord recovery looks mostly backward
    // through the timestamped HID history, so it does not need an extra 12-24 ms
    // delay; the existing same-frame grouping window is enough.
    const int window=std::clamp(cfg_.judgement.multiGroupWindowMs,1,30);
    if(!force && age<window) return false;
    out=burst_;
    burst_={};
    return true;
}

void JudgementHaptics::emitBurst(const ConfirmBurst& b) {
    if(!b.active || !b.decoded.valid) return;
    Decoded d=b.decoded;

    ChartJudgementMatch chartMatch{};
    if(chart_) chartMatch=chart_->ObserveJudgement(b.first,false,b.successNote);
    if(chartMatch.matched && cfg_.challenge.enabled) {
        engine_.SetChallengeActive(chartMatch.challengeActive);
        if(chartMatch.challengeTransition) {
            engine_.Trigger(chartMatch.challengeActive ? HapticEvent::ChallengeStart : HapticEvent::ChallengeEnd);
            Log::Info(std::string("DSC Challenge Time: ") + (chartMatch.challengeActive ? "START" : "END") + ".");
        }
    }

    int correlatedTargets=0;
    int physicalCount=0;
    uint8_t physicalMask=0;
    {
        std::lock_guard lock(mutex_);
        correlatedTargets=correlatedTargetCountLocked(b);
        physicalCount=physicalChordCountLocked(b,physicalMask);
    }

    const int gameTargetCount=std::max(b.directTargetCount,correlatedTargets);
    const int chartCount=chartMatch.matched ? bitCount(chartMatch.group.faceMask) : 0;
    if(chartCount>=1 && chartCount<=4)
        d.multiCount=chartCount;
    else if(physicalCount>=2 && physicalCount<=4)
        d.multiCount=physicalCount;
    else if(gameTargetCount>=2 && gameTargetCount<=4)
        d.multiCount=gameTargetCount;
    else
        d.multiCount=std::clamp(std::max({1,b.callbackCount,b.hintedCount,b.reportedMax}),1,4);

    Pending shaped=b.pending;
    bool haveShaped=b.havePending;
    if(chartMatch.matched && chartMatch.group.faceMask) {
        const uint8_t mask=static_cast<uint8_t>(chartMatch.group.faceMask&0x0f);
        if(!haveShaped){shaped.when=b.first;shaped.slide=false;haveShaped=true;}
        shaped.detail=mask;
        switch(d.multiCount){
        case 4: shaped.event=HapticEvent::Multi4; break;
        case 3: shaped.event=HapticEvent::Multi3; break;
        case 2: shaped.event=HapticEvent::Multi2; break;
        default:
            if(mask==0x01) shaped.event=HapticEvent::Square;
            else if(mask==0x02) shaped.event=HapticEvent::Cross;
            else if(mask==0x04) shaped.event=HapticEvent::Circle;
            else if(mask==0x08) shaped.event=HapticEvent::Triangle;
            else shaped.event=HapticEvent::NoteGeneric;
            break;
        }
    } else if(physicalCount>=2 && physicalMask) {
        if(!haveShaped){shaped.event=HapticEvent::NoteGeneric;shaped.slide=false;shaped.when=b.first;haveShaped=true;}
        shaped.detail=physicalMask;
    }

    playGradeOverlay(d.grade);
    if(d.grade!=Grade::Worst)
        playConfirmed(haveShaped ? &shaped : nullptr,d,false,false);

    const bool chartSuccess=chartMatch.matched && chartMatch.group.specialFaceMask!=0;
    if((b.successNote||chartSuccess) && gradeAllowsSustain(d.grade))
        engine_.Trigger(HapticEvent::SuccessNote,gradeGain(d.grade));

    if(cfg_.judgement.logEvents) {
        std::string source="single";
        if(chartCount>0) source="chart";
        else if(physicalCount>1) source="physical-window";
        else if(gameTargetCount>1) source=(correlatedTargets>1 ? "game-target-window" : "game-targets");
        else if(b.callbackCount>1) source="game-group";
        if(b.hintedCount>1 && source!="chart" && source!="physical-window") source += (source=="single" ? "input" : "+input");
        if(b.reportedMax>1 && source!="chart") source += (source=="single" ? "reported" : "+reported");
        char physicalHex[8]{}; std::snprintf(physicalHex,sizeof(physicalHex),"0x%X",static_cast<unsigned>(physicalMask));
        char chartHex[8]{}; std::snprintf(chartHex,sizeof(chartHex),"0x%X",static_cast<unsigned>(chartMatch.matched?chartMatch.group.faceMask:0));
        Log::Info(std::string("DIVA confirmed: ") + GradeName(d.grade) +
                  " multi=" + std::to_string(d.multiCount) +
                  " chart_multi=" + std::to_string(chartCount) +
                  " chart_mask=" + chartHex +
                  " chart_group=" + std::to_string(chartMatch.groupIndex) +
                  " chart_ms=" + std::to_string(chartMatch.timingErrorMs) +
                  " game_targets=" + std::to_string(gameTargetCount) +
                  " physical_multi=" + std::to_string(physicalCount) +
                  " physical_mask=" + physicalHex +
                  " callbacks=" + std::to_string(b.callbackCount) +
                  " input_hint=" + std::to_string(b.hintedCount) +
                  " reported=" + std::to_string(b.reportedMax) +
                  " source=" + source +
                  ((b.successNote||chartSuccess) ? " success_note=1" : " success_note=0") +
                  (chartMatch.matched && chartMatch.challengeActive ? " challenge=1" : " challenge=0"));
    }
}

void JudgementHaptics::Submit(HapticEvent event, uint8_t detailMask, bool slide) {
    if(!cfg_.judgement.enabled) {
        engine_.Trigger(event,1.0f,detailMask);
        return;
    }

    const auto now=Clock::now();
    {
        std::lock_guard lock(mutex_);
        if(!hookAvailable_) {
            if(!cfg_.judgement.strictValidation)
                engine_.Trigger(event,1.0f,detailMask);
            return;
        }
        // Menus/loading are handled by the separate UI-response validator.
        if(!inGameplayLocked(now)) return;
    }

    if(cfg_.judgement.pressPreviewGain>0.0f)
        engine_.Trigger(event,cfg_.judgement.pressPreviewGain,detailMask);

    Pending p{event,static_cast<uint8_t>(detailMask&0x0f),slide,now};
    Decoded paired{};
    bool havePair=false;
    {
        std::lock_guard lock(mutex_);
        purgeLocked(now);
        const int oi=findOrphanLocked(p,now);
        if(oi>=0) {
            paired=orphan_[static_cast<size_t>(oi)].decoded;
            orphan_.erase(orphan_.begin()+oi);
            havePair=true;
        } else {
            pending_.push_back(p);
        }
    }
    if(havePair) {
        playConfirmed(&p,paired,p.slide,false);
        if(gradeAllowsSustain(paired.grade) && !p.slide && p.detail) {
            std::lock_guard lock(mutex_);
            holdEligibleMask_ |= static_cast<uint8_t>(p.detail & physicalFaceMask_ & 0x0f);
        }
    }
}

void JudgementHaptics::UpdatePhysicalFace(uint8_t faceMask) {
    faceMask &= 0x0f;
    const auto now=Clock::now();
    uint8_t releasedActive=0;
    uint8_t newActive=0;
    {
        std::lock_guard lock(mutex_);
        const uint8_t rising=static_cast<uint8_t>(faceMask & ~physicalFaceMask_);
        const uint8_t falling=static_cast<uint8_t>(physicalFaceMask_ & ~faceMask);
        for(int i=0;i<4;i++) if(rising&(1u<<i)) faceDownSince_[i]=now;
        holdEligibleMask_ &= static_cast<uint8_t>(~falling);
        releasedActive = static_cast<uint8_t>(activeHoldMask_ & falling);
        activeHoldMask_ &= faceMask;
        physicalFaceMask_=faceMask;
        // Record the logical face state on every report. Keeping repeated samples
        // is intentional: it lets a judgement arriving on another thread match
        // the state that was actually held across the confirmation instant.
        faceSamples_.push_back(FaceSample{faceMask,now});
        while(faceSamples_.size()>96) faceSamples_.pop_front();
        newActive=activeHoldMask_;
    }
    if(releasedActive) engine_.Trigger(HapticEvent::HoldRelease);
    engine_.SetHoldMask(newActive);
}

void JudgementHaptics::UpdatePhysicalSlide(uint8_t sideMask) {
    std::lock_guard lock(mutex_);
    physicalSlideSides_=static_cast<uint8_t>(sideMask&0x03);
}

void JudgementHaptics::OnGamePoll() {
    std::lock_guard lock(mutex_);
    lastGamePoll_=Clock::now();
}

void JudgementHaptics::OnInternalTargetHit(void* target, int32_t rawHitState) {
    if(!target || rawHitState<0 || rawHitState>8) return;
    const auto now=Clock::now();
    std::lock_guard lock(mutex_);
    purgeLocked(now);
    // The same target may be checked several times in a frame; keep one recent
    // event per pointer/result moment and let correlation de-duplicate by pointer.
    targetHits_.push_back(TargetHit{target,rawHitState,now});
}

void JudgementHaptics::OnJudgement(int32_t rawHitState, bool slide, bool slideChain,
                                   bool slideChainStart, bool slideChainMax,
                                   bool slideChainContinues, bool successNote,
                                   int reportedMultiCount, int directTargetCount) {
    if(!cfg_.judgement.enabled) return;
    const auto d=Decode(rawHitState,reportedMultiCount);
    if(!d.valid) return;
    const auto now=Clock::now();

    Pending paired{};
    bool havePair=false;
    uint8_t chainSides=0;
    ConfirmBurst flush{};
    bool haveFlush=false;

    {
        std::lock_guard lock(mutex_);
        lastGamePoll_=now;
        purgeLocked(now);

        // A slide is a different mechanic; do not let an immediately preceding
        // face-note burst wait behind it.
        if(slide) haveFlush=takeBurstLocked(now,true,flush);
        else haveFlush=takeBurstLocked(now,false,flush);

        const int pi=findPendingLocked(d,slide,now);
        if(pi>=0) {
            paired=pending_[static_cast<size_t>(pi)];
            pending_.erase(pending_.begin()+pi);
            havePair=true;
        }

        if(havePair && gradeAllowsSustain(d.grade) && !paired.slide && paired.detail)
            holdEligibleMask_ |= static_cast<uint8_t>(paired.detail & physicalFaceMask_ & 0x0f);

        if(slide && gradeAllowsSustain(d.grade) &&
           (slideChain || slideChainStart || slideChainContinues || slideChainMax)) {
            chainSides = havePair ? slideSidesForEvent(paired.event) : physicalSlideSides_;
            if(!chainSides) chainSides=chainMask_;
            if(!chainSides) chainSides=0x03;
            chainMask_=chainSides;
            chainUntil_=now+std::chrono::milliseconds(std::max(90,cfg_.input.chainStartMs+35));
        }

        if(!slide) {
            const int hint=havePair ? pendingCount(paired) : 1;
            if(!burst_.active) {
                burst_.active=true;
                burst_.decoded=d;
                burst_.callbackCount=1;
                burst_.directTargetCount=std::clamp(directTargetCount,0,4);
                burst_.reportedMax=std::max(1,d.multiCount);
                burst_.hintedCount=std::max(1,hint);
                burst_.first=burst_.last=now;
                burst_.successNote=successNote;
                if(havePair){burst_.pending=paired;burst_.havePending=true;}
            } else {
                // Same-frame/same-moment GetHitState calls are treated as one chord.
                burst_.callbackCount=std::min(4,burst_.callbackCount+1);
                burst_.directTargetCount=std::max(burst_.directTargetCount,std::clamp(directTargetCount,0,4));
                burst_.reportedMax=std::max(burst_.reportedMax,std::max(1,d.multiCount));
                burst_.hintedCount=std::max(burst_.hintedCount,std::max(1,hint));
                if(static_cast<int>(d.grade)>static_cast<int>(burst_.decoded.grade))
                    burst_.decoded.grade=d.grade; // use the weakest result in a mixed chord
                burst_.successNote = burst_.successNote || successNote;
                burst_.last=now;
                if(havePair) {
                    if(!burst_.havePending){burst_.pending=paired;burst_.havePending=true;}
                    else {
                        burst_.pending.detail=static_cast<uint8_t>((burst_.pending.detail|paired.detail)&0x0f);
                        burst_.hintedCount=std::max(burst_.hintedCount,pendingCount(burst_.pending));
                    }
                }
            }
        }
    }

    if(haveFlush) emitBurst(flush);

    if(slide) {
        ChartJudgementMatch chartMatch{};
        if(chart_) chartMatch=chart_->ObserveJudgement(now,true,successNote);
        if(chartMatch.matched && cfg_.challenge.enabled) {
            engine_.SetChallengeActive(chartMatch.challengeActive);
            if(chartMatch.challengeTransition)
                engine_.Trigger(chartMatch.challengeActive ? HapticEvent::ChallengeStart : HapticEvent::ChallengeEnd);
        }

        Decoded sd=d;
        Pending shaped=paired;
        bool haveShaped=havePair;
        int chartSlideCount=0;
        if(chartMatch.matched && chartMatch.group.slideMask) {
            chartSlideCount=std::max(1,static_cast<int>(chartMatch.group.slideTargetCount));
            if(!haveShaped){shaped.when=now;shaped.slide=true;shaped.detail=0;haveShaped=true;}
            const uint8_t sm=chartMatch.group.slideMask;
            if(sm==0x01) shaped.event=chartSlideCount>=2?HapticEvent::SlideDoubleLeft:HapticEvent::SlideLeft;
            else if(sm==0x02) shaped.event=chartSlideCount>=2?HapticEvent::SlideDoubleRight:HapticEvent::SlideRight;
            else if(havePair && (paired.event==HapticEvent::SlideOutward || paired.event==HapticEvent::SlideInward)) shaped.event=paired.event;
            else shaped.event=HapticEvent::SlideMixed;
            if(chartMatch.group.chainMask && gradeAllowsSustain(sd.grade)) {
                const uint8_t sides=static_cast<uint8_t>(chartMatch.group.chainMask&0x03);
                {
                    std::lock_guard lock(mutex_);
                    chainMask_=sides?sides:sm;
                    chainUntil_=now+std::chrono::milliseconds(std::max(90,cfg_.input.chainStartMs+35));
                }
                engine_.SetChainMask(sides?sides:sm);
            }
        } else {
            if(directTargetCount>=2 && directTargetCount<=4) sd.multiCount=directTargetCount;
            else if(havePair) sd.multiCount=std::max(sd.multiCount,pendingCount(paired));
        }

        playGradeOverlay(sd.grade);
        if(sd.grade!=Grade::Worst) playConfirmed(haveShaped ? &shaped : nullptr,sd,true,false);
        const bool chartSuccess=chartMatch.matched && chartMatch.group.specialSlideMask!=0;
        if((successNote||chartSuccess) && gradeAllowsSustain(sd.grade))
            engine_.Trigger(HapticEvent::SuccessNote,gradeGain(sd.grade));
        if(cfg_.judgement.logEvents) {
            Log::Info(std::string("DIVA confirmed: ") + GradeName(sd.grade) +
                      " slide=1 chain=" + ((slideChain||slideChainStart||slideChainContinues||slideChainMax||(chartMatch.matched&&chartMatch.group.chainMask)) ? std::string("1") : std::string("0")) +
                      " chart_slide_count=" + std::to_string(chartSlideCount) +
                      " chart_group=" + std::to_string(chartMatch.groupIndex) +
                      " chart_ms=" + std::to_string(chartMatch.timingErrorMs) +
                      " targets=" + std::to_string(std::clamp(directTargetCount,0,4)) +
                      " input_hint=" + std::to_string(havePair ? pendingCount(paired) : 1) +
                      " reported=" + std::to_string(std::max(1,d.multiCount)) +
                      (chartMatch.matched ? " source=chart" : " source=fallback") +
                      ((successNote||chartSuccess) ? " success_note=1" : " success_note=0") +
                      (chartMatch.matched&&chartMatch.challengeActive ? " challenge=1" : " challenge=0"));
        }
    }
}

void JudgementHaptics::SetHookAvailable(bool available) {
    bool clear=false;
    {
        std::lock_guard lock(mutex_);
        hookAvailable_=available;
        if(!available){
            pending_.clear(); orphan_.clear(); targetHits_.clear(); faceSamples_.clear(); burst_={}; lastGamePoll_={};
            holdEligibleMask_=activeHoldMask_=0;
            physicalFaceMask_=physicalSlideSides_=0;
            chainMask_=0; chainUntil_={};
            clear=true;
        }
    }
    if(clear){engine_.SetHoldMask(0);engine_.SetChainMask(0);}
}

bool JudgementHaptics::HookAvailable() const {
    std::lock_guard lock(mutex_);
    return hookAvailable_;
}

bool JudgementHaptics::InGameplay() const {
    const auto now=Clock::now();
    std::lock_guard lock(mutex_);
    return inGameplayLocked(now);
}

void JudgementHaptics::Reset() {
    {
        std::lock_guard lock(mutex_);
        pending_.clear(); orphan_.clear(); targetHits_.clear(); burst_={};
        physicalFaceMask_=physicalSlideSides_=0;
        holdEligibleMask_=activeHoldMask_=0;
        chainMask_=0;chainUntil_={};
    }
    engine_.SetHoldMask(0);
    engine_.SetChainMask(0);
}

void JudgementHaptics::Tick() {
    const auto now=Clock::now();
    uint8_t oldHold=0,newHold=0,newChain=0;
    bool holdStarted=false,holdReleased=false;
    ConfirmBurst flush{};
    bool haveFlush=false;
    {
        std::lock_guard lock(mutex_);
        purgeLocked(now);
        haveFlush=takeBurstLocked(now,false,flush);
        oldHold=activeHoldMask_;
        if(!inGameplayLocked(now)) {
            // If gameplay just ended, do not lose the final confirmed note.
            if(!haveFlush) haveFlush=takeBurstLocked(now,true,flush);
            holdEligibleMask_=0;
            activeHoldMask_=0;
            chainMask_=0;
            chainUntil_={};
        } else {
            uint8_t eligible=static_cast<uint8_t>(holdEligibleMask_ & physicalFaceMask_ & 0x0f);
            uint8_t active=0;
            for(int i=0;i<4;i++) {
                const uint8_t bit=static_cast<uint8_t>(1u<<i);
                if(!(eligible&bit) || faceDownSince_[i]==TP{}) continue;
                const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-faceDownSince_[i]).count();
                if(ms>=cfg_.input.holdStartMs) active|=bit;
            }
            activeHoldMask_=active;
            if(chainMask_ && chainUntil_!=TP{} && now>chainUntil_) {
                chainMask_=0;chainUntil_={};
            }
        }
        newHold=activeHoldMask_;
        newChain=chainMask_;
        holdStarted=(oldHold==0 && newHold!=0);
        holdReleased=(oldHold!=0 && newHold==0);
    }
    if(haveFlush) emitBurst(flush);
    if(holdStarted) engine_.Trigger(HapticEvent::HoldStart);
    if(holdReleased) engine_.Trigger(HapticEvent::HoldRelease);
    engine_.SetHoldMask(newHold);
    engine_.SetChainMask(newChain);
}
