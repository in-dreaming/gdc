#include "rans.h"

#include <cstring>
#include <vector>

namespace gdc {
namespace rans {

namespace {

constexpr uint32_t SCALE_BITS = 12;
constexpr uint32_t SCALE = 1u << SCALE_BITS; // 4096
constexpr uint32_t RANS_L = 1u << 23;        // lower bound of normalization interval

// Normalize histogram to sum exactly SCALE, every present symbol >= 1.
bool normalizeFreqs(const uint64_t* hist, uint64_t total, uint16_t* freq) {
    if (total == 0) return false;
    uint32_t sum = 0;
    int maxSym = -1;
    uint32_t maxVal = 0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] == 0) { freq[i] = 0; continue; }
        uint32_t f = (uint32_t)((hist[i] * SCALE) / total);
        if (f == 0) f = 1;
        freq[i] = (uint16_t)f;
        sum += f;
        if (f > maxVal) { maxVal = f; maxSym = i; }
    }
    if (maxSym < 0) return false;
    // Fix rounding drift on the most frequent symbol first, then spread.
    if (sum != SCALE) {
        int64_t delta = (int64_t)SCALE - (int64_t)sum;
        if ((int64_t)freq[maxSym] + delta >= 1) {
            freq[maxSym] = (uint16_t)((int64_t)freq[maxSym] + delta);
        } else {
            // rare: shave from all symbols > 1 until it fits
            int64_t need = -delta;
            for (int i = 0; i < 256 && need > 0; i++) {
                while (freq[i] > 1 && need > 0) { freq[i]--; need--; }
            }
            if (need > 0) return false;
        }
    }
    return true;
}

} // namespace

size_t encode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap) {
    if (srcSize == 0 || dstCap < 512 + 4) return 0;

    uint64_t hist[256] = {0};
    for (size_t i = 0; i < srcSize; i++) hist[src[i]]++;
    uint16_t freq[256];
    if (!normalizeFreqs(hist, srcSize, freq)) return 0;
    uint32_t cum[257];
    cum[0] = 0;
    for (int i = 0; i < 256; i++) cum[i + 1] = cum[i] + freq[i];

    // Encode backwards into a temp buffer (emitted bytes end up in forward
    // consumption order for the decoder).
    thread_local std::vector<uint8_t> tmp;
    if (tmp.size() < srcSize + 64) tmp.resize(srcSize + 64);
    uint8_t* const tmpEnd = tmp.data() + tmp.size();
    uint8_t* out = tmpEnd;
    uint32_t x = RANS_L;
    for (size_t i = srcSize; i-- > 0;) {
        uint8_t s = src[i];
        uint32_t f = freq[s];
        uint32_t xMax = ((RANS_L >> SCALE_BITS) << 8) * f;
        while (x >= xMax) {
            if (out == tmp.data()) return 0; // cannot shrink, bail out
            *--out = (uint8_t)(x & 0xff);
            x >>= 8;
        }
        x = ((x / f) << SCALE_BITS) + (x % f) + cum[s];
    }
    size_t payload = (size_t)(tmpEnd - out);
    size_t encSize = 512 + 4 + payload;
    if (encSize >= srcSize || encSize > dstCap) return 0;

    uint8_t* d = dst;
    for (int i = 0; i < 256; i++) {
        d[0] = (uint8_t)(freq[i] & 0xff);
        d[1] = (uint8_t)(freq[i] >> 8);
        d += 2;
    }
    d[0] = (uint8_t)(x & 0xff);
    d[1] = (uint8_t)((x >> 8) & 0xff);
    d[2] = (uint8_t)((x >> 16) & 0xff);
    d[3] = (uint8_t)((x >> 24) & 0xff);
    d += 4;
    std::memcpy(d, out, payload);
    return encSize;
}

size_t decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    if (srcSize < 512 + 4) return 0;
    uint16_t freq[256];
    const uint8_t* p = src;
    for (int i = 0; i < 256; i++) {
        freq[i] = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
    }
    uint32_t cum[257];
    cum[0] = 0;
    for (int i = 0; i < 256; i++) {
        cum[i + 1] = cum[i] + freq[i];
        if (cum[i + 1] > SCALE) return 0;
    }
    if (cum[256] != SCALE) return 0;

    // slot -> symbol table
    thread_local std::vector<uint8_t> sym;
    if (sym.size() < SCALE) sym.resize(SCALE);
    for (int s = 0; s < 256; s++) {
        for (uint32_t k = cum[s]; k < cum[s + 1]; k++) sym[k] = (uint8_t)s;
    }

    uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    const uint8_t* const pend = src + srcSize;

    for (size_t i = 0; i < rawSize; i++) {
        uint32_t slot = x & (SCALE - 1);
        uint8_t s = sym[slot];
        dst[i] = s;
        x = (uint32_t)freq[s] * (x >> SCALE_BITS) + slot - cum[s];
        while (x < RANS_L) {
            if (p >= pend) return 0;
            x = (x << 8) | *p++;
        }
    }
    return rawSize;
}

} // namespace rans
} // namespace gdc
