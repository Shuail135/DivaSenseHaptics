#include "DscProbe.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace {
constexpr int kParamCount[0x6B] = {
    0,1,4,2,2,2,7,4,2,6,2,1,6,2,1,1,
    3,2,3,5,5,4,4,5,2,0,2,4,2,2,1,21,
    0,3,2,5,1,1,7,1,1,2,1,2,1,2,3,3,
    1,2,2,3,6,6,1,1,2,3,1,2,2,4,4,1,
    2,1,2,1,1,3,3,3,2,1,9,3,2,4,2,3,
    2,24,1,2,1,3,1,3,4,1,2,6,3,2,3,3,
    4,1,1,3,3,4,2,3,3,8,2
};

uint32_t readU32(std::span<const uint8_t> b, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

bool plausibleTargetType(int32_t t) {
    // Public FT/Mega Mix documentation uses target IDs through the mid-20s.
    // Keep a little room for variants while rejecting arbitrary memory.
    return t >= 0 && t <= 31;
}

std::optional<DscProbeCandidate> validateFromTime(std::span<const uint8_t> bytes, size_t start) {
    if(start + 8 > bytes.size() || readU32(bytes, start) != 0x01) return std::nullopt;

    size_t pos = start;
    int commands = 0;
    int times = 0;
    int targets = 0;
    int modeSelect = 0;
    int flying = 0;
    int32_t lastTime = -1;
    bool ended = false;

    // DSCs are small compared with the process. This also bounds malformed data.
    constexpr size_t kMaxCandidateBytes = 12u * 1024u * 1024u;
    const size_t hardEnd = std::min(bytes.size(), start + kMaxCandidateBytes);

    while(pos + 4 <= hardEnd && commands < 250000) {
        const uint32_t opcode = readU32(bytes, pos);
        pos += 4;
        if(opcode > 0x6A) return std::nullopt;
        if(opcode == 0x00) {
            ended = true;
            ++commands;
            break;
        }
        const int params = kParamCount[opcode];
        const size_t need = static_cast<size_t>(params) * 4u;
        if(pos + need > hardEnd) return std::nullopt;

        if(opcode == 0x01) {
            const int32_t t = static_cast<int32_t>(readU32(bytes, pos));
            // TIME is 1/100000 sec. Reject negative/non-monotonic/extreme values.
            if(t < 0 || t < lastTime || t > 720000000) return std::nullopt;
            lastTime = t;
            ++times;
        } else if(opcode == 0x06) {
            const int32_t type = static_cast<int32_t>(readU32(bytes, pos));
            if(!plausibleTargetType(type)) return std::nullopt;
            ++targets;
        } else if(opcode == 0x1A) {
            ++modeSelect;
        } else if(opcode == 0x1C || opcode == 0x3A) {
            ++flying;
        }

        pos += need;
        ++commands;
    }

    if(!ended) return std::nullopt;
    // A real playable chart has many TIME/TARGET commands. These thresholds make
    // accidental matches in heap data vanishingly unlikely while still allowing
    // short Easy charts/tutorials.
    if(commands < 40 || times < 8 || targets < 16) return std::nullopt;

    DscProbeCandidate out;
    out.offset = start;
    out.length = pos - start;
    out.commandCount = commands;
    out.timeCount = times;
    out.targetCount = targets;
    out.score = targets * 100 + times * 8 + commands + modeSelect * 20 + flying * 5;
    return out;
}
}

std::optional<DscProbeCandidate> FindBestDscCandidate(std::span<const uint8_t> bytes) {
    if(bytes.size() < 64) return std::nullopt;
    std::optional<DscProbeCandidate> best;

    // Memory regions/chunks are page aligned, so a 4-byte stride preserves DSC
    // word alignment. Every suffix TIME is a possible start; the earliest TIME in
    // the real chart wins naturally because it contains the most TARGETs.
    for(size_t off = 0; off + 8 <= bytes.size(); off += 4) {
        if(readU32(bytes, off) != 0x01) continue;
        auto c = validateFromTime(bytes, off);
        if(!c) continue;
        if(!best || c->score > best->score ||
           (c->score == best->score && c->length > best->length)) {
            best = *c;
        }
    }
    return best;
}
