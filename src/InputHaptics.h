#pragma once
#include "Config.h"
#include "HapticEngine.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

class JudgementHaptics;

enum class MenuAction {
    Up, Down, Left, Right, Confirm
};

class InputHaptics {
public:
    using MenuSubmit = std::function<void(MenuAction)>;

    InputHaptics(HapticEngine& engine, const ModConfig& cfg,
                 JudgementHaptics* judgement,
                 MenuSubmit menuSubmit = {});
    void OnUsbReport(const uint8_t* data, size_t size);
    void Reset();

private:
    using Clock=std::chrono::steady_clock;
    using TP=Clock::time_point;

    struct SlidePending {
        uint8_t actions=0;
        TP since{};
        bool active=false;
    };

    uint8_t physicalFaceMask(const uint8_t* d) const;
    uint8_t logicalFaceMask(const uint8_t* d) const;
    uint8_t slideActions(const uint8_t* d) const;
    uint8_t slideSideMask(uint8_t slide) const;
    uint8_t menuDirections(const uint8_t* d) const;
    void primeModeState(const uint8_t* d, size_t size, bool gameplay);
    void handleMenuInput(const uint8_t* d);
    void clearGameplayState();
    void flushNotePending(TP now);
    void flushSlidePending(TP now);
    void updateTouch(const uint8_t* d, size_t size);

    HapticEngine& engine_;
    ModConfig cfg_;
    JudgementHaptics* judgement_ = nullptr;
    MenuSubmit menuSubmit_{};

    bool modeKnown_=false;
    bool wasGameplay_=false;
    uint8_t prevMenuDirs_=0;
    bool prevMenuConfirm_=false;

    uint8_t prevFace_=0;
    uint8_t prevSlide_=0;
    uint8_t pendingNoteMask_=0;
    bool notePending_=false;
    TP notePendingSince_{};
    SlidePending slidePending_{};


    bool touchActive_=false;
    int touchLastX_=0;
    int touchDirection_=0;
};
