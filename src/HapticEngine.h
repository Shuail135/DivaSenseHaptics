#pragma once
#include "Config.h"
#include "RingBuffer.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

enum class HapticEvent {
    Square, Cross, Circle, Triangle, NoteGeneric,
    Multi2, Multi3, Multi4,
    HoldStart, HoldRelease,
    SlideLeft, SlideRight,
    SlideDoubleLeft, SlideDoubleRight,
    SlideOutward, SlideInward, SlideMixed,
    JudgementCool, JudgementFine, JudgementSafe, JudgementSad,
    JudgementWrong, JudgementWorst,
    MenuUp, MenuDown, MenuLeft, MenuRight, MenuConfirm,
    SuccessNote,
    ChallengeStart, ChallengeEnd
};

class HapticEngine {
public:
    void Configure(const ModConfig& cfg);
    void PushAudioFloatStereo(const float* interleaved, uint32_t frames, uint32_t sampleRate);
    void Trigger(HapticEvent e, float gainScale = 1.0f);
    void SetHoldMask(uint8_t mask);
    void SetChainMask(uint8_t mask); // bit0 left, bit1 right
    void SetChallengeActive(bool active);

    // Produces LR haptic PCM at output sample rate, interleaved (2 floats/frame).
    void RenderBlock(float* outLR, uint32_t frames, uint32_t outputSampleRate);


private:
    struct FilterState { float prevX=0, hp=0, lp=0; };
    struct Voice { HapticEvent event{}; double age=0.0; float gain=1.0f;};

    float filterSample(float x, FilterState& s, uint32_t sampleRate);
    static float softClip(float x, float drive);
    static float envExp(double age, double decay);
    static float sine(double hz, double t);
    static float chirp(double f0, double f1, double t, double duration);
    void voiceSample(const Voice& v, float& l, float& r) const;

    ModConfig cfg_{};
    StereoRing<131072> audio_{};
    FilterState filterL_{}, filterR_{};

    std::mutex voicesMutex_;
    std::vector<Voice> voices_;

    std::atomic<uint8_t> holdMask_{0};
    std::atomic<uint8_t> chainMask_{0};
    std::atomic<bool> challengeActive_{false};
    double holdPhase_ = 0.0;
    double chainPhase_ = 0.0;
};
