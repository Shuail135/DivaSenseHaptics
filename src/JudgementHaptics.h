#pragma once
#include "Config.h"
#include "HapticEngine.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

class ChartAwareness;

class JudgementHaptics {
public:
    enum class Grade { Cool, Fine, Safe, Sad, Wrong, Worst, None };

    JudgementHaptics(HapticEngine& engine, const ModConfig& cfg, ChartAwareness* chart = nullptr);

    // Physical note/slide gestures are only candidates. In strict mode they do
    // not produce gameplay feedback until the game's GetHitState hook confirms
    // that DIVA actually accepted/judged the action.
    void Submit(HapticEvent event, uint8_t detailMask = 0, bool slide = false);

    // Current physical state is used only to shape already-confirmed mechanics:
    // a successful note that remains held may become a HOLD texture, and a
    // confirmed slide-chain may keep a directional chain texture alive.
    void UpdatePhysicalFace(uint8_t faceMask);
    void UpdatePhysicalSlide(uint8_t sideMask); // bit0 left, bit1 right

    // Called from the game's GetHitState hook.
    void OnGamePoll();
    void OnJudgement(Grade, bool slide, bool slideChain,
                     bool slideChainStart, bool slideChainMax,
                     bool slideChainContinues, bool successNote,
                     int reportedMultiCount);

    void SetHookAvailable(bool available);
    bool HookAvailable() const;
    bool InGameplay() const;
    void Tick();

    static const char* GradeName(Grade grade);

private:
    using Clock = std::chrono::steady_clock;
    using TP = Clock::time_point;

    struct Pending {
        HapticEvent event = HapticEvent::Cross;
        uint8_t detail = 0;
        bool slide = false;
        TP when{};
    };

    static bool gradeAllowsSustain(Grade grade);
    static uint8_t slideSidesForEvent(HapticEvent event);
    bool inGameplayLocked(TP now) const;
    float gradeGain(Grade grade) const;
    HapticEvent gradeEvent(Grade grade) const;
    void purgeLocked(TP now);
    int findPendingLocked(bool slide, TP now) const;
    static HapticEvent eventForGameResult(const Pending* pending, int multiCount, bool gameSlide);
    void playConfirmed(const Pending* pending, Grade grade, int multiCount, bool gameSlide);
    void playGradeOverlay(Grade grade);

    HapticEngine& engine_;
    ModConfig cfg_;
    ChartAwareness* chart_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<Pending> pending_;
    bool hookAvailable_ = false;
    TP lastGamePoll_{};

    uint8_t physicalFaceMask_ = 0;
    uint8_t physicalSlideSides_ = 0;
    std::array<TP,4> faceDownSince_{};
    uint8_t holdEligibleMask_ = 0;
    uint8_t activeHoldMask_ = 0;

    uint8_t chainMask_ = 0;
    TP chainUntil_{};
};
