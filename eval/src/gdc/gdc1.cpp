#include "gdc1.h"
#include "rans.h"

#include <cstring>
#include <vector>

namespace gdc {
namespace gdc1 {

namespace {

constexpr size_t MIN_MATCH = 4;
constexpr size_t LAST_LITERALS = 5;
constexpr size_t MAX_DIST = 0xffff;
constexpr size_t HASH_LOG = 16;
constexpr size_t CHAIN_MASK = 0xffff;

// per-level tuning: {chain search depth, lazy parsing, entropy coding}
struct LevelCfg { int depth; bool lazy; bool entropy; };
const LevelCfg kLevels[10] = {
    {2,   false, false}, // 0
    {4,   false, false}, // 1
    {8,   false, false}, // 2
    {16,  false, true},  // 3
    {24,  false, true},  // 4
    {32,  true,  true},  // 5
    {64,  true,  true},  // 6
    {128, true,  true},  // 7
    {256, true,  true},  // 8
    {512, true,  true},  // 9
};

inline uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint32_t hash4(uint32_t v) {
    return (v * 2654435761u) >> (32 - HASH_LOG);
}

struct MatchFinder {
    std::vector<uint32_t> head; // hash -> pos+1
    std::vector<uint32_t> prev; // pos & CHAIN_MASK -> previous pos+1 in chain

    void reset() {
        head.assign(1u << HASH_LOG, 0);
        prev.assign(CHAIN_MASK + 1, 0);
    }
    void insert(const uint8_t* base, size_t pos) {
        uint32_t h = hash4(read32(base + pos));
        prev[pos & CHAIN_MASK] = head[h];
        head[h] = (uint32_t)pos + 1;
    }
    // returns match length (0 if none), sets dist
    size_t find(const uint8_t* base, size_t pos, const uint8_t* matchLimit, int depth, size_t& dist) const {
        uint32_t h = hash4(read32(base + pos));
        uint32_t candPlus = head[h];
        size_t bestLen = 0;
        const uint8_t* p = base + pos;
        while (candPlus && depth-- > 0) {
            size_t cand = candPlus - 1;
            if (cand >= pos || pos - cand > MAX_DIST) break;
            const uint8_t* c = base + cand;
            if (read32(c) == read32(p)) {
                size_t len = MIN_MATCH;
                while (p + len < matchLimit && c[len] == p[len]) len++;
                if (len > bestLen) {
                    bestLen = len;
                    dist = pos - cand;
                }
            }
            uint32_t nextPlus = prev[cand & CHAIN_MASK];
            if (nextPlus == 0 || nextPlus - 1 >= cand) break; // stale/cyclic chain guard
            candPlus = nextPlus;
        }
        return bestLen;
    }
};

struct Streams {
    std::vector<uint8_t> tokens; // pure token bytes (lit<<4 | mlen codes)
    std::vector<uint8_t> exts;   // length extension bytes (lit + match, in order)
    std::vector<uint8_t> lits;
    std::vector<uint8_t> offLo;  // offset low bytes (near-uniform)
    std::vector<uint8_t> offHi;  // offset high bytes (highly skewed -> rANS)
};

void putLenExt(std::vector<uint8_t>& v, size_t len) {
    while (len >= 255) { v.push_back(255); len -= 255; }
    v.push_back((uint8_t)len);
}

// rep-offset: offset 0x0000 in the stream means "repeat previous offset".
// Saves nothing in raw bytes but makes the offsets stream highly skewed
// (rANS-friendly) and lets the parser pick cheap repeat matches.
void emitSeq(Streams& s, const uint8_t* lits, size_t litLen, size_t mLen, size_t dist, size_t& lastDist) {
    size_t litCode = litLen < 15 ? litLen : 15;
    size_t mCode = mLen ? ((mLen - MIN_MATCH) < 15 ? (mLen - MIN_MATCH) : 15) : 0;
    s.tokens.push_back((uint8_t)((litCode << 4) | mCode));
    if (litLen >= 15) putLenExt(s.exts, litLen - 15);
    s.lits.insert(s.lits.end(), lits, lits + litLen);
    if (mLen) {
        size_t enc = (dist == lastDist) ? 0 : dist;
        s.offLo.push_back((uint8_t)(enc & 0xff));
        s.offHi.push_back((uint8_t)(enc >> 8));
        lastDist = dist;
        if (mLen - MIN_MATCH >= 15) putLenExt(s.exts, mLen - MIN_MATCH - 15);
    }
}

void putU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
uint32_t getU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Writes one stream: rANS if enabled and smaller, else raw. Returns stored size.
size_t storeStream(const std::vector<uint8_t>& s, bool entropy, uint8_t* dst, size_t cap, bool& usedRans) {
    usedRans = false;
    if (entropy && s.size() >= 256) {
        size_t r = rans::encode(s.data(), s.size(), dst, cap);
        if (r > 0) { usedRans = true; return r; }
    }
    if (s.size() > cap) return SIZE_MAX;
    std::memcpy(dst, s.data(), s.size());
    return s.size();
}

} // namespace

size_t compressBound(size_t srcSize) {
    return srcSize + srcSize / 128 + 4096;
}

// Layout:
//   u8  flags (bit0..4 = tokens/exts/lits/offLo/offHi rANS-coded)
//   u32 raw[5]   (uncompressed stream sizes)
//   u32 enc[5]   (stored stream sizes)
//   [tokens][exts][lits][offLo][offHi]
size_t compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap, int level) {
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    const LevelCfg& cfg = kLevels[level];

    thread_local Streams s;
    s.tokens.clear(); s.exts.clear(); s.lits.clear(); s.offLo.clear(); s.offHi.clear();
    thread_local MatchFinder mf;
    mf.reset();

    const uint8_t* const end = src + srcSize;
    const uint8_t* const mfLimit = (srcSize > MIN_MATCH + LAST_LITERALS + 8)
        ? end - (MIN_MATCH + LAST_LITERALS) : src;
    const uint8_t* const matchLimit = end - LAST_LITERALS;
    const uint8_t* anchor = src;
    const uint8_t* p = src;
    size_t lastDist = 0;

    while (p < mfLimit) {
        size_t dist = 0;
        size_t len = mf.find(src, (size_t)(p - src), matchLimit, cfg.depth, dist);
        // check repeat-offset match: ~free to encode, prefer when nearly as long
        if (lastDist && (size_t)(p - src) >= lastDist && p + MIN_MATCH <= matchLimit) {
            const uint8_t* c = p - lastDist;
            if (read32(c) == read32(p)) {
                size_t repLen = MIN_MATCH;
                while (p + repLen < matchLimit && c[repLen] == p[repLen]) repLen++;
                if (repLen + 1 >= len) { len = repLen; dist = lastDist; }
            }
        }
        if (len >= MIN_MATCH) {
            // single-step lazy: prefer a longer match starting one byte later
            if (cfg.lazy && p + 1 < mfLimit) {
                mf.insert(src, (size_t)(p - src));
                size_t dist2 = 0;
                size_t len2 = mf.find(src, (size_t)(p + 1 - src), matchLimit, cfg.depth, dist2);
                if (len2 > len) {
                    p++; // current byte becomes a literal
                    len = len2;
                    dist = dist2;
                } else {
                    // keep match at p; position already inserted
                    emitSeq(s, anchor, (size_t)(p - anchor), len, dist, lastDist);
                    for (size_t k = 1; k < len && p + k + MIN_MATCH <= matchLimit; k++)
                        mf.insert(src, (size_t)(p - src) + k);
                    p += len;
                    anchor = p;
                    continue;
                }
            }
            emitSeq(s, anchor, (size_t)(p - anchor), len, dist, lastDist);
            for (size_t k = 0; k < len && p + k + MIN_MATCH <= matchLimit; k++)
                mf.insert(src, (size_t)(p - src) + k);
            p += len;
            anchor = p;
        } else {
            mf.insert(src, (size_t)(p - src));
            p++;
        }
    }
    emitSeq(s, anchor, (size_t)(end - anchor), 0, 0, lastDist); // terminal literal run

    const size_t NS = 5;
    const size_t HDR = 1 + 2 * NS * 4;
    if (dstCap < HDR) return 0;
    uint8_t* d = dst + HDR;
    size_t cap = dstCap - HDR;
    const std::vector<uint8_t>* streams[NS] = {&s.tokens, &s.exts, &s.lits, &s.offLo, &s.offHi};
    size_t enc[NS];
    uint8_t flags = 0;
    for (size_t k = 0; k < NS; k++) {
        bool r;
        enc[k] = storeStream(*streams[k], cfg.entropy, d, cap, r);
        if (enc[k] == SIZE_MAX) return 0;
        if (r) flags |= (uint8_t)(1 << k);
        d += enc[k]; cap -= enc[k];
    }

    dst[0] = flags;
    for (size_t k = 0; k < NS; k++) {
        putU32(dst + 1 + 4 * k, (uint32_t)streams[k]->size());
        putU32(dst + 1 + 4 * (NS + k), (uint32_t)enc[k]);
    }
    return (size_t)(d - dst);
}

size_t decompress(const uint8_t* src, size_t compSize, uint8_t* dst, size_t rawSize) {
    const size_t NS = 5;
    const size_t HDR = 1 + 2 * NS * 4;
    if (compSize < HDR) return 0;
    uint8_t flags = src[0];
    size_t raw[NS], enc[NS];
    size_t encTotal = 0;
    for (size_t k = 0; k < NS; k++) {
        raw[k] = getU32(src + 1 + 4 * k);
        enc[k] = getU32(src + 1 + 4 * (NS + k));
        encTotal += enc[k];
    }
    if (HDR + encTotal != compSize) return 0;
    if (raw[3] != raw[4]) return 0; // offLo/offHi must pair up

    thread_local std::vector<uint8_t> bufs[NS];
    const uint8_t* p = src + HDR;
    const uint8_t* sp[NS];
    for (size_t k = 0; k < NS; k++) {
        if (flags & (1 << k)) {
            if (bufs[k].size() < raw[k]) bufs[k].resize(raw[k]);
            if (rans::decode(p, enc[k], bufs[k].data(), raw[k]) != raw[k]) return 0;
            sp[k] = bufs[k].data();
        } else {
            if (enc[k] != raw[k]) return 0;
            sp[k] = p;
        }
        p += enc[k];
    }
    const uint8_t* tok = sp[0];
    const uint8_t* ext = sp[1];
    const uint8_t* lit = sp[2];
    const uint8_t* offLo = sp[3];
    const uint8_t* offHi = sp[4];
    const uint8_t* const tokEnd = tok + raw[0];
    const uint8_t* const extEnd = ext + raw[1];
    const uint8_t* const litEnd = lit + raw[2];
    const uint8_t* const offEnd = offLo + raw[3];

    uint8_t* op = dst;
    uint8_t* const oend = dst + rawSize;
    size_t lastDist = 0;
    auto readLen = [&](size_t base) -> size_t {
        size_t len = base;
        if (base == 15) {
            uint8_t b;
            do {
                if (ext >= extEnd) return SIZE_MAX;
                b = *ext++;
                len += b;
            } while (b == 255);
        }
        return len;
    };

    while (op < oend) {
        if (tok >= tokEnd) return 0;
        uint8_t token = *tok++;
        size_t litLen = readLen(token >> 4);
        if (litLen == SIZE_MAX) return 0;
        if ((size_t)(litEnd - lit) < litLen || (size_t)(oend - op) < litLen) return 0;
        std::memcpy(op, lit, litLen);
        lit += litLen;
        op += litLen;
        if (op == oend) break; // terminal sequence
        if (offLo >= offEnd) return 0;
        size_t dist = (size_t)*offLo++ | ((size_t)*offHi++ << 8);
        if (dist == 0) dist = lastDist; // rep-offset
        else lastDist = dist;
        size_t mLen = readLen(token & 0xf);
        if (mLen == SIZE_MAX) return 0;
        mLen += MIN_MATCH;
        if (dist == 0 || (size_t)(op - dst) < dist || (size_t)(oend - op) < mLen) return 0;
        const uint8_t* m = op - dist;
        uint8_t* const cpEnd = op + mLen;
        if (dist >= 8 && (size_t)(oend - op) >= mLen + 8) {
            // wild copy: 8B chunks, may overshoot into the slack region
            do {
                std::memcpy(op, m, 8);
                op += 8; m += 8;
            } while (op < cpEnd);
            op = cpEnd;
        } else {
            for (size_t i = 0; i < mLen; i++) op[i] = m[i]; // overlap-safe
            op += mLen;
        }
    }
    return op == oend ? rawSize : 0;
}

} // namespace gdc1
} // namespace gdc
