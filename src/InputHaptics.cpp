#include "InputHaptics.h"
#include "JudgementHaptics.h"
#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {
constexpr uint8_t N_SQUARE=1<<0;
constexpr uint8_t N_CROSS=1<<1;
constexpr uint8_t N_CIRCLE=1<<2;
constexpr uint8_t N_TRIANGLE=1<<3;

constexpr uint8_t M_UP=1<<0, M_DOWN=1<<1, M_LEFT=1<<2, M_RIGHT=1<<3;

// slide action bits: stick directions plus each shoulder independently.
constexpr uint8_t S_LSL=1<<0, S_LSR=1<<1, S_RSL=1<<2, S_RSR=1<<3;
constexpr uint8_t S_L1=1<<4, S_R1=1<<5, S_L2=1<<6, S_R2=1<<7;
constexpr uint8_t S_LEFT_MASK=S_LSL|S_RSL|S_L1|S_L2;
constexpr uint8_t S_RIGHT_MASK=S_LSR|S_RSR|S_R1|S_R2;
}

InputHaptics::InputHaptics(HapticEngine& e,const ModConfig& c,JudgementHaptics* judgement,MenuSubmit menuSubmit)
    :engine_(e),cfg_(c),judgement_(judgement),menuSubmit_(std::move(menuSubmit)){Reset();}

void InputHaptics::clearGameplayState() {
    prevFace_=prevSlide_=pendingNoteMask_=0;
    notePending_=false; slidePending_={};
    touchActive_=false; touchLastX_=0; touchDirection_=0;
    engine_.SetHoldMask(0); engine_.SetChainMask(0);
    if(judgement_) {
        judgement_->UpdatePhysicalFace(0);
        judgement_->UpdatePhysicalSlide(0);
    }
}

void InputHaptics::Reset() {
    modeKnown_=false;wasGameplay_=false;
    prevMenuDirs_=0;prevMenuConfirm_=false;
    clearGameplayState();
}

int InputHaptics::bitCount(uint8_t v) { int n=0; while(v){n+=v&1;v>>=1;} return n; }

uint8_t InputHaptics::physicalFaceMask(const uint8_t* d) const {
    const uint8_t b=d[8];
    uint8_t m=0;
    if(b&0x10)m|=N_SQUARE;
    if(b&0x20)m|=N_CROSS;
    if(b&0x40)m|=N_CIRCLE;
    if(b&0x80)m|=N_TRIANGLE;
    return m;
}

uint8_t InputHaptics::logicalFaceMask(const uint8_t* d) const {
    const uint8_t b=d[8];
    uint8_t m=physicalFaceMask(d);
    if(cfg_.input.dpadAsFaceButtons) {
        switch(b&0x0f) {
        case 0:m|=N_TRIANGLE;break;
        case 1:m|=N_TRIANGLE|N_CIRCLE;break;
        case 2:m|=N_CIRCLE;break;
        case 3:m|=N_CIRCLE|N_CROSS;break;
        case 4:m|=N_CROSS;break;
        case 5:m|=N_CROSS|N_SQUARE;break;
        case 6:m|=N_SQUARE;break;
        case 7:m|=N_SQUARE|N_TRIANGLE;break;
        default:break;
        }
    }
    const uint8_t b2=d[9];
    if((b2&0x01) && cfg_.input.l1MacroMask) m|=cfg_.input.l1MacroMask;
    if((b2&0x02) && cfg_.input.r1MacroMask) m|=cfg_.input.r1MacroMask;
    if((b2&0x04) && cfg_.input.l2MacroMask) m|=cfg_.input.l2MacroMask;
    if((b2&0x08) && cfg_.input.r2MacroMask) m|=cfg_.input.r2MacroMask;
    return m;
}

uint8_t InputHaptics::slideActions(const uint8_t* d) const {
    uint8_t s=0;
    if(cfg_.input.sticksAsSlides) {
        const int lo=128-std::clamp(cfg_.input.stickThreshold,20,120);
        const int hi=128+std::clamp(cfg_.input.stickThreshold,20,120);
        if(d[1]<lo)s|=S_LSL; else if(d[1]>hi)s|=S_LSR;
        if(d[3]<lo)s|=S_RSL; else if(d[3]>hi)s|=S_RSR;
    }
    if(cfg_.input.shouldersAsSlides) {
        const uint8_t b=d[9];
        if((b&0x01) && !cfg_.input.l1MacroMask)s|=S_L1;
        if((b&0x02) && !cfg_.input.r1MacroMask)s|=S_R1;
        if((b&0x04) && !cfg_.input.l2MacroMask)s|=S_L2;
        if((b&0x08) && !cfg_.input.r2MacroMask)s|=S_R2;
    }
    return s;
}

uint8_t InputHaptics::slideSideMask(uint8_t slide) const {
    uint8_t sides=0;
    if(slide&S_LEFT_MASK)sides|=0x01;
    if(slide&S_RIGHT_MASK)sides|=0x02;
    return sides;
}

uint8_t InputHaptics::menuDirections(const uint8_t* d) const {
    uint8_t dirs=0;
    switch(d[8]&0x0f) {
    case 0: dirs|=M_UP; break;
    case 1: dirs|=M_UP|M_RIGHT; break;
    case 2: dirs|=M_RIGHT; break;
    case 3: dirs|=M_RIGHT|M_DOWN; break;
    case 4: dirs|=M_DOWN; break;
    case 5: dirs|=M_DOWN|M_LEFT; break;
    case 6: dirs|=M_LEFT; break;
    case 7: dirs|=M_LEFT|M_UP; break;
    default: break;
    }
    if(cfg_.menu.stickNavigation) {
        const int lo=128-std::clamp(cfg_.input.stickThreshold,20,120);
        const int hi=128+std::clamp(cfg_.input.stickThreshold,20,120);
        if(d[2]<lo)dirs|=M_UP; else if(d[2]>hi)dirs|=M_DOWN;
        if(d[1]<lo)dirs|=M_LEFT; else if(d[1]>hi)dirs|=M_RIGHT;
    }
    if(!cfg_.menu.leftRightNavigation) dirs&=static_cast<uint8_t>(~(M_LEFT|M_RIGHT));
    return dirs;
}

void InputHaptics::primeModeState(const uint8_t* d,size_t size,bool gameplay) {
    clearGameplayState();
    prevMenuDirs_=menuDirections(d);
    prevMenuConfirm_=(physicalFaceMask(d)&cfg_.menu.confirmMask)!=0;
    if(gameplay) {
        prevFace_=logicalFaceMask(d);
        prevSlide_=slideActions(d);
        if(judgement_) {
            judgement_->UpdatePhysicalFace(prevFace_);
            uint8_t sides=slideSideMask(prevSlide_);
            if(size>=41 && cfg_.input.touchpadAsSlides) {
                const bool touch=(d[33]&0x80)==0;
                if(touch){touchActive_=true;touchLastX_=d[34]|((d[35]&0x0f)<<8);}
            }
            judgement_->UpdatePhysicalSlide(sides);
        }
    }
}

void InputHaptics::handleMenuInput(const uint8_t* d) {
    if(!cfg_.menu.enabled || !menuSubmit_) {
        prevMenuDirs_=menuDirections(d);
        prevMenuConfirm_=(physicalFaceMask(d)&cfg_.menu.confirmMask)!=0;
        return;
    }
    const uint8_t dirs=menuDirections(d);
    const uint8_t rising=static_cast<uint8_t>(dirs&~prevMenuDirs_);
    if(rising&M_UP)menuSubmit_(MenuAction::Up);
    if(rising&M_DOWN)menuSubmit_(MenuAction::Down);
    if(rising&M_LEFT)menuSubmit_(MenuAction::Left);
    if(rising&M_RIGHT)menuSubmit_(MenuAction::Right);

    const bool confirm=(physicalFaceMask(d)&cfg_.menu.confirmMask)!=0;
    if(confirm && !prevMenuConfirm_)menuSubmit_(MenuAction::Confirm);
    prevMenuDirs_=dirs;
    prevMenuConfirm_=confirm;
}

void InputHaptics::flushNotePending(TP now) {
    if(!notePending_)return;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-notePendingSince_).count();
    if(ms<cfg_.input.multiWindowMs)return;
    const int n=bitCount(pendingNoteMask_);
    if(n>=4)judgement_->Submit(HapticEvent::Multi4,pendingNoteMask_);
    else if(n==3)judgement_->Submit(HapticEvent::Multi3,pendingNoteMask_);
    else if(n==2)judgement_->Submit(HapticEvent::Multi2,pendingNoteMask_);
    else if(n==1) {
        if(pendingNoteMask_&N_SQUARE)judgement_->Submit(HapticEvent::Square,pendingNoteMask_);
        else if(pendingNoteMask_&N_CROSS)judgement_->Submit(HapticEvent::Cross,pendingNoteMask_);
        else if(pendingNoteMask_&N_CIRCLE)judgement_->Submit(HapticEvent::Circle,pendingNoteMask_);
        else judgement_->Submit(HapticEvent::Triangle,pendingNoteMask_);
    }
    notePending_=false;pendingNoteMask_=0;
}

void InputHaptics::flushSlidePending(TP now) {
    if(!slidePending_.active)return;
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-slidePending_.since).count();
    if(ms<cfg_.input.slideWindowMs)return;

    uint8_t a=slidePending_.actions;
    int leftCount=((a&S_LSL)?1:0)+((a&S_RSL)?1:0)+((a&S_L1)?1:0)+((a&S_L2)?1:0);
    int rightCount=((a&S_LSR)?1:0)+((a&S_RSR)?1:0)+((a&S_R1)?1:0)+((a&S_R2)?1:0);

    const bool leftStickLeft=a&S_LSL, leftStickRight=a&S_LSR;
    const bool rightStickLeft=a&S_RSL, rightStickRight=a&S_RSR;

    if(leftCount>=2 && rightCount==0) judgement_->Submit(HapticEvent::SlideDoubleLeft,0,true);
    else if(rightCount>=2 && leftCount==0) judgement_->Submit(HapticEvent::SlideDoubleRight,0,true);
    else if(leftStickLeft && rightStickRight) judgement_->Submit(HapticEvent::SlideOutward,0,true);
    else if(leftStickRight && rightStickLeft) judgement_->Submit(HapticEvent::SlideInward,0,true);
    else if(leftCount && rightCount) judgement_->Submit(HapticEvent::SlideMixed,0,true);
    else if(leftCount) judgement_->Submit(HapticEvent::SlideLeft,0,true);
    else if(rightCount) judgement_->Submit(HapticEvent::SlideRight,0,true);

    slidePending_={};
}

void InputHaptics::updateTouch(const uint8_t* d,size_t size) {
    if(!cfg_.input.touchpadAsSlides || size<41)return;
    bool active=(d[33]&0x80)==0;
    int x=d[34]|((d[35]&0x0f)<<8);
    if(active && !touchActive_) {
        touchActive_=true;touchLastX_=x;touchDirection_=0;
    } else if(active && touchActive_) {
        int dx=x-touchLastX_;
        if(std::abs(dx)>=cfg_.input.touchSwipePixels) {
            touchDirection_=dx<0?-1:1;
            judgement_->Submit(dx<0?HapticEvent::SlideLeft:HapticEvent::SlideRight,0,true);
            touchLastX_=x;
        }
    } else if(!active && touchActive_) {
        touchActive_=false;touchDirection_=0;
    }
}

void InputHaptics::OnUsbReport(const uint8_t* d,size_t size) {
    if(!judgement_ || !cfg_.input.enabled || !d || size<11 || d[0]!=0x01)return;

    // In the real mod a valid judgement hook is the authority that separates
    // gameplay from UI/loading. If it is unavailable, strict mode intentionally
    // stays silent rather than turning every controller edge into a false haptic.
    if(judgement_ && !judgement_->HookAvailable()) {
        if(modeKnown_)Reset();
        return;
    }

    const bool gameplay = judgement_->InGameplay();
    if(!modeKnown_ || gameplay!=wasGameplay_) {
        modeKnown_=true;wasGameplay_=gameplay;
        primeModeState(d,size,gameplay);
        return;
    }

    if(!gameplay) {
        if(judgement_) {judgement_->UpdatePhysicalFace(0);judgement_->UpdatePhysicalSlide(0);}
        engine_.SetHoldMask(0);engine_.SetChainMask(0);
        handleMenuInput(d);
        return;
    }

    const auto now=Clock::now();
    flushNotePending(now);
    flushSlidePending(now);

    uint8_t face=logicalFaceMask(d);
    uint8_t rising=static_cast<uint8_t>(face&~prevFace_);
    if(rising) {
        if(!notePending_){notePending_=true;notePendingSince_=now;pendingNoteMask_=rising;}
        else pendingNoteMask_|=rising;
    }
    if(judgement_) judgement_->UpdatePhysicalFace(face);

    uint8_t slide=slideActions(d);
    uint8_t slideRise=static_cast<uint8_t>(slide&~prevSlide_);
    if(slideRise) {
        if(!slidePending_.active)slidePending_={slideRise,now,true};
        else slidePending_.actions|=slideRise;
    }
    updateTouch(d,size);

    if(judgement_) {
        uint8_t sides=slideSideMask(slide);
        if(touchActive_ && touchDirection_<0)sides|=0x01;
        if(touchActive_ && touchDirection_>0)sides|=0x02;
        judgement_->UpdatePhysicalSlide(sides);
    }

    prevFace_=face;
    prevSlide_=slide;
}
