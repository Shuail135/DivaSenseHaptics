#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

struct DscProbeCandidate {
    size_t offset = 0;
    size_t length = 0;
    int commandCount = 0;
    int timeCount = 0;
    int targetCount = 0;
    int score = 0;
};

// Searches a byte range for a plausible raw FT/Mega Mix DSC command stream.
// The returned candidate starts at its first TIME command and ends after END.
// Starting at TIME is intentional: it avoids depending on the unknown 4-byte
// DSC header while retaining all gameplay TARGET/MODE_SELECT commands.
std::optional<DscProbeCandidate> FindBestDscCandidate(std::span<const uint8_t> bytes);
