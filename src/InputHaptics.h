#pragma once
#include "Config.h"
#include "HapticEngine.h"
#include <array>
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
                 JudgementHaptics* judgement = nullptr,
                 MenuSubmit menuSubmit = {});
    void OnUsbReport(const uint8_t* data, size_t size);
    void Reset();

private:
    using Clock=std::chrono::steady_clock;
    using TP=Clock::time_point;

    struct PressState { bool down=false, holdActive=false; TP since{}; };
    struct SlidePending {
        uint8_t actions=0;
        TP since{};
        bool active=false;
    };

    static int bitCount(uint8_t v);
    uint8_t physicalFaceMask(const uint8_t* d) const;
    uint8_t logicalFaceMask(const uint8_t* d) const;
    uint8_t slideActions(const uint8_t* d) const;
    uint8_t slideSideMask(uint8_t slide) const;
    uint8_t menuDirections(const uint8_t* d) const;
    void primeModeState(const uint8_t* d, size_t size, bool gameplay);
    void handleMenuInput(const uint8_t* d);
    void clearGameplayState();
    void flushNotePending(TP now, bool force=false);
    void flushSlidePending(TP now, bool force=false);
    void updateHoldsFallback(uint8_t face, TP now);
    void updateChainsFallback(uint8_t slide, TP now);
    void updateTouch(const uint8_t* d, size_t size, TP now);
    void emitGameplay(HapticEvent event, float gain = 1.0f, uint8_t detail = 0, bool slide = false);

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

    std::array<PressState,4> presses_{};
    std::array<TP,4> slideSince_{};
    uint8_t chainActive_=0;

    bool touchActive_=false;
    int touchLastX_=0;
    int touchDirection_=0;
    TP touchLastMove_{};
};
