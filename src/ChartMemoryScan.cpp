#ifdef _WIN32
#include "ChartMemoryScan.h"
#include "DscProbe.h"
#include "Log.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
bool readablePrivate(const MEMORY_BASIC_INFORMATION& mbi) {
    if(mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) return false;
    if((mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
    const DWORD p = mbi.Protect & 0xffu;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for(size_t i=0;i<n;++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

std::wstring memoryName(uintptr_t address, uint64_t hash) {
    std::wostringstream ss;
    ss << L"<memory-dsc@0x" << std::hex << address << L"#" << hash << L">";
    return ss.str();
}
}

ChartMemoryScan::ChartMemoryScan(Callback callback) : callback_(std::move(callback)) {}
ChartMemoryScan::~ChartMemoryScan(){ Stop(); }

bool ChartMemoryScan::Start() {
    if(thread_.joinable()) return true;
    stop_ = false;
    thread_ = std::thread(&ChartMemoryScan::threadMain, this);
    Log::Info("DSC memory fallback enabled; if file capture misses base-game charts, readable game heap memory will be scanned during gameplay.");
    return true;
}

void ChartMemoryScan::Stop() {
    stop_ = true;
    {
        std::lock_guard lock(mutex_);
        requested_ = true;
    }
    cv_.notify_all();
    if(thread_.joinable()) thread_.join();
}

void ChartMemoryScan::Request() {
    if(stop_ || runningScan_) return;
    {
        std::lock_guard lock(mutex_);
        requested_ = true;
    }
    cv_.notify_one();
}

void ChartMemoryScan::threadMain() {
    while(!stop_) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock,[&]{ return stop_ || requested_; });
            if(stop_) break;
            requested_ = false;
        }
        runningScan_ = true;
        scanOnce();
        runningScan_ = false;
    }
}

bool ChartMemoryScan::scanOnce() {
    HANDLE process = GetCurrentProcess();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    struct Best {
        int score = -1;
        int targets = 0;
        int times = 0;
        int commands = 0;
        uintptr_t address = 0;
        std::vector<uint8_t> bytes;
    } best;

    constexpr SIZE_T kWindow = 16u * 1024u * 1024u;
    constexpr SIZE_T kStep = 8u * 1024u * 1024u; // overlap catches charts crossing a scan window
    size_t regions = 0;
    size_t readableBytes = 0;

    while(address < maximum && !stop_) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const SIZE_T regionSize = mbi.RegionSize;
        uintptr_t next = base + regionSize;
        if(next <= address) break;

        if(readablePrivate(mbi) && regionSize >= 4096) {
            ++regions;
            readableBytes += static_cast<size_t>(regionSize);
            for(SIZE_T rel = 0; rel < regionSize && !stop_; rel += kStep) {
                const SIZE_T want = std::min<SIZE_T>(kWindow, regionSize - rel);
                if(want < 64) break;
                std::vector<uint8_t> buf(static_cast<size_t>(want));
                SIZE_T got = 0;
                if(!ReadProcessMemory(process, reinterpret_cast<const void*>(base + rel),
                                      buf.data(), want, &got) || got < 64) {
                    continue;
                }
                buf.resize(static_cast<size_t>(got));
                auto candidate = FindBestDscCandidate(std::span<const uint8_t>(buf.data(),buf.size()));
                if(!candidate) continue;
                if(candidate->offset + candidate->length > buf.size()) continue;
                if(candidate->score <= best.score) continue;

                best.score = candidate->score;
                best.targets = candidate->targetCount;
                best.times = candidate->timeCount;
                best.commands = candidate->commandCount;
                best.address = base + rel + candidate->offset;
                best.bytes.assign(buf.begin() + static_cast<std::ptrdiff_t>(candidate->offset),
                                  buf.begin() + static_cast<std::ptrdiff_t>(candidate->offset + candidate->length));
            }
        }
        address = next;
    }

    if(best.bytes.empty()) {
        // Avoid spamming the log every retry; the first two misses are enough to
        // diagnose whether the active DSC survives in process memory.
        if(noFindLogs_ < 2) {
            Log::Warn("DSC memory scan found no validated chart candidate (private regions=" +
                      std::to_string(regions) + ", scanned=" +
                      std::to_string(readableBytes / (1024u*1024u)) + " MiB). Retrying while gameplay is active.");
            ++noFindLogs_;
        }
        return false;
    }

    const uint64_t hash = fnv1a(best.bytes.data(), best.bytes.size());
    auto chart = DscChart::Parse(best.bytes, memoryName(best.address,hash));
    if(!chart.valid) {
        if(noFindLogs_ < 2) {
            Log::Warn("DSC memory scan found a structural candidate but the gameplay parser rejected it; retrying.");
            ++noFindLogs_;
        }
        return false;
    }

    noFindLogs_ = 0;
    Log::Info("Recovered DIVA DSC from process memory: address=0x" + [&]{
        std::ostringstream ss; ss << std::hex << best.address; return ss.str();
    }() + ", bytes=" + std::to_string(best.bytes.size()) +
    ", targets=" + std::to_string(best.targets) +
    ", TIME commands=" + std::to_string(best.times) +
    ", commands=" + std::to_string(best.commands) + ".");

    if(callback_) callback_(std::move(chart));
    return true;
}
#endif
