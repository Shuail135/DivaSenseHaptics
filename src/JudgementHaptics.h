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

    struct Decoded {
        Grade grade = Grade::None;
        int multiCount = 0;
        bool valid = false;
    };

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
    void OnJudgement(int32_t rawHitState, bool slide, bool slideChain,
                     bool slideChainStart, bool slideChainMax,
                     bool slideChainContinues, bool successNote,
                     int reportedMultiCount);

    void SetHookAvailable(bool available);
    bool HookAvailable() const;
    bool InGameplay() const;
    void Tick();

    static Decoded Decode(int32_t rawHitState, int reportedMultiCount = 0);
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
    struct FaceSample {
        uint8_t mask = 0;
        TP when{};
    };
    struct ConfirmBurst {
        bool active = false;
        Decoded decoded{};
        Grade bodyGrade = Grade::None;
        int32_t rawHitState = 21;
        int callbackCount = 0;
        int reportedMax = 1;
        int hintedCount = 1;
        Pending pending{};
        bool havePending = false;
        bool successNote = false;
        TP first{};
        TP last{};
    };

    static int bitCount(uint8_t v);
    static bool gradeAllowsSustain(Grade grade);
    static uint8_t slideSidesForEvent(HapticEvent event);
    bool inGameplayLocked(TP now) const;
    float gradeGain(Grade grade) const;
    HapticEvent gradeEvent(Grade grade) const;
    int pendingCount(const Pending& p) const;
    void purgeLocked(TP now);
    int physicalChordCountLocked(const ConfirmBurst& burst, uint8_t& maskOut) const;
    int findPendingLocked(const Decoded& d, bool slide, TP now) const;
    static HapticEvent eventForGameResult(const Pending* pending, const Decoded& decoded, bool gameSlide);
    void playConfirmed(const Pending* pending, const Decoded& decoded, bool gameSlide);
    void playGradeOverlay(Grade grade);
    bool takeBurstLocked(TP now, bool force, ConfirmBurst& out);
    void emitBurst(const ConfirmBurst& burst);

    HapticEngine& engine_;
    ModConfig cfg_;
    ChartAwareness* chart_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<Pending> pending_;
    std::deque<FaceSample> faceSamples_;
    ConfirmBurst burst_{};
    bool hookAvailable_ = false;
    unsigned unknownResultLogs_ = 0;
    TP lastGamePoll_{};

    uint8_t physicalFaceMask_ = 0;
    uint8_t physicalSlideSides_ = 0;
    std::array<TP,4> faceDownSince_{};
    uint8_t holdEligibleMask_ = 0;
    uint8_t activeHoldMask_ = 0;

    uint8_t chainMask_ = 0;
    TP chainUntil_{};
};
