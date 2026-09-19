#ifdef _WIN32
#include "JudgementHook.h"
#include "Log.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>

JudgementHook* JudgementHook::active_ = nullptr;

namespace {
constexpr std::array<uint8_t,11> kHitStateAnchor{
    0xE8,0x00,0x00,0x00,0x00,0x48,0x8B,0x4D,0xE8,0x89,0x01
};
constexpr char kHitStateMask[] = "x????xxxxxx";

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

}

uint8_t* JudgementHook::FindHitStateCallSite() {
    return findUniqueExecutablePattern(kHitStateAnchor.data(),kHitStateMask,kHitStateAnchor.size(),
        "DIVA judgement signature matched more than once; judgement hook disabled for safety.");
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

    return true;
}

void JudgementHook::Stop() {
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

int32_t __fastcall JudgementHook::HookThunk(
    void* game,bool* playDefaultSe,void* ratingCount,void* ratingPos,
    int32_t a5,void* soundEffect,uint32_t* multiCount,uint32_t* playerHitTimeBits,
    int32_t* targetIndex,bool* isSuccessNote,bool* slide,bool* slideChain,
    bool* slideChainStart,bool* slideChainMax,bool* slideChainContinues,void* a16) {

    JudgementHook* self=active_;
    if(!self || !self->original_) return 21;

    const int32_t result=self->original_(
        game,playDefaultSe,ratingCount,ratingPos,a5,soundEffect,multiCount,
        playerHitTimeBits,targetIndex,isSuccessNote,slide,slideChain,slideChainStart,
        slideChainMax,slideChainContinues,a16);

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
            multiCount ? static_cast<int>(*multiCount) : 0);
    }
    return result;
}

#endif
