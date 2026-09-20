#include "JudgementHaptics.h"
#include "ChartAwareness.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <limits>

JudgementHaptics::JudgementHaptics(HapticEngine& engine, const ModConfig& cfg, ChartAwareness* chart)
    : engine_(engine), cfg_(cfg), chart_(chart) {}

bool JudgementHaptics::gradeAllowsSustain(Grade g) {
    return g == Grade::Cool || g == Grade::Fine || g == Grade::Safe || g == Grade::Sad;
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

bool JudgementHaptics::inGameplayLocked(TP now) const {
    if(!hookAvailable_ || lastGamePoll_==TP{}) return false;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-lastGamePoll_).count();
    return ms<=cfg_.judgement.gameplayPollTimeoutMs;
}

void JudgementHaptics::purgeLocked(TP now) {
    const auto keepMs = std::max(1, cfg_.judgement.matchWindowMs);
    while(!pending_.empty() &&
          std::chrono::duration_cast<std::chrono::milliseconds>(now-pending_.front().when).count() > keepMs)
        pending_.pop_front();
    while(pending_.size()>16) pending_.pop_front();
}


int JudgementHaptics::findPendingLocked(bool slide, TP now) const {
    int best=-1;
    long long bestScore=std::numeric_limits<long long>::max();
    for(size_t i=0;i<pending_.size();++i) {
        const auto age=std::llabs(std::chrono::duration_cast<std::chrono::milliseconds>(now-pending_[i].when).count());
        if(age>cfg_.judgement.matchWindowMs || pending_[i].slide!=slide) continue;
        if(age<bestScore){bestScore=age;best=static_cast<int>(i);}
    }
    return best;
}

void JudgementHaptics::playGradeOverlay(Grade grade) {
    const float g=gradeGain(grade);
    if(g<=0.0f || cfg_.judgement.overlayGain<=0.0f) return;
    engine_.Trigger(gradeEvent(grade), g);
}

HapticEvent JudgementHaptics::eventForGameResult(const Pending* p, int multiCount, bool gameSlide) {
    if(gameSlide) {
        if(p && p->slide) return p->event; // preserve known left/right direction
        return HapticEvent::SlideMixed;    // mapped key/keyboard: confirmed slide, unknown direction
    }
    if(multiCount>=4) return HapticEvent::Multi4;
    if(multiCount==3) return HapticEvent::Multi3;
    if(multiCount==2) return HapticEvent::Multi2;
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

void JudgementHaptics::playConfirmed(const Pending* p, Grade grade, int multiCount, bool gameSlide) {
    const float g=std::clamp(gradeGain(grade),0.0f,2.0f);
    if(g<=0.0f) return;
	engine_.Trigger(eventForGameResult(p, multiCount, gameSlide), g);
}


void JudgementHaptics::Submit(HapticEvent event, uint8_t detailMask, bool slide) {
    if(!cfg_.judgement.enabled) return;

    const auto now=Clock::now();
    {
        std::lock_guard lock(mutex_);
        if(!hookAvailable_) return;
        // Menus/loading are handled by the separate UI-response validator.
        if(!inGameplayLocked(now)) return;
    }

    std::lock_guard lock(mutex_);
    purgeLocked(now);
    pending_.push_back({event,static_cast<uint8_t>(detailMask&0x0f),slide,now});
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

void JudgementHaptics::OnJudgement(Grade grade, bool slide, bool slideChain,
                                   bool slideChainStart, bool slideChainMax,
                                   bool slideChainContinues, bool successNote,
                                   int reportedMultiCount) {
    if(!cfg_.judgement.enabled) return;
    
    if(grade == Grade::None) return;
	
	const int multiCount = std::clamp(reportedMultiCount, 1, 4);

    const auto now=Clock::now();

    Pending paired{};
    bool havePair=false;
    {
        std::lock_guard lock(mutex_);
        lastGamePoll_=now;
        purgeLocked(now);

        const int pi=findPendingLocked(slide,now);
        if(pi>=0) {
            paired=pending_[static_cast<size_t>(pi)];
            pending_.erase(pending_.begin()+pi);
            havePair=true;
        }

        if(slide && gradeAllowsSustain(grade) &&
           (slideChain || slideChainStart || slideChainContinues || slideChainMax)) {
            uint8_t sides = havePair ? slideSidesForEvent(paired.event) : physicalSlideSides_;
            if(!sides) sides = chainMask_;
            if(!sides) sides = 0x03;
            chainMask_=sides;
            chainUntil_=now+std::chrono::milliseconds(std::max(90,cfg_.input.chainStartMs+35));
        }
    }

	if (!slide) {
		ChartJudgementMatch chartMatch{};
		if (chart_) chartMatch = chart_->ObserveJudgement(now, false, successNote);
		if (chartMatch.matched && cfg_.challenge.enabled) {
			engine_.SetChallengeActive(chartMatch.challengeActive);
			if (chartMatch.challengeTransition) {
				engine_.Trigger(chartMatch.challengeActive ? HapticEvent::ChallengeStart : HapticEvent::ChallengeEnd);
			}
		}
		
		playGradeOverlay(grade);
		if (grade != Grade::Worst) {
			playConfirmed(havePair ? &paired : nullptr, grade, multiCount, false);
		}
		if (gradeAllowsSustain(grade)) {
			std::lock_guard lock(mutex_);
			const uint8_t holdMask = chartMatch.matched ? chartMatch.group.holdMask : (havePair ? paired.detail : 0);
			holdEligibleMask_ |= static_cast<uint8_t>(holdMask & physicalFaceMask_);
		}
		
		const bool chartSuccess = chartMatch.matched && chartMatch.group.specialFaceMask != 0;
		if ((successNote || chartSuccess) && gradeAllowsSustain(grade)) {
			engine_.Trigger(HapticEvent::SuccessNote, gradeGain(grade));
		}
		if(cfg_.judgement.logEvents) {
            Log::Info(std::string("DIVA grade: ") + GradeName(grade) +
                      " multi=" + std::to_string(multiCount) +
                      (chartMatch.matched&&chartMatch.challengeActive ? " challenge=1" : " challenge=0"));
        }
		
	}

    if(slide) {
        ChartJudgementMatch chartMatch{};
        if(chart_) chartMatch=chart_->ObserveJudgement(now,true,successNote);
        if(chartMatch.matched && cfg_.challenge.enabled) {
            engine_.SetChallengeActive(chartMatch.challengeActive);
            if(chartMatch.challengeTransition)
                engine_.Trigger(chartMatch.challengeActive ? HapticEvent::ChallengeStart : HapticEvent::ChallengeEnd);
        }

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
            if(chartMatch.group.chainMask && gradeAllowsSustain(grade)) {
                const uint8_t sides=static_cast<uint8_t>(chartMatch.group.chainMask&0x03);
                {
                    std::lock_guard lock(mutex_);
                    chainMask_=sides?sides:sm;
                    chainUntil_=now+std::chrono::milliseconds(std::max(90,cfg_.input.chainStartMs+35));
                }
                engine_.SetChainMask(sides?sides:sm);
            }
        } 

        playGradeOverlay(grade);
        if(grade!=Grade::Worst) playConfirmed(haveShaped ? &shaped : nullptr, grade, multiCount,true);
        const bool chartSuccess=chartMatch.matched && chartMatch.group.specialSlideMask!=0;
        if((successNote||chartSuccess) && gradeAllowsSustain(grade))
            engine_.Trigger(HapticEvent::SuccessNote,gradeGain(grade));
        if(cfg_.judgement.logEvents) {
            Log::Info(std::string("DIVA grade: ") + GradeName(grade) +
                      " multi=" + std::to_string(multiCount) +
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
            pending_.clear(); lastGamePoll_={};
            holdEligibleMask_=activeHoldMask_=0;
            physicalFaceMask_=physicalSlideSides_=0;
            chainMask_=0; chainUntil_={};
            clear=true;
        }
    }
    if(clear){engine_.SetHoldMask(0);engine_.SetChainMask(0);engine_.SetChallengeActive(false);}
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

void JudgementHaptics::Tick() {
    const auto now=Clock::now();
    uint8_t oldHold=0,newHold=0,newChain=0;
    bool holdStarted=false,holdReleased=false;
    {
        std::lock_guard lock(mutex_);
        purgeLocked(now);
        oldHold=activeHoldMask_;
        if(!inGameplayLocked(now)) {
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
    if(holdStarted) engine_.Trigger(HapticEvent::HoldStart);
    if(holdReleased) engine_.Trigger(HapticEvent::HoldRelease);
    engine_.SetHoldMask(newHold);
    engine_.SetChainMask(newChain);
    if(!InGameplay()) engine_.SetChallengeActive(false);
}
