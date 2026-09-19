#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct DscChartGroup {
    double hitSeconds = 0.0;
    uint8_t faceMask = 0;      // current mod bits: square=1,cross=2,circle=4,triangle=8
    uint8_t holdMask = 0;
    uint8_t slideMask = 0;     // bit0 left, bit1 right
    uint8_t slideTargetCount = 0;
    uint8_t chainMask = 0;
    uint8_t specialFaceMask = 0;
    uint8_t specialSlideMask = 0;
    bool challenge = false;
};

struct DscChallengeMarker {
    double seconds = 0.0;
    bool start = false;
};

struct DscChart {
    std::wstring sourcePath;
    std::vector<DscChartGroup> groups;
    std::vector<DscChallengeMarker> challengeMarkers;
    bool valid = false;

    static DscChart Parse(const std::vector<uint8_t>& bytes, const std::wstring& sourcePath,
                          int groupWindowMs = 2, int chainGapMs = 130);
};
