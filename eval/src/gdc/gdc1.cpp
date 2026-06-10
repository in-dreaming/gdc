#include "gdc1.h"
#include "rans.h"

#include <cmath>
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

// per-level tuning: {chain search depth, lazy parsing, entropy coding, optimal parse}
struct LevelCfg { int depth; bool lazy; bool entropy; bool optimal; };
const LevelCfg kLevels[10] = {
    {2,   false, false, false}, // 0
    {4,   false, false, false}, // 1
    {8,   false, false, false}, // 2
    {16,  false, true,  false}, // 3
    {24,  false, true,  false}, // 4
    {32,  true,  true,  false}, // 5
    {64,  true,  true,  false}, // 6
    {128, true,  true,  false}, // 7
    {256, true,  true,  false}, // 8
    {512, true,  true,  true},  // 9
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

// Writes one stream, choosing the smallest of {raw, rANS-o0 with inline or
// static table, rANS-o1}. kind selects the trained static table (one per
// stream type); decode() dispatches via the stream header automatically.
// mode: 0 = raw, 1 = order-0, 2 = order-1. Returns stored size.
size_t storeStream(const std::vector<uint8_t>& s, bool entropy, int kind, bool allowO1,
                   uint8_t* dst, size_t cap, int& mode) {
    mode = 0;
    size_t best = SIZE_MAX;
    if (entropy && s.size() >= 64) {
        size_t r = rans::encode(s.data(), s.size(), dst, cap);
        if (r > 0) { mode = 1; best = r; }
        // static table: no table bytes in the stream -> wins on small blocks
        {
            thread_local std::vector<uint8_t> alt;
            if (alt.size() < cap) alt.resize(cap);
            size_t rs = rans::encodeStatic(s.data(), s.size(), alt.data(), cap, kind);
            if (rs > 0 && rs < best) {
                std::memcpy(dst, alt.data(), rs);
                mode = 1;
                best = rs;
            }
        }
        // o1 has 16 tables of overhead; only worth probing on larger streams
        if (allowO1 && s.size() >= 16384) {
            thread_local std::vector<uint8_t> alt;
            if (alt.size() < cap) alt.resize(cap);
            size_t r1 = rans::encodeO1(s.data(), s.size(), alt.data(), cap);
            // o1 decode is ~2x slower (serialized context chain); demand a
            // real ratio win before paying that
            if (r1 > 0 && r1 + (best >> 5) < best) {
                std::memcpy(dst, alt.data(), r1);
                mode = 2;
                best = r1;
            }
        }
        if (mode != 0) return best;
    }
    if (s.size() > cap) return SIZE_MAX;
    std::memcpy(dst, s.data(), s.size());
    return s.size();
}

void (*g_trainHook)(int, const uint8_t*, size_t) = nullptr;

// ---------- optimal parse (level 9) ----------
// Forward DP over positions with an approximate bit-cost model; prices are in
// 1/8-bit units. Tracks one rep-offset along the optimal path (zstd-style
// approximation: exact rep tracking needs per-state DP, not worth it here).

constexpr uint32_t INF_PRICE = 0xffffffffu;

// per-symbol prices (1/8-bit units) derived from a first greedy pass over the
// same input: -log2(p) of each stream's actual histogram, so the DP optimizes
// against (an estimate of) the real post-rANS cost.
struct PriceModel {
    uint16_t lit[256];
    uint16_t offLo[256];
    uint16_t offHi[256];

    static void fill(uint16_t* out, const uint64_t* hist, uint64_t total) {
        for (int i = 0; i < 256; i++) {
            if (hist[i] == 0 || total == 0) { out[i] = 12 * 8; continue; }
            double bits = -std::log2((double)hist[i] / (double)total);
            if (bits < 0.5) bits = 0.5;
            if (bits > 12.0) bits = 12.0;
            out[i] = (uint16_t)(bits * 8.0 + 0.5);
        }
    }
    void build(const Streams& s) {
        uint64_t h[256];
        auto histOf = [&](const std::vector<uint8_t>& v) -> uint64_t {
            std::memset(h, 0, sizeof(h));
            for (uint8_t b : v) h[b]++;
            return (uint64_t)v.size();
        };
        fill(lit, h, histOf(s.lits));
        fill(offLo, h, histOf(s.offLo));
        fill(offHi, h, histOf(s.offHi));
    }
    uint32_t matchPrice8(size_t mLen, size_t encDist) const {
        // encDist == 0 means rep-offset
        uint32_t bits8 = 64 /*token*/ + offLo[encDist & 0xff] + offHi[encDist >> 8];
        size_t ml = mLen - MIN_MATCH;
        if (ml >= 15) bits8 += 64 * (uint32_t)(1 + (ml - 15) / 255);
        return bits8;
    }
};

struct OptArrays {
    std::vector<uint32_t> price, plen, pdist, rep;
};

void greedyParse(const uint8_t* src, size_t srcSize, const LevelCfg& cfg, int depth,
                 MatchFinder& mf, Streams& s);

// Returns false if input too large for DP (caller falls back to lazy parse).
bool optimalParse(const uint8_t* src, size_t srcSize, const LevelCfg& cfg,
                  MatchFinder& mf, Streams& s) {
    if (srcSize > (8u << 20) || srcSize < MIN_MATCH + LAST_LITERALS + 8) return false;
    const uint8_t* const end = src + srcSize;
    const uint8_t* const matchLimit = end - LAST_LITERALS;
    const size_t mfEnd = srcSize - (MIN_MATCH + LAST_LITERALS);

    // pass 1: cheap greedy parse -> stream histograms -> symbol prices
    thread_local Streams scratch;
    scratch.tokens.clear(); scratch.exts.clear(); scratch.lits.clear();
    scratch.offLo.clear(); scratch.offHi.clear();
    greedyParse(src, srcSize, cfg, /*depth=*/32, mf, scratch);
    thread_local PriceModel model;
    model.build(scratch);
    mf.reset();

    thread_local OptArrays a;
    size_t n = srcSize;
    a.price.assign(n + 1, INF_PRICE);
    a.plen.assign(n + 1, 0);
    a.pdist.assign(n + 1, 0);
    a.rep.assign(n + 1, 0);
    a.price[0] = 0;

    // find() runs at *every* position here (greedy only at match starts); on
    // big inputs cap the chain depth to keep level 9 usable. Small files
    // (prefab/asset) need the full depth -- shallow chains cost real ratio.
    const int dpDepth = (n <= (256u << 10)) ? cfg.depth : (cfg.depth > 128 ? 128 : cfg.depth);

    for (size_t i = 0; i < n; i++) {
        uint32_t base = a.price[i];
        if (base == INF_PRICE) continue;
        // literal step
        uint32_t lp = base + model.lit[src[i]];
        if (lp < a.price[i + 1]) {
            a.price[i + 1] = lp;
            a.plen[i + 1] = 0;
            a.rep[i + 1] = a.rep[i];
        }
        if (i > mfEnd) continue;

        size_t dist = 0;
        size_t len = mf.find(src, i, matchLimit, dpDepth, dist);
        mf.insert(src, i);

        // rep-offset candidate along the current optimal path
        size_t repDist = a.rep[i];
        size_t repLen = 0;
        if (repDist && i >= repDist && src + i + MIN_MATCH <= matchLimit &&
            read32(src + i - repDist) == read32(src + i)) {
            repLen = MIN_MATCH;
            while (src + i + repLen < matchLimit && src[i - repDist + repLen] == src[i + repLen]) repLen++;
        }

        auto relax = [&](size_t l, size_t d, bool isRep) {
            uint32_t np = base + model.matchPrice8(l, isRep ? 0 : d);
            if (np < a.price[i + l]) {
                a.price[i + l] = np;
                a.plen[i + l] = (uint32_t)l;
                a.pdist[i + l] = (uint32_t)d;
                a.rep[i + l] = (uint32_t)d;
            }
        };
        // very long match: accept greedily and skip the covered region.
        // Visiting every position inside a long run is O(n*len) (each find()
        // re-extends a run-length match) and DP gains nothing there.
        constexpr size_t GREEDY_LEN = 1024;
        if (len >= GREEDY_LEN || repLen >= GREEDY_LEN) {
            bool useRep = repLen + 1 >= len;
            size_t gl = useRep ? repLen : len;
            size_t gd = useRep ? repDist : dist;
            relax(gl, gd, useRep || gd == repDist);
            i += gl - 1; // skipped positions stay unreachable/un-inserted
            continue;
        }
        if (len >= MIN_MATCH) {
            for (size_t l = MIN_MATCH; l <= len; l++) relax(l, dist, dist == repDist);
        }
        if (repLen >= MIN_MATCH) {
            for (size_t l = MIN_MATCH; l <= repLen; l++) relax(l, repDist, true);
        }
    }

    // backtrace: collect (pos, len, dist) matches in reverse order
    thread_local std::vector<uint32_t> mpos, mlen, mdist;
    mpos.clear(); mlen.clear(); mdist.clear();
    for (size_t j = n; j > 0;) {
        uint32_t l = a.plen[j];
        if (l == 0) { j -= 1; continue; }
        j -= l;
        mpos.push_back((uint32_t)j);
        mlen.push_back(l);
        mdist.push_back(a.pdist[j + l]);
    }

    const uint8_t* anchor = src;
    size_t lastDist = 0;
    for (size_t k = mpos.size(); k-- > 0;) {
        const uint8_t* p = src + mpos[k];
        emitSeq(s, anchor, (size_t)(p - anchor), mlen[k], mdist[k], lastDist);
        anchor = p + mlen[k];
    }
    emitSeq(s, anchor, (size_t)(end - anchor), 0, 0, lastDist);
    return true;
}

// greedy/lazy parse (levels 0..8, and pass 1 of the optimal parse)
void greedyParse(const uint8_t* src, size_t srcSize, const LevelCfg& cfg, int depth,
                 MatchFinder& mf, Streams& s) {
    const uint8_t* const end = src + srcSize;
    const uint8_t* const mfLimit = (srcSize > MIN_MATCH + LAST_LITERALS + 8)
        ? end - (MIN_MATCH + LAST_LITERALS) : src;
    const uint8_t* const matchLimit = end - LAST_LITERALS;
    const uint8_t* anchor = src;
    const uint8_t* p = src;
    size_t lastDist = 0;

    while (p < mfLimit) {
        size_t dist = 0;
        size_t len = mf.find(src, (size_t)(p - src), matchLimit, depth, dist);
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
                size_t len2 = mf.find(src, (size_t)(p + 1 - src), matchLimit, depth, dist2);
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
}

} // namespace

void setTrainHook(void (*hook)(int streamKind, const uint8_t* data, size_t n)) {
    g_trainHook = hook;
}

size_t compressBound(size_t srcSize) {
    return srcSize + srcSize / 128 + 4096;
}

// Layout:
//   u8  flags (bit0..4 = tokens/exts/lits/offLo/offHi rANS-coded,
//              bit5 = lits stream uses order-1 rANS)
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

    if (!cfg.optimal || !optimalParse(src, srcSize, cfg, mf, s))
        greedyParse(src, srcSize, cfg, cfg.depth, mf, s);

    const size_t NS = 5;
    const size_t HDR = 1 + 2 * NS * 4;
    if (dstCap < HDR) return 0;
    uint8_t* d = dst + HDR;
    size_t cap = dstCap - HDR;
    const std::vector<uint8_t>* streams[NS] = {&s.tokens, &s.exts, &s.lits, &s.offLo, &s.offHi};
    if (g_trainHook) {
        for (size_t k = 0; k < NS; k++) g_trainHook((int)k, streams[k]->data(), streams[k]->size());
    }
    size_t enc[NS];
    uint8_t flags = 0;
    for (size_t k = 0; k < NS; k++) {
        int mode;
        enc[k] = storeStream(*streams[k], cfg.entropy, (int)k, /*allowO1=*/k == 2, d, cap, mode);
        if (enc[k] == SIZE_MAX) return 0;
        if (mode != 0) flags |= (uint8_t)(1 << k);
        if (mode == 2) flags |= 32;
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
            const bool o1 = (k == 2) && (flags & 32);
            size_t got = o1 ? rans::decodeO1(p, enc[k], bufs[k].data(), raw[k])
                            : rans::decode(p, enc[k], bufs[k].data(), raw[k]);
            if (got != raw[k]) return 0;
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

    // fast-path margin: short lit runs (<15) copy a fixed 16B block (wild),
    // skipping the variable-size memcpy and exact bounds checks
    uint8_t* const oendSafe = rawSize > 64 ? oend - 64 : dst;

    while (op < oend) {
        if (tok >= tokEnd) return 0;
        uint8_t token = *tok++;
        size_t litLen = token >> 4;
        if (litLen < 15 && op < oendSafe && (size_t)(litEnd - lit) >= 16) {
            std::memcpy(op, lit, 16);
            lit += litLen;
            op += litLen;
            // op < oend guaranteed (>=50B slack) -> never the terminal seq
        } else {
            litLen = readLen(litLen);
            if (litLen == SIZE_MAX) return 0;
            if ((size_t)(litEnd - lit) < litLen || (size_t)(oend - op) < litLen) return 0;
            std::memcpy(op, lit, litLen);
            lit += litLen;
            op += litLen;
            if (op == oend) break; // terminal sequence
        }
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
        if ((size_t)(oend - op) >= mLen + 8) {
            // wild copy: 8B chunks, may overshoot into the slack region.
            // dist < 8 first expands the pattern LZ4-style so subsequent
            // 8B copies are overlap-safe.
            if (dist < 8) {
                static const unsigned incTab[8] = {0, 1, 2, 1, 0, 4, 4, 4};
                static const int decTab[8] = {0, 0, 0, -1, -4, 1, 2, 3};
                op[0] = m[0]; op[1] = m[1]; op[2] = m[2]; op[3] = m[3];
                m += incTab[dist];
                std::memcpy(op + 4, m, 4);
                m -= decTab[dist];
            } else {
                std::memcpy(op, m, 8);
                m += 8;
            }
            op += 8;
            while (op < cpEnd) {
                std::memcpy(op, m, 8);
                op += 8; m += 8;
            }
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
