#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

struct ModConfig {
    struct Audio {
        bool enabled = true;
        float musicGain = 0.32f;
        float masterGain = 0.62f;
        float highPassHz = 35.0f;
        float lowPassHz = 220.0f;
        float softClipDrive = 1.35f;
        std::wstring endpointContains = L"Wireless Controller";
        int maxBufferedMs = 55;
        int targetBufferedMs = 18;
    } audio;

    struct Controller {
        bool enabled = true;
        bool forceAudioHaptics = true;
        int unmuteIntervalMs = 40;
    } controller;

    struct Input {
        bool enabled = true;
        int multiWindowMs = 14;
        int holdStartMs = 115;
        int slideWindowMs = 18;
        int chainStartMs = 85;
        int stickThreshold = 72;
        int touchSwipePixels = 90;
        bool sticksAsSlides = true;
        bool shouldersAsSlides = true;
        bool touchpadAsSlides = true;
        bool dpadAsFaceButtons = true;
        // Optional DIVA multi-note macros. Bits: square=1, cross=2, circle=4, triangle=8.
        uint8_t l1MacroMask = 0;
        uint8_t r1MacroMask = 0;
        uint8_t l2MacroMask = 0;
        uint8_t r2MacroMask = 0;
    } input;

    struct Effects {
        float singleGain = 0.64f;
        float multiGain = 0.96f;
        // Extra strength by confirmed chord size. v0.4.5 prefers direct DIVA target counting and falls back to the confirmed
        // DIVA judgement with the time-stamped physical face-button mask.
        float multi2Multiplier = 1.10f;
        float multi3Multiplier = 1.28f;
        float multi4Multiplier = 1.48f;
        float slideGain = 0.90f;
        float holdGain = 0.18f;
        float chainGain = 0.23f;
        float chainFrequencyHz = 22.0f;
        // Exact GetHitState isSuccessNote flag (Chance/Success rainbow note).
        float successNoteGain = 1.10f;
        float challengeGain = 0.90f;
        float challengeNoteMultiplier = 1.12f;
    } effects;

    struct Judgement {
        bool enabled = true;
        // Strict mode means a raw physical press never becomes a gameplay haptic
        // by itself. A real DIVA judgement must confirm it first.
        bool strictValidation = true;
        float pressPreviewGain = 0.0f;
        float coolGain = 1.00f;
        float fineGain = 0.82f;
        float safeGain = 0.58f;
        float sadGain = 0.38f;
        float wrongGain = 0.32f;
        float worstGain = 0.10f;
        float overlayGain = 0.42f;
        int matchWindowMs = 180;
        int pendingTimeoutMs = 240;
        int gameplayPollTimeoutMs = 250;
        // If Mega Mix+ emits near-simultaneous judgement callbacks, group them as a
        // fallback chord signal. Physical confirmed chords normally use the HID history.
        int multiGroupWindowMs = 6;
        // How far around the confirmed judgement to look for the maximum
        // simultaneous face-button mask. This removes the HID-vs-game timing race.
        int physicalMultiWindowMs = 24;
        // Direct per-target game checks may occur just before or after the outer
        // COOL/FINE/etc. judgement. Correlate them inside this short window.
        int targetCorrelationWindowMs = 12;
        bool logEvents = false;
    } judgement;

    struct Menu {
        bool enabled = true;
        // UI actions are not fired on the controller edge. They are held briefly
        // and accepted only if the rendered menu reacts immediately afterwards.
        float navGain = 0.56f;
        float confirmGain = 0.88f;
        int validationWindowMs = 120;
        int sampleIntervalMs = 16;
        int settleMs = 80;
        // Pre-input frames must be quieter than this for the UI to count as
        // settled. This is intentionally strict to reject animated loading screens.
        float settleMaxDelta = 0.0045f;
        float navMinDelta = 0.0035f;
        float confirmMinDelta = 0.0060f;
        float motionFactor = 2.15f;
        float motionMargin = 0.0015f;
        float maxBaselineMotion = 0.020f;
        // Face-note mask used as menu confirm: square=1, cross=2, circle=4, triangle=8.
        uint8_t confirmMask = 0x02; // Cross / A in the default international PC UI.
        bool leftRightNavigation = true;
        bool stickNavigation = true;
        bool logEvents = false;
    } menu;

    struct Challenge {
        bool enabled = true;
        // The old black-bar detector is heuristic and can false-positive on PV
        // transitions. Keep it opt-in until an exact MODE_SELECT hook is used.
        bool visualDetection = false;
        int sampleIntervalMs = 160;
        float darkThreshold = 0.055f;
        float centerMinimum = 0.085f;
        float contrastMinimum = 0.040f;
        int enterSamples = 3;
        int exitSamples = 4;
    } challenge;

    static ModConfig Load(const std::filesystem::path& path);
};
