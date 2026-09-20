#pragma once
#ifdef _WIN32

#include "JudgementHaptics.h"
#include <array>
#include <cstddef>
#include <cstdint>

class JudgementHook {
public:
    explicit JudgementHook(JudgementHaptics& haptics) : haptics_(haptics) {}
    ~JudgementHook() { Stop(); }

    bool Start();
    void Stop();

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
		
	using CheckHitStateInternalFn = int32_t (__fastcall *)(
		void* game,
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
		void* game,
		void* target,
		uint16_t a3,
		uint16_t a4);

    static uint8_t* FindHitStateCallSite();
	static uint8_t* FindHitStateInternal();
    static void* AllocateRelayNear(void* address, size_t bytes);
    static bool IsExecutableAddress(const void* address);

    JudgementHaptics& haptics_;
    GetHitStateFn original_ = nullptr;
	CheckHitStateInternalFn originalInternal_ = nullptr;

	void* internalTarget_ = nullptr;
	bool internalHookInstalled_ = false;
	bool minHookOwnsInit_ = false;
	
    uint8_t* callSite_ = nullptr;
    void* relay_ = nullptr;
    std::array<uint8_t,5> originalBytes_{};
    std::array<uint8_t,5> patchedBytes_{};

    static JudgementHook* active_;
};

#endif
