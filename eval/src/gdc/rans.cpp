#include "rans.h"
#include "static_tables.h"

#include <cstring>
#include <vector>

namespace gdc {
namespace rans {

namespace {

constexpr uint32_t SCALE_BITS = 12;
constexpr uint32_t SCALE = 1u << SCALE_BITS; // 4096
// 16-bit renorm (ryg rans_word style): with L = 1<<16 and 12-bit scale each
// decode step needs at most ONE renorm read -> branch instead of loop.
constexpr uint32_t RANS_L = 1u << 16;        // lower bound of normalization interval

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

// 4-way interleaved encode, backwards into tmp (emitted bytes end up in
// forward consumption order for the decoder). Symbol i uses state (i & 3);
// decoder mirrors this schedule exactly. Returns payload start (or nullptr
// if the stream cannot shrink), fills st[4].
const uint8_t* encodeCore(const uint8_t* src, size_t srcSize, const uint16_t* freq,
                          const uint32_t* cum, std::vector<uint8_t>& tmp, uint32_t st[4]) {
    if (tmp.size() < srcSize + 64) tmp.resize(srcSize + 64);
    uint8_t* const tmpEnd = tmp.data() + tmp.size();
    uint8_t* out = tmpEnd;
    st[0] = st[1] = st[2] = st[3] = RANS_L;
    for (size_t i = srcSize; i-- > 0;) {
        uint32_t& x = st[i & 3];
        uint8_t s = src[i];
        uint32_t f = freq[s];
        // 64-bit: f can be SCALE (single-symbol context) and 2^20*4096 = 2^32
        // overflows u32 -> bogus renorms that desync the decoder
        uint64_t xMax = ((uint64_t)(RANS_L >> SCALE_BITS) << 16) * f;
        if (x >= xMax) { // at most one 16-bit renorm per symbol
            if (out - tmp.data() < 2) return nullptr; // cannot shrink, bail out
            out -= 2;
            out[0] = (uint8_t)(x & 0xff);
            out[1] = (uint8_t)((x >> 8) & 0xff);
            x >>= 16;
        }
        x = ((x / f) << SCALE_BITS) + (x % f) + cum[s];
    }
    return out;
}

void writeStates(uint8_t*& d, const uint32_t st[4]) {
    for (int k = 0; k < 4; k++) {
        d[0] = (uint8_t)(st[k] & 0xff);
        d[1] = (uint8_t)((st[k] >> 8) & 0xff);
        d[2] = (uint8_t)((st[k] >> 16) & 0xff);
        d[3] = (uint8_t)((st[k] >> 24) & 0xff);
        d += 4;
    }
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

    thread_local std::vector<uint8_t> tmp;
    uint32_t st[4];
    const uint8_t* out = encodeCore(src, srcSize, freq, cum, tmp, st);
    if (!out) return 0;
    size_t payload = (size_t)(tmp.data() + tmp.size() - out);

    // header: [u16 tableSize][table][u32 state0..3][payload]
    uint8_t table[3 * 256];
    size_t tableSize = writeFreqTable(freq, table, sizeof(table));
    if (tableSize == 0) return 0;
    size_t encSize = 2 + tableSize + 16 + payload;
    if (encSize >= srcSize || encSize > dstCap) return 0;

    uint8_t* d = dst;
    d[0] = (uint8_t)(tableSize & 0xff);
    d[1] = (uint8_t)(tableSize >> 8);
    d += 2;
    std::memcpy(d, table, tableSize);
    d += tableSize;
    writeStates(d, st);
    std::memcpy(d, out, payload);
    return encSize;
}

// Static built-in table: header [u16 0x8000|id][u32 state0..3][payload].
size_t encodeStatic(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap, int id) {
    if (srcSize == 0 || dstCap < 18 || id < 0 || id >= kNumStaticTables) return 0;
    const uint16_t* freq = kStaticFreq[id];
    thread_local uint32_t cumCache[kNumStaticTables][257];
    thread_local bool cumBuilt[kNumStaticTables] = {};
    if (!cumBuilt[id]) {
        cumCache[id][0] = 0;
        for (int i = 0; i < 256; i++) cumCache[id][i + 1] = cumCache[id][i] + freq[i];
        cumBuilt[id] = true;
    }

    thread_local std::vector<uint8_t> tmp;
    uint32_t st[4];
    const uint8_t* out = encodeCore(src, srcSize, freq, cumCache[id], tmp, st);
    if (!out) return 0;
    size_t payload = (size_t)(tmp.data() + tmp.size() - out);

    size_t encSize = 2 + 16 + payload;
    if (encSize >= srcSize || encSize > dstCap) return 0;
    uint8_t* d = dst;
    uint16_t tag = (uint16_t)(0x8000 | id);
    d[0] = (uint8_t)(tag & 0xff);
    d[1] = (uint8_t)(tag >> 8);
    d += 2;
    writeStates(d, st);
    std::memcpy(d, out, payload);
    return encSize;
}

size_t decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    if (srcSize < 2 + 16) return 0;
    size_t tableSize = (size_t)(src[0] | (src[1] << 8));
    const uint8_t* p;
    const uint32_t* T;

    thread_local std::vector<uint32_t> tbl;
    if (tableSize & 0x8000) {
        // built-in static table; packed slot tables cached per id
        int id = (int)(tableSize & 0x7fff);
        if (id >= kNumStaticTables) return 0;
        static thread_local std::vector<uint32_t> stbl[kNumStaticTables];
        if (stbl[id].empty()) {
            stbl[id].resize(SCALE);
            const uint16_t* freq = kStaticFreq[id];
            uint32_t cum = 0;
            for (int s = 0; s < 256; s++) {
                uint32_t f = freq[s];
                if (f == 0) continue;
                uint32_t e = (uint32_t)s | ((f - 1) << 8) | (cum << 20);
                for (uint32_t k = 0; k < f; k++) stbl[id][cum + k] = e;
                cum += f;
            }
            if (cum != SCALE) { stbl[id].clear(); return 0; }
        }
        T = stbl[id].data();
        p = src + 2;
    } else {
        if (2 + tableSize + 16 > srcSize) return 0;
        uint16_t freq[256];
        if (readFreqTable(src + 2, tableSize, freq) != tableSize) return 0;
        p = src + 2 + tableSize;
        uint32_t cum[257];
        cum[0] = 0;
        for (int i = 0; i < 256; i++) {
            cum[i + 1] = cum[i] + freq[i];
            if (cum[i + 1] > SCALE) return 0;
        }
        if (cum[256] != SCALE) return 0;

        // packed slot table: sym | (freq-1)<<8 | cum<<20  (one L1 load per symbol)
        if (tbl.size() < SCALE) tbl.resize(SCALE);
        for (int s = 0; s < 256; s++) {
            uint32_t e = (uint32_t)s | ((uint32_t)(freq[s] - 1) << 8) | ((uint32_t)cum[s] << 20);
            for (uint32_t k = cum[s]; k < cum[s + 1]; k++) tbl[k] = e;
        }
        T = tbl.data();
    }

    auto readState = [&]() {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        return v;
    };
    uint32_t st[4];
    for (int k = 0; k < 4; k++) st[k] = readState();
    const uint8_t* const pend = src + srcSize;

    // 4 symbols per iteration: four independent dependency chains overlap in
    // the CPU pipeline; each state renorms at most once (16-bit) per symbol.
    uint32_t x0 = st[0], x1 = st[1], x2 = st[2], x3 = st[3];
    size_t i = 0;
    if (rawSize >= 4) {
        for (; i + 4 <= rawSize; i += 4) {
            uint32_t e0 = T[x0 & (SCALE - 1)];
            dst[i] = (uint8_t)e0;
            x0 = (((e0 >> 8) & 0xfff) + 1) * (x0 >> SCALE_BITS) + (x0 & (SCALE - 1)) - (e0 >> 20);
            if (x0 < RANS_L) {
                if (p + 2 > pend) return 0;
                x0 = (x0 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
                p += 2;
            }
            uint32_t e1 = T[x1 & (SCALE - 1)];
            dst[i + 1] = (uint8_t)e1;
            x1 = (((e1 >> 8) & 0xfff) + 1) * (x1 >> SCALE_BITS) + (x1 & (SCALE - 1)) - (e1 >> 20);
            if (x1 < RANS_L) {
                if (p + 2 > pend) return 0;
                x1 = (x1 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
                p += 2;
            }
            uint32_t e2 = T[x2 & (SCALE - 1)];
            dst[i + 2] = (uint8_t)e2;
            x2 = (((e2 >> 8) & 0xfff) + 1) * (x2 >> SCALE_BITS) + (x2 & (SCALE - 1)) - (e2 >> 20);
            if (x2 < RANS_L) {
                if (p + 2 > pend) return 0;
                x2 = (x2 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
                p += 2;
            }
            uint32_t e3 = T[x3 & (SCALE - 1)];
            dst[i + 3] = (uint8_t)e3;
            x3 = (((e3 >> 8) & 0xfff) + 1) * (x3 >> SCALE_BITS) + (x3 & (SCALE - 1)) - (e3 >> 20);
            if (x3 < RANS_L) {
                if (p + 2 > pend) return 0;
                x3 = (x3 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
                p += 2;
            }
        }
    }
    // tail: decode remaining symbols with their scheduled states (no renorm
    // needed for correctness of the final symbols' values)
    uint32_t xs[4] = {x0, x1, x2, x3};
    for (; i < rawSize; i++) {
        uint32_t& x = xs[i & 3];
        uint32_t e = T[x & (SCALE - 1)];
        dst[i] = (uint8_t)e;
        x = (((e >> 8) & 0xfff) + 1) * (x >> SCALE_BITS) + (x & (SCALE - 1)) - (e >> 20);
        if (x < RANS_L && p + 2 <= pend) {
            x = (x << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            p += 2;
        }
    }
    return rawSize;
}

// ---------------- order-1 (16 contexts = prev byte >> 4) ----------------
//
// Layout: 16 x [u16 tableSize][table] (tableSize 0 = unused context)
//         [u32 state0..3][payload]
// Symbol schedule matches order-0: symbol i uses state (i & 3).

size_t encodeO1(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap) {
    if (srcSize < 4 || dstCap < 64) return 0;

    uint64_t hist[16][256] = {};
    uint64_t total[16] = {};
    uint8_t prev = 0;
    for (size_t i = 0; i < srcSize; i++) {
        uint32_t c = prev >> 4;
        hist[c][src[i]]++;
        total[c]++;
        prev = src[i];
    }
    static thread_local uint16_t freq[16][256];
    uint32_t cum[16][257];
    for (int c = 0; c < 16; c++) {
        if (total[c] == 0) continue;
        if (!normalizeFreqs(hist[c], total[c], freq[c])) return 0;
        cum[c][0] = 0;
        for (int i = 0; i < 256; i++) cum[c][i + 1] = cum[c][i] + freq[c][i];
    }

    thread_local std::vector<uint8_t> tmp;
    if (tmp.size() < srcSize + 64) tmp.resize(srcSize + 64);
    uint8_t* const tmpEnd = tmp.data() + tmp.size();
    uint8_t* out = tmpEnd;
    uint32_t st[4] = {RANS_L, RANS_L, RANS_L, RANS_L};
    for (size_t i = srcSize; i-- > 0;) {
        uint32_t& x = st[i & 3];
        uint32_t c = i ? (uint32_t)(src[i - 1] >> 4) : 0;
        uint8_t s = src[i];
        uint32_t f = freq[c][s];
        uint64_t xMax = ((uint64_t)(RANS_L >> SCALE_BITS) << 16) * f; // see encode()
        if (x >= xMax) {
            if (out - tmp.data() < 2) return 0;
            out -= 2;
            out[0] = (uint8_t)(x & 0xff);
            out[1] = (uint8_t)((x >> 8) & 0xff);
            x >>= 16;
        }
        x = ((x / f) << SCALE_BITS) + (x % f) + cum[c][s];
    }
    size_t payload = (size_t)(tmpEnd - out);

    uint8_t* d = dst;
    uint8_t* const dend = dst + dstCap;
    for (int c = 0; c < 16; c++) {
        uint8_t table[3 * 256];
        size_t tableSize = 0;
        if (total[c] > 0) {
            tableSize = writeFreqTable(freq[c], table, sizeof(table));
            if (tableSize == 0) return 0;
        }
        if (d + 2 + tableSize > dend) return 0;
        d[0] = (uint8_t)(tableSize & 0xff);
        d[1] = (uint8_t)(tableSize >> 8);
        d += 2;
        std::memcpy(d, table, tableSize);
        d += tableSize;
    }
    if (d + 16 + payload > dend) return 0;
    for (int k = 0; k < 4; k++) {
        d[0] = (uint8_t)(st[k] & 0xff);
        d[1] = (uint8_t)((st[k] >> 8) & 0xff);
        d[2] = (uint8_t)((st[k] >> 16) & 0xff);
        d[3] = (uint8_t)((st[k] >> 24) & 0xff);
        d += 4;
    }
    std::memcpy(d, out, payload);
    d += payload;
    size_t encSize = (size_t)(d - dst);
    return encSize < srcSize ? encSize : 0;
}

#ifdef GDC_RANS_DEBUG
#include <cstdio>
#define RANS_DBG(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define RANS_DBG(...)
#endif

size_t decodeO1(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    const uint8_t* p = src;
    const uint8_t* const pend = src + srcSize;

    thread_local std::vector<uint32_t> tbl; // 16 contexts x SCALE entries
    if (tbl.size() < 16 * SCALE) tbl.resize(16 * SCALE);
    for (int c = 0; c < 16; c++) {
        if (p + 2 > pend) return 0;
        size_t tableSize = (size_t)(p[0] | (p[1] << 8));
        p += 2;
        if (tableSize == 0) continue; // unused context, table stays stale/unused
        if (p + tableSize > pend) { RANS_DBG("o1: table %d overruns\n", c); return 0; }
        uint16_t freq[256];
        if (readFreqTable(p, tableSize, freq) != tableSize) { RANS_DBG("o1: table %d bad rle\n", c); return 0; }
        p += tableSize;
        uint32_t cum = 0;
        uint32_t* T = tbl.data() + (size_t)c * SCALE;
        for (int s = 0; s < 256; s++) {
            uint32_t f = freq[s];
            if (f == 0) continue;
            uint32_t e = (uint32_t)s | ((f - 1) << 8) | (cum << 20);
            for (uint32_t k = 0; k < f; k++) T[cum + k] = e;
            cum += f;
        }
        if (cum != SCALE) { RANS_DBG("o1: ctx %d cum=%u != SCALE\n", c, cum); return 0; }
    }
    if (p + 16 > pend) { RANS_DBG("o1: no room for states\n"); return 0; }
    uint32_t st[4];
    for (int k = 0; k < 4; k++) {
        st[k] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
    }

    uint32_t x0 = st[0], x1 = st[1], x2 = st[2], x3 = st[3];
    uint8_t prev = 0;
    size_t i = 0;
    for (; i + 4 <= rawSize; i += 4) {
        const uint32_t* T0 = tbl.data() + ((size_t)(prev >> 4) << SCALE_BITS);
        uint32_t e0 = T0[x0 & (SCALE - 1)];
        uint8_t s0 = (uint8_t)e0;
        dst[i] = s0;
        x0 = (((e0 >> 8) & 0xfff) + 1) * (x0 >> SCALE_BITS) + (x0 & (SCALE - 1)) - (e0 >> 20);
        if (x0 < RANS_L) {
            if (p + 2 > pend) return 0;
            x0 = (x0 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            p += 2;
        }
        const uint32_t* T1 = tbl.data() + ((size_t)(s0 >> 4) << SCALE_BITS);
        uint32_t e1 = T1[x1 & (SCALE - 1)];
        uint8_t s1 = (uint8_t)e1;
        dst[i + 1] = s1;
        x1 = (((e1 >> 8) & 0xfff) + 1) * (x1 >> SCALE_BITS) + (x1 & (SCALE - 1)) - (e1 >> 20);
        if (x1 < RANS_L) {
            if (p + 2 > pend) return 0;
            x1 = (x1 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            p += 2;
        }
        const uint32_t* T2 = tbl.data() + ((size_t)(s1 >> 4) << SCALE_BITS);
        uint32_t e2 = T2[x2 & (SCALE - 1)];
        uint8_t s2 = (uint8_t)e2;
        dst[i + 2] = s2;
        x2 = (((e2 >> 8) & 0xfff) + 1) * (x2 >> SCALE_BITS) + (x2 & (SCALE - 1)) - (e2 >> 20);
        if (x2 < RANS_L) {
            if (p + 2 > pend) return 0;
            x2 = (x2 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            p += 2;
        }
        const uint32_t* T3 = tbl.data() + ((size_t)(s2 >> 4) << SCALE_BITS);
        uint32_t e3 = T3[x3 & (SCALE - 1)];
        uint8_t s3 = (uint8_t)e3;
        dst[i + 3] = s3;
        x3 = (((e3 >> 8) & 0xfff) + 1) * (x3 >> SCALE_BITS) + (x3 & (SCALE - 1)) - (e3 >> 20);
        if (x3 < RANS_L) {
            if (p + 2 > pend) return 0;
            x3 = (x3 << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            p += 2;
        }
        prev = s3;
    }
    uint32_t xs[4] = {x0, x1, x2, x3};
    for (; i < rawSize; i++) {
        uint32_t& x = xs[i & 3];
        const uint32_t* T = tbl.data() + ((size_t)(prev >> 4) << SCALE_BITS);
        uint32_t e = T[x & (SCALE - 1)];
        prev = (uint8_t)e;
        dst[i] = prev;
        x = (((e >> 8) & 0xfff) + 1) * (x >> SCALE_BITS) + (x & (SCALE - 1)) - (e >> 20);
        if (x < RANS_L && p + 2 <= pend) {
            x = (x << 16) | ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            p += 2;
        }
    }
    return rawSize;
}

} // namespace rans
} // namespace gdc
