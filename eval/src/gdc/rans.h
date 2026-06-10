#pragma once

#include <cstddef>
#include <cstdint>

namespace gdc {
namespace rans {

// Order-0 byte rANS, 12-bit normalized frequencies (ryg-style).
// Stream layout: [u16 freq[256] = 512B][u32 final state][payload].
// Returns encoded size, or 0 if encoding failed / did not shrink below srcSize.
size_t encode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap);

// Returns rawSize on success, 0 on failure/corruption.
size_t decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize);

// Worst-case encoded size for srcSize input.
inline size_t bound(size_t srcSize) { return srcSize + srcSize / 64 + 512 + 4 + 64; }

} // namespace rans
} // namespace gdc
