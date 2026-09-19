#ifdef _WIN32
#include "JudgementHook.h"
#include "Log.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

JudgementHook* JudgementHook::active_ = nullptr;

namespace {
constexpr std::array<uint8_t,11> kHitStateAnchor{
    0xE8,0x00,0x00,0x00,0x00,0x48,0x8B,0x4D,0xE8,0x89,0x01
};
constexpr char kHitStateMask[] = "x????xxxxxx";

// Published by score-mm as sigHitStateInternal. v0.4.5 locates this function,
// but hooks the main executable's direct CALL sites instead of overwriting the
// function entry. This mirrors the observed game path more safely without a
// third-party detour library.
constexpr std::array<uint8_t,7> kInternalHitAnchor{
    0x66,0x44,0x89,0x4C,0x24,0x00,0x53
};
constexpr char kInternalHitMask[] = "xxxxx?x";

struct CapturedTargetHit {
    void* target = nullptr;
    int32_t grade = 21;
};

thread_local bool gCaptureTargets = false;
thread_local std::array<CapturedTargetHit,16> gCapturedTargets{};
thread_local int gCapturedTargetCount = 0;

bool matchesPattern(const uint8_t* p, const uint8_t* bytes, const char* mask, size_t size) {
    for(size_t i=0;i<size;++i) {
        if(mask[i]=='x' && p[i]!=bytes[i]) return false;
    }
    return true;
}

uint8_t* findUniqueExecutablePattern(const uint8_t* bytes, const char* mask, size_t patternSize,
                                     const char* duplicateWarning) {
    auto* module=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if(!module) return nullptr;
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(module+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    uint8_t* found=nullptr;
    int count=0;
    auto* sec=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)==0) continue;
        const size_t size=std::max<size_t>(sec[i].Misc.VirtualSize,sec[i].SizeOfRawData);
        if(size<patternSize) continue;
        auto* begin=module+sec[i].VirtualAddress;
        auto* end=begin+size-patternSize+1;
        for(auto* p=begin;p<end;++p) {
            if(!matchesPattern(p,bytes,mask,patternSize)) continue;
            found=p;
            ++count;
            if(count>1) {
                Log::Warn(duplicateWarning);
                return nullptr;
            }
        }
    }
    return count==1 ? found : nullptr;
}

uintptr_t alignUp(uintptr_t v, uintptr_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

std::string hexRva(const void* address) {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto p=reinterpret_cast<uintptr_t>(address);
    std::ostringstream ss;
    ss << "0x" << std::hex << (p>=base ? p-base : p);
    return ss.str();
}

int directTargetCountForOuterResult(int32_t outerResult) {
    if(gCapturedTargetCount<=0) return 0;

    int sameGrade=0;
    int validTotal=0;
    for(int i=0;i<gCapturedTargetCount;++i) {
        const auto& h=gCapturedTargets[static_cast<size_t>(i)];
        if(h.grade>=0 && h.grade<=8) {
            ++validTotal;
            if(h.grade==outerResult) ++sameGrade;
        }
    }

    // Matching the outer judgement is the strongest signal. If the game used
    // mixed WRONG subtypes for a chord, fall back to the total number of unique
    // target objects, but only inside the legal DIVA chord range.
    if(sameGrade>=1 && sameGrade<=4) return sameGrade;
    if(validTotal>=1 && validTotal<=4) return validTotal;
    return 0;
}
}

uint8_t* JudgementHook::FindHitStateCallSite() {
    return findUniqueExecutablePattern(kHitStateAnchor.data(),kHitStateMask,kHitStateAnchor.size(),
        "DIVA judgement signature matched more than once; judgement hook disabled for safety.");
}

uint8_t* JudgementHook::FindInternalHitState() {
    return findUniqueExecutablePattern(kInternalHitAnchor.data(),kInternalHitMask,kInternalHitAnchor.size(),
        "DIVA internal target-hit signature matched more than once; direct chord counting disabled for safety.");
}

bool JudgementHook::IsExecutableAddress(const void* address) {
    if(!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if(VirtualQuery(address,&mbi,sizeof(mbi))!=sizeof(mbi) || mbi.State!=MEM_COMMIT) return false;
    const DWORD p=mbi.Protect & 0xffu;
    return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}

void* JudgementHook::AllocateRelayNear(void* address,size_t bytes) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran=std::max<uintptr_t>(si.dwAllocationGranularity,0x10000u);
    const uintptr_t target=reinterpret_cast<uintptr_t>(address);
    constexpr uintptr_t kRange=0x7fff0000ull;
    const uintptr_t low=target>kRange ? target-kRange : 0x10000ull;
    const uintptr_t high=target<std::numeric_limits<uintptr_t>::max()-kRange
        ? target+kRange : std::numeric_limits<uintptr_t>::max();

    uintptr_t cursor=low;
    while(cursor<high) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(VirtualQuery(reinterpret_cast<void*>(cursor),&mbi,sizeof(mbi))!=sizeof(mbi)) break;
        const uintptr_t base=reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t end=base+mbi.RegionSize;
        if(end<=cursor) break;
        if(mbi.State==MEM_FREE) {
            uintptr_t candidate=alignUp(std::max(base,low),gran);
            if(candidate>=base && candidate+bytes<=end && candidate+bytes<=high) {
                if(void* p=VirtualAlloc(reinterpret_cast<void*>(candidate),bytes,
                                        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)) {
                    const int64_t disp=reinterpret_cast<int64_t>(p)-
                                       (reinterpret_cast<int64_t>(address)+5);
                    if(disp>=std::numeric_limits<int32_t>::min() && disp<=std::numeric_limits<int32_t>::max())
                        return p;
                    VirtualFree(p,0,MEM_RELEASE);
                }
            }
        }
        cursor=std::max(cursor+gran,end);
    }
    return nullptr;
}

std::vector<uint8_t*> JudgementHook::FindDirectCallsTo(void* targetPtr) {
    std::vector<uint8_t*> out;
    auto* module=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* target=reinterpret_cast<uint8_t*>(targetPtr);
    if(!module || !target) return out;
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return out;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(module+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return out;

    auto* sec=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)==0) continue;
        const size_t size=std::max<size_t>(sec[i].Misc.VirtualSize,sec[i].SizeOfRawData);
        if(size<5) continue;
        auto* begin=module+sec[i].VirtualAddress;
        auto* end=begin+size-5+1;
        for(auto* p=begin;p<end;++p) {
            if(p[0]!=0xE8) continue;
            int32_t disp=0;
            std::memcpy(&disp,p+1,sizeof(disp));
            auto* dest=p+5+static_cast<int64_t>(disp);
            if(dest==target) out.push_back(p);
        }
    }
    return out;
}

bool JudgementHook::InstallInternalHook() {
    if(DirectTargetCountingInstalled()) return true;
    auto* target=FindInternalHitState();
    if(!target) {
        Log::Warn("DIVA internal target-hit function was not found; chord size will use controller/grouping fallbacks.");
        return false;
    }
    if(!IsExecutableAddress(target)) {
        Log::Warn("DIVA internal target-hit signature resolved to non-executable memory; direct chord counting disabled.");
        return false;
    }

    const auto callSites=FindDirectCallsTo(target);
    if(callSites.empty()) {
        Log::Warn("DIVA internal target-hit function was found at " + hexRva(target) +
                  " but no direct CALL sites reference it; direct chord counting disabled.");
        return false;
    }

    // A single relay is enough because all callers and the target live in the
    // main executable image and are well inside rel32 range of one another.
    internalRelay_=AllocateRelayNear(callSites.front(),0x1000);
    if(!internalRelay_) {
        Log::Warn("Could not allocate a near relay for DIVA internal target-hit call sites; direct chord counting disabled.");
        return false;
    }

    std::array<uint8_t,12> relayCode{0x48,0xB8};
    const uint64_t thunk=reinterpret_cast<uint64_t>(&JudgementHook::InternalHookThunk);
    std::memcpy(relayCode.data()+2,&thunk,sizeof(thunk));
    relayCode[10]=0xFF; relayCode[11]=0xE0;
    std::memcpy(internalRelay_,relayCode.data(),relayCode.size());
    DWORD relayOld=0;
    if(!VirtualProtect(internalRelay_,0x1000,PAGE_EXECUTE_READ,&relayOld)) {
        VirtualFree(internalRelay_,0,MEM_RELEASE); internalRelay_=nullptr;
        Log::Warn("Could not make DIVA internal target-hit relay executable; direct chord counting disabled.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(),internalRelay_,relayCode.size());

    originalInternal_=reinterpret_cast<CheckHitStateInternalFn>(target);
    internalTarget_=target;
    internalSawCall_.store(false);

    for(auto* site : callSites) {
        const int64_t disp64=reinterpret_cast<int64_t>(internalRelay_)-
                             (reinterpret_cast<int64_t>(site)+5);
        if(disp64<std::numeric_limits<int32_t>::min() || disp64>std::numeric_limits<int32_t>::max())
            continue;

        InternalCallPatch patch{};
        patch.site=site;
        std::memcpy(patch.original.data(),site,patch.original.size());
        if(patch.original[0]!=0xE8) continue;
        patch.patched=patch.original;
        const int32_t disp=static_cast<int32_t>(disp64);
        std::memcpy(patch.patched.data()+1,&disp,sizeof(disp));

        DWORD oldProtect=0;
        if(!VirtualProtect(site,patch.patched.size(),PAGE_EXECUTE_READWRITE,&oldProtect))
            continue;
        std::memcpy(site,patch.patched.data(),patch.patched.size());
        FlushInstructionCache(GetCurrentProcess(),site,patch.patched.size());
        DWORD ignored=0;
        VirtualProtect(site,patch.patched.size(),oldProtect,&ignored);
        internalCallPatches_.push_back(patch);
    }

    if(internalCallPatches_.empty()) {
        originalInternal_=nullptr;
        internalTarget_=nullptr;
        VirtualFree(internalRelay_,0,MEM_RELEASE); internalRelay_=nullptr;
        Log::Warn("DIVA internal target-hit CALL sites could not be patched; direct chord counting disabled.");
        return false;
    }

    Log::Info("DIVA direct target counter installed: target RVA " + hexRva(target) +
              ", intercepted CALL sites=" + std::to_string(internalCallPatches_.size()) + ".");
    return true;
}

void JudgementHook::RemoveInternalHook() {
    for(auto& patch : internalCallPatches_) {
        if(!patch.site) continue;
        if(std::memcmp(patch.site,patch.patched.data(),patch.patched.size())==0) {
            DWORD oldProtect=0;
            if(VirtualProtect(patch.site,patch.original.size(),PAGE_EXECUTE_READWRITE,&oldProtect)) {
                std::memcpy(patch.site,patch.original.data(),patch.original.size());
                FlushInstructionCache(GetCurrentProcess(),patch.site,patch.original.size());
                DWORD ignored=0;
                VirtualProtect(patch.site,patch.original.size(),oldProtect,&ignored);
            }
        } else {
            Log::Warn("A DIVA internal target-hit CALL site changed after installation; leaving the newer patch intact during shutdown.");
        }
    }
    internalCallPatches_.clear();
    internalTarget_=nullptr;
    originalInternal_=nullptr;
    internalSawCall_.store(false);
    if(internalRelay_){VirtualFree(internalRelay_,0,MEM_RELEASE);internalRelay_=nullptr;}
}

bool JudgementHook::Start() {
    if(callSite_) return true;
    if(active_) {
        Log::Warn("DIVA judgement hook already has an active instance.");
        return false;
    }

    uint8_t* site=FindHitStateCallSite();
    if(!site) {
        Log::Warn("DIVA GetHitState signature was not found; strict gameplay haptics will stay disabled for safety.");
        return false;
    }

    int32_t oldDisp=0;
    std::memcpy(&oldDisp,site+1,sizeof(oldDisp));
    auto* target=site+5+static_cast<int64_t>(oldDisp);
    if(!IsExecutableAddress(target)) {
        Log::Warn("DIVA GetHitState signature resolved to a non-executable address; judgement hook disabled.");
        return false;
    }
    original_=reinterpret_cast<GetHitStateFn>(target);

    relay_=AllocateRelayNear(site,0x1000);
    if(!relay_) {
        Log::Warn("Could not allocate a near relay for the DIVA judgement hook; strict gameplay haptics will stay disabled for safety.");
        original_=nullptr;
        return false;
    }

    std::array<uint8_t,12> relayCode{0x48,0xB8};
    const uint64_t thunk=reinterpret_cast<uint64_t>(&JudgementHook::HookThunk);
    std::memcpy(relayCode.data()+2,&thunk,sizeof(thunk));
    relayCode[10]=0xFF;
    relayCode[11]=0xE0;
    std::memcpy(relay_,relayCode.data(),relayCode.size());
    DWORD relayOld=0;
    if(!VirtualProtect(relay_,0x1000,PAGE_EXECUTE_READ,&relayOld)) {
        VirtualFree(relay_,0,MEM_RELEASE); relay_=nullptr; original_=nullptr;
        Log::Warn("Could not make the DIVA judgement relay executable; strict gameplay haptics will stay disabled for safety.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(),relay_,relayCode.size());

    const int64_t newDisp64=reinterpret_cast<int64_t>(relay_)-
                            (reinterpret_cast<int64_t>(site)+5);
    if(newDisp64<std::numeric_limits<int32_t>::min() || newDisp64>std::numeric_limits<int32_t>::max()) {
        VirtualFree(relay_,0,MEM_RELEASE); relay_=nullptr; original_=nullptr;
        Log::Warn("DIVA judgement relay was allocated outside rel32 range; strict gameplay haptics will stay disabled for safety.");
        return false;
    }
    const int32_t newDisp=static_cast<int32_t>(newDisp64);
    std::memcpy(originalBytes_.data(),site,originalBytes_.size());
    patchedBytes_=originalBytes_;
    patchedBytes_[0]=0xE8;
    std::memcpy(patchedBytes_.data()+1,&newDisp,sizeof(newDisp));

    DWORD oldProtect=0;
    if(!VirtualProtect(site,patchedBytes_.size(),PAGE_EXECUTE_READWRITE,&oldProtect)) {
        VirtualFree(relay_,0,MEM_RELEASE); relay_=nullptr; original_=nullptr;
        Log::Warn("Could not make the DIVA GetHitState call site writable; strict gameplay haptics will stay disabled for safety.");
        return false;
    }
    active_=this;
    std::memcpy(site,patchedBytes_.data(),patchedBytes_.size());
    FlushInstructionCache(GetCurrentProcess(),site,patchedBytes_.size());
    DWORD ignored=0;
    VirtualProtect(site,patchedBytes_.size(),oldProtect,&ignored);
    callSite_=site;

    Log::Info("DIVA judgement hook installed: callsite RVA " + hexRva(site) +
              ", GetHitState target " + hexRva(target) + ".");

    // v0.5.0 no longer patches the unused internal target routine. Live testing
    // showed that path does not execute for the retail gameplay route being used.
    // Exact chord/mechanic identity now comes from the loaded DSC chart instead.
    Log::Info("Internal target-count hook disabled; DSC chart TARGET groups are the preferred exact note source.");
    return true;
}

void JudgementHook::Stop() {
    RemoveInternalHook();

    if(!callSite_) {
        if(active_==this) active_=nullptr;
        if(relay_){VirtualFree(relay_,0,MEM_RELEASE);relay_=nullptr;}
        original_=nullptr;
        return;
    }

    if(std::memcmp(callSite_,patchedBytes_.data(),patchedBytes_.size())==0) {
        DWORD oldProtect=0;
        if(VirtualProtect(callSite_,originalBytes_.size(),PAGE_EXECUTE_READWRITE,&oldProtect)) {
            std::memcpy(callSite_,originalBytes_.data(),originalBytes_.size());
            FlushInstructionCache(GetCurrentProcess(),callSite_,originalBytes_.size());
            DWORD ignored=0;
            VirtualProtect(callSite_,originalBytes_.size(),oldProtect,&ignored);
        }
    } else {
        Log::Warn("DIVA judgement call site changed after installation; leaving the newer patch intact during shutdown.");
    }

    if(active_==this) active_=nullptr;
    callSite_=nullptr;
    original_=nullptr;
    if(relay_){VirtualFree(relay_,0,MEM_RELEASE);relay_=nullptr;}
}

int32_t __fastcall JudgementHook::InternalHookThunk(
    void* gameState,void* target,uint16_t a3,uint16_t a4) {
    JudgementHook* self=active_;
    if(!self || !self->originalInternal_) return 21;

    const int32_t result=self->originalInternal_(gameState,target,a3,a4);
    if(!self->internalSawCall_.exchange(true))
        Log::Info("DIVA internal target hook is executing; first raw result=" + std::to_string(result) + ".");
    if(result>=0 && result<=8)
        self->haptics_.OnInternalTargetHit(target,result);
    if(gCaptureTargets && result>=0 && result<=8) {
        bool seen=false;
        for(int i=0;i<gCapturedTargetCount;++i) {
            if(gCapturedTargets[static_cast<size_t>(i)].target==target) {
                // Keep the weakest/worst result if the same target is evaluated
                // more than once during one outer GetHitState call.
                if(result>gCapturedTargets[static_cast<size_t>(i)].grade)
                    gCapturedTargets[static_cast<size_t>(i)].grade=result;
                seen=true;
                break;
            }
        }
        if(!seen && gCapturedTargetCount<static_cast<int>(gCapturedTargets.size())) {
            gCapturedTargets[static_cast<size_t>(gCapturedTargetCount++)]={target,result};
        }
    }
    return result;
}

int32_t __fastcall JudgementHook::HookThunk(
    void* game,bool* playDefaultSe,void* ratingCount,void* ratingPos,
    int32_t a5,void* soundEffect,uint32_t* multiCount,uint32_t* playerHitTimeBits,
    int32_t* targetIndex,bool* isSuccessNote,bool* slide,bool* slideChain,
    bool* slideChainStart,bool* slideChainMax,bool* slideChainContinues,void* a16) {

    JudgementHook* self=active_;
    if(!self || !self->original_) return 21;

    // Also keep the older nested capture path when the game happens to invoke
    // the target checker inside this call. legacy diagnostic code no longer depends on that call
    // order; InternalHookThunk records target checks continuously as well.
    const bool previousCapture=gCaptureTargets;
    const auto previousTargets=gCapturedTargets;
    const int previousTargetCount=gCapturedTargetCount;
    gCaptureTargets=self->originalInternal_!=nullptr;
    gCapturedTargetCount=0;
    gCapturedTargets={};

    const int32_t result=self->original_(
        game,playDefaultSe,ratingCount,ratingPos,a5,soundEffect,multiCount,
        playerHitTimeBits,targetIndex,isSuccessNote,slide,slideChain,slideChainStart,
        slideChainMax,slideChainContinues,a16);

    const int directTargetCount=(result!=21 && gCaptureTargets)
        ? directTargetCountForOuterResult(result) : 0;

    gCaptureTargets=previousCapture;
    gCapturedTargets=previousTargets;
    gCapturedTargetCount=previousTargetCount;

    self->haptics_.OnGamePoll();
    if(result!=21) {
        self->haptics_.OnJudgement(
            result,
            slide ? *slide : false,
            slideChain ? *slideChain : false,
            slideChainStart ? *slideChainStart : false,
            slideChainMax ? *slideChainMax : false,
            slideChainContinues ? *slideChainContinues : false,
            isSuccessNote ? *isSuccessNote : false,
            multiCount ? static_cast<int>(*multiCount) : 0,
            directTargetCount);
    }
    return result;
}

#endif
