#pragma once
#ifdef _WIN32

#include "JudgementHaptics.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class JudgementHook {
public:
    explicit JudgementHook(JudgementHaptics& haptics) : haptics_(haptics) {}
    ~JudgementHook() { Stop(); }

    bool Start();
    void Stop();
    bool Installed() const { return callSite_ != nullptr; }
    bool DirectTargetCountingInstalled() const { return internalTarget_ != nullptr && !internalCallPatches_.empty(); }

private:
    using GetHitStateFn = int32_t (__fastcall *)(
        void* game,
        bool* playDefaultSe,
        void* ratingCount,
        void* ratingPos,
        int32_t a5,
        void* soundEffect,
        uint32_t* multiCount,
        uint32_t* playerHitTimeBits,
        int32_t* targetIndex,
        bool* isSuccessNote,
        bool* slide,
        bool* slideChain,
        bool* slideChainStart,
        bool* slideChainMax,
        bool* slideChainContinues,
        void* a16);

    // Published by score-mm. This routine evaluates one DIVA target object.
    // v0.4.5 deliberately leaves the routine's prologue untouched and hooks the
    // game's direct CALL instructions that invoke it instead. That is less
    // fragile than our earlier hand-written entry trampoline while keeping this
    // project self-contained (no MinHook dependency).
    using CheckHitStateInternalFn = int32_t (__fastcall *)(
        void* gameState,
        void* target,
        uint16_t a3,
        uint16_t a4);

    static int32_t __fastcall HookThunk(
        void* game,
        bool* playDefaultSe,
        void* ratingCount,
        void* ratingPos,
        int32_t a5,
        void* soundEffect,
        uint32_t* multiCount,
        uint32_t* playerHitTimeBits,
        int32_t* targetIndex,
        bool* isSuccessNote,
        bool* slide,
        bool* slideChain,
        bool* slideChainStart,
        bool* slideChainMax,
        bool* slideChainContinues,
        void* a16);

    static int32_t __fastcall InternalHookThunk(
        void* gameState,
        void* target,
        uint16_t a3,
        uint16_t a4);

    struct InternalCallPatch {
        uint8_t* site = nullptr;
        std::array<uint8_t,5> original{};
        std::array<uint8_t,5> patched{};
    };

    static uint8_t* FindHitStateCallSite();
    static uint8_t* FindInternalHitState();
    static std::vector<uint8_t*> FindDirectCallsTo(void* target);
    static void* AllocateRelayNear(void* address, size_t bytes);
    static bool IsExecutableAddress(const void* address);
    bool InstallInternalHook();
    void RemoveInternalHook();

    JudgementHaptics& haptics_;
    GetHitStateFn original_ = nullptr;
    uint8_t* callSite_ = nullptr;
    void* relay_ = nullptr;
    std::array<uint8_t,5> originalBytes_{};
    std::array<uint8_t,5> patchedBytes_{};

    CheckHitStateInternalFn originalInternal_ = nullptr;
    uint8_t* internalTarget_ = nullptr;
    void* internalRelay_ = nullptr;
    std::vector<InternalCallPatch> internalCallPatches_;
    std::atomic<bool> internalSawCall_{false};

    static JudgementHook* active_;
};

#endif
