#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace gdc {

using Clock = std::chrono::steady_clock;

inline double secondsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// QTSF compress types, matches the dataset's cfg encoding.
enum class CompType : uint32_t {
    None = 0,
    LZ4 = 1,
    LZ4HC = 2,
    OodleKraken = 3,
    OodleLeviathan = 4,
    OodleMermaid = 5,
    OodleSelkie = 6,
    LZ3 = 7,
    LZ3HUF = 8,
    ZSTD = 9,
    // local-only extensions, not part of the dataset encoding
    GDC0 = 100,
    GDC1 = 101,
    Count
};

const char* compTypeName(CompType t);
bool compTypeFromName(const std::string& name, CompType& out);

} // namespace gdc
