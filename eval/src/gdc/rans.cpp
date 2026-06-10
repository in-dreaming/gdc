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

// Compact freq table serialization (typ. 60-250B instead of 512B):
//   byte 0x00:        zero run; next byte = run length (1..255)
//   byte 1..254:      freq value, one symbol
//   byte 0xFF:        escape; next 2 bytes = u16 LE freq (255..4096)
size_t writeFreqTable(const uint16_t* freq, uint8_t* dst, size_t cap) {
    size_t n = 0;
    int i = 0;
    while (i < 256) {
        if (freq[i] == 0) {
            int run = 0;
            while (i < 256 && freq[i] == 0 && run < 255) { run++; i++; }
            if (n + 2 > cap) return 0;
            dst[n++] = 0x00;
            dst[n++] = (uint8_t)run;
        } else if (freq[i] < 255) {
            if (n + 1 > cap) return 0;
            dst[n++] = (uint8_t)freq[i];
            i++;
        } else {
            if (n + 3 > cap) return 0;
            dst[n++] = 0xFF;
            dst[n++] = (uint8_t)(freq[i] & 0xff);
            dst[n++] = (uint8_t)(freq[i] >> 8);
            i++;
        }
    }
    return n;
}

// Returns bytes consumed, 0 on corruption.
size_t readFreqTable(const uint8_t* src, size_t srcSize, uint16_t* freq) {
    size_t p = 0;
    int i = 0;
    while (i < 256) {
        if (p >= srcSize) return 0;
        uint8_t b = src[p++];
        if (b == 0x00) {
            if (p >= srcSize) return 0;
            int run = src[p++];
            if (run == 0 || i + run > 256) return 0;
            for (int k = 0; k < run; k++) freq[i++] = 0;
        } else if (b == 0xFF) {
            if (p + 2 > srcSize) return 0;
            freq[i++] = (uint16_t)(src[p] | (src[p + 1] << 8));
            p += 2;
        } else {
            freq[i++] = b;
        }
    }
    return p;
}

} // namespace

size_t encode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap) {
    if (srcSize == 0 || dstCap < 16) return 0;

    uint64_t hist[256] = {0};
    for (size_t i = 0; i < srcSize; i++) hist[src[i]]++;
    uint16_t freq[256];
    if (!normalizeFreqs(hist, srcSize, freq)) return 0;
    uint32_t cum[257];
    cum[0] = 0;
    for (int i = 0; i < 256; i++) cum[i + 1] = cum[i] + freq[i];

    // 2-way interleaved encode, backwards into a temp buffer (emitted bytes
    // end up in forward consumption order for the decoder). Symbol i uses
    // state (i & 1); decoder mirrors this schedule exactly.
    thread_local std::vector<uint8_t> tmp;
    if (tmp.size() < srcSize + 64) tmp.resize(srcSize + 64);
    uint8_t* const tmpEnd = tmp.data() + tmp.size();
    uint8_t* out = tmpEnd;
    uint32_t st[2] = {RANS_L, RANS_L};
    for (size_t i = srcSize; i-- > 0;) {
        uint32_t& x = st[i & 1];
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

    // header: [u16 tableSize][table][u32 state0][u32 state1][payload]
    uint8_t table[3 * 256];
    size_t tableSize = writeFreqTable(freq, table, sizeof(table));
    if (tableSize == 0) return 0;
    size_t encSize = 2 + tableSize + 8 + payload;
    if (encSize >= srcSize || encSize > dstCap) return 0;

    uint8_t* d = dst;
    d[0] = (uint8_t)(tableSize & 0xff);
    d[1] = (uint8_t)(tableSize >> 8);
    d += 2;
    std::memcpy(d, table, tableSize);
    d += tableSize;
    for (int k = 0; k < 2; k++) {
        d[0] = (uint8_t)(st[k] & 0xff);
        d[1] = (uint8_t)((st[k] >> 8) & 0xff);
        d[2] = (uint8_t)((st[k] >> 16) & 0xff);
        d[3] = (uint8_t)((st[k] >> 24) & 0xff);
        d += 4;
    }
    std::memcpy(d, out, payload);
    return encSize;
}

size_t decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    if (srcSize < 2 + 8) return 0;
    size_t tableSize = (size_t)(src[0] | (src[1] << 8));
    if (2 + tableSize + 8 > srcSize) return 0;
    uint16_t freq[256];
    if (readFreqTable(src + 2, tableSize, freq) != tableSize) return 0;
    const uint8_t* p = src + 2 + tableSize;
    uint32_t cum[257];
    cum[0] = 0;
    for (int i = 0; i < 256; i++) {
        cum[i + 1] = cum[i] + freq[i];
        if (cum[i + 1] > SCALE) return 0;
    }
    if (cum[256] != SCALE) return 0;

    // packed slot table: sym | (freq-1)<<8 | cum<<20  (one L1 load per symbol)
    thread_local std::vector<uint32_t> tbl;
    if (tbl.size() < SCALE) tbl.resize(SCALE);
    for (int s = 0; s < 256; s++) {
        uint32_t e = (uint32_t)s | ((uint32_t)(freq[s] - 1) << 8) | ((uint32_t)cum[s] << 20);
        for (uint32_t k = cum[s]; k < cum[s + 1]; k++) tbl[k] = e;
    }

    auto readState = [&]() {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        return v;
    };
    uint32_t x0 = readState();
    uint32_t x1 = readState();
    const uint8_t* const pend = src + srcSize;
    const uint32_t* const T = tbl.data();

    // 2 symbols per iteration: the two states renorm independently, giving the
    // CPU two dependency chains to overlap.
    size_t i = 0;
    for (; i + 2 <= rawSize; i += 2) {
        uint32_t e0 = T[x0 & (SCALE - 1)];
        dst[i] = (uint8_t)e0;
        x0 = (((e0 >> 8) & 0xfff) + 1) * (x0 >> SCALE_BITS) + (x0 & (SCALE - 1)) - (e0 >> 20);
        while (x0 < RANS_L) {
            if (p >= pend) return 0;
            x0 = (x0 << 8) | *p++;
        }
        uint32_t e1 = T[x1 & (SCALE - 1)];
        dst[i + 1] = (uint8_t)e1;
        x1 = (((e1 >> 8) & 0xfff) + 1) * (x1 >> SCALE_BITS) + (x1 & (SCALE - 1)) - (e1 >> 20);
        while (x1 < RANS_L) {
            if (p >= pend) return 0;
            x1 = (x1 << 8) | *p++;
        }
    }
    if (i < rawSize) {
        uint32_t e0 = T[x0 & (SCALE - 1)];
        dst[i] = (uint8_t)e0;
    }
    return rawSize;
}

} // namespace rans
} // namespace gdc
