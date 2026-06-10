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
    std::vector<uint8_t> tokens; // token bytes + length extension bytes
    std::vector<uint8_t> lits;
    std::vector<uint8_t> offs;   // u16 LE per sequence
};

void putLenExt(std::vector<uint8_t>& v, size_t len) {
    while (len >= 255) { v.push_back(255); len -= 255; }
    v.push_back((uint8_t)len);
}

void emitSeq(Streams& s, const uint8_t* lits, size_t litLen, size_t mLen, size_t dist) {
    size_t litCode = litLen < 15 ? litLen : 15;
    size_t mCode = mLen ? ((mLen - MIN_MATCH) < 15 ? (mLen - MIN_MATCH) : 15) : 0;
    s.tokens.push_back((uint8_t)((litCode << 4) | mCode));
    if (litLen >= 15) putLenExt(s.tokens, litLen - 15);
    s.lits.insert(s.lits.end(), lits, lits + litLen);
    if (mLen) {
        s.offs.push_back((uint8_t)(dist & 0xff));
        s.offs.push_back((uint8_t)(dist >> 8));
        if (mLen - MIN_MATCH >= 15) putLenExt(s.tokens, mLen - MIN_MATCH - 15);
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
//   u8  flags (bit0/1/2 = tokens/lits/offs rANS-coded)
//   u32 rawTok, rawLit, rawOff   (uncompressed stream sizes)
//   u32 encTok, encLit, encOff   (stored stream sizes)
//   [tokens][lits][offs]
size_t compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap, int level) {
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    const LevelCfg& cfg = kLevels[level];

    thread_local Streams s;
    s.tokens.clear(); s.lits.clear(); s.offs.clear();
    thread_local MatchFinder mf;
    mf.reset();

    const uint8_t* const end = src + srcSize;
    const uint8_t* const mfLimit = (srcSize > MIN_MATCH + LAST_LITERALS + 8)
        ? end - (MIN_MATCH + LAST_LITERALS) : src;
    const uint8_t* const matchLimit = end - LAST_LITERALS;
    const uint8_t* anchor = src;
    const uint8_t* p = src;

    while (p < mfLimit) {
        size_t dist = 0;
        size_t len = mf.find(src, (size_t)(p - src), matchLimit, cfg.depth, dist);
        if (len >= MIN_MATCH) {
            // single-step lazy: prefer a longer match starting one byte later
            if (cfg.lazy && p + 1 < mfLimit) {
                mf.insert(src, (size_t)(p - src));
                size_t dist2 = 0;
                size_t len2 = mf.find(src, (size_t)(p + 1 - src), matchLimit, cfg.depth, dist2);
                if (len2 > len + 1) {
                    p++; // current byte becomes a literal
                    len = len2;
                    dist = dist2;
                } else {
                    // keep match at p; position already inserted
                    emitSeq(s, anchor, (size_t)(p - anchor), len, dist);
                    for (size_t k = 1; k < len && p + k + MIN_MATCH <= matchLimit; k++)
                        mf.insert(src, (size_t)(p - src) + k);
                    p += len;
                    anchor = p;
                    continue;
                }
            }
            emitSeq(s, anchor, (size_t)(p - anchor), len, dist);
            for (size_t k = 0; k < len && p + k + MIN_MATCH <= matchLimit; k++)
                mf.insert(src, (size_t)(p - src) + k);
            p += len;
            anchor = p;
        } else {
            mf.insert(src, (size_t)(p - src));
            p++;
        }
    }
    emitSeq(s, anchor, (size_t)(end - anchor), 0, 0); // terminal literal run

    const size_t HDR = 1 + 6 * 4;
    if (dstCap < HDR) return 0;
    uint8_t* d = dst + HDR;
    size_t cap = dstCap - HDR;
    bool r0, r1, r2;
    size_t e0 = storeStream(s.tokens, cfg.entropy, d, cap, r0);
    if (e0 == SIZE_MAX) return 0;
    d += e0; cap -= e0;
    size_t e1 = storeStream(s.lits, cfg.entropy, d, cap, r1);
    if (e1 == SIZE_MAX) return 0;
    d += e1; cap -= e1;
    size_t e2 = storeStream(s.offs, cfg.entropy, d, cap, r2);
    if (e2 == SIZE_MAX) return 0;
    d += e2;

    dst[0] = (uint8_t)((r0 ? 1 : 0) | (r1 ? 2 : 0) | (r2 ? 4 : 0));
    putU32(dst + 1, (uint32_t)s.tokens.size());
    putU32(dst + 5, (uint32_t)s.lits.size());
    putU32(dst + 9, (uint32_t)s.offs.size());
    putU32(dst + 13, (uint32_t)e0);
    putU32(dst + 17, (uint32_t)e1);
    putU32(dst + 21, (uint32_t)e2);
    return (size_t)(d - dst);
}

size_t decompress(const uint8_t* src, size_t compSize, uint8_t* dst, size_t rawSize) {
    const size_t HDR = 1 + 6 * 4;
    if (compSize < HDR) return 0;
    uint8_t flags = src[0];
    size_t rawTok = getU32(src + 1), rawLit = getU32(src + 5), rawOff = getU32(src + 9);
    size_t encTok = getU32(src + 13), encLit = getU32(src + 17), encOff = getU32(src + 21);
    if (HDR + encTok + encLit + encOff != compSize) return 0;

    thread_local std::vector<uint8_t> tokBuf, litBuf, offBuf;
    const uint8_t* p = src + HDR;
    auto loadStream = [&](std::vector<uint8_t>& buf, size_t rawN, size_t encN, bool isRans,
                          const uint8_t*& outPtr) -> bool {
        if (isRans) {
            if (buf.size() < rawN) buf.resize(rawN);
            if (rans::decode(p, encN, buf.data(), rawN) != rawN) return false;
            outPtr = buf.data();
        } else {
            if (encN != rawN) return false;
            outPtr = p;
        }
        p += encN;
        return true;
    };
    const uint8_t *tok, *lit, *off;
    if (!loadStream(tokBuf, rawTok, encTok, flags & 1, tok)) return 0;
    if (!loadStream(litBuf, rawLit, encLit, flags & 2, lit)) return 0;
    if (!loadStream(offBuf, rawOff, encOff, flags & 4, off)) return 0;
    const uint8_t* const tokEnd = tok + rawTok;
    const uint8_t* const litEnd = lit + rawLit;
    const uint8_t* const offEnd = off + rawOff;

    uint8_t* op = dst;
    uint8_t* const oend = dst + rawSize;
    auto readLen = [&](size_t base) -> size_t {
        size_t len = base;
        if (base == 15) {
            uint8_t b;
            do {
                if (tok >= tokEnd) return SIZE_MAX;
                b = *tok++;
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
        if ((size_t)(offEnd - off) < 2) return 0;
        size_t dist = (size_t)off[0] | ((size_t)off[1] << 8);
        off += 2;
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
