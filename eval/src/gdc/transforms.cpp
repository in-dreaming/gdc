#include "transforms.h"

#include <cstring>
#include <vector>

#include "zstd.h"

namespace gdc {

namespace {

// Splits the input into <stride> byte planes: all byte-0s of each block,
// then all byte-1s, ... Tail bytes (n % stride) are copied verbatim.
// Turns "interleaved bit-field blocks" (ASTC/BCn) into per-field streams
// that LZ + entropy stages model far better.
void planeFwd(const uint8_t* src, size_t n, uint8_t* dst, size_t stride) {
    const size_t blocks = n / stride;
    for (size_t p = 0; p < stride; p++) {
        uint8_t* out = dst + p * blocks;
        const uint8_t* in = src + p;
        for (size_t i = 0; i < blocks; i++) out[i] = in[i * stride];
    }
    std::memcpy(dst + blocks * stride, src + blocks * stride, n - blocks * stride);
}

void planeInv(const uint8_t* src, size_t n, uint8_t* dst, size_t stride) {
    const size_t blocks = n / stride;
    for (size_t p = 0; p < stride; p++) {
        const uint8_t* in = src + p * blocks;
        uint8_t* out = dst + p;
        for (size_t i = 0; i < blocks; i++) out[i * stride] = in[i];
    }
    std::memcpy(dst + blocks * stride, src + blocks * stride, n - blocks * stride);
}

class PlaneTransform final : public ITransform {
public:
    PlaneTransform(const char* name, size_t stride) : m_name(name), m_stride(stride) {}
    const char* name() const override { return m_name; }
    size_t forward(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) const override {
        if (cap < n) return 0;
        planeFwd(src, n, dst, m_stride);
        return n;
    }
    size_t inverse(const uint8_t* src, size_t encN, uint8_t* dst, size_t rawN) const override {
        if (encN != rawN) return 0;
        planeInv(src, rawN, dst, m_stride);
        return rawN;
    }

private:
    const char* m_name;
    size_t m_stride;
};

// Byte-wise delta. Helps slowly-varying numeric streams (esp. after a plane
// split puts same-significance bytes next to each other).
class Delta8Transform final : public ITransform {
public:
    const char* name() const override { return "delta8"; }
    size_t forward(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) const override {
        if (cap < n) return 0;
        uint8_t prev = 0;
        for (size_t i = 0; i < n; i++) {
            dst[i] = (uint8_t)(src[i] - prev);
            prev = src[i];
        }
        return n;
    }
    size_t inverse(const uint8_t* src, size_t encN, uint8_t* dst, size_t rawN) const override {
        if (encN != rawN) return 0;
        uint8_t acc = 0;
        for (size_t i = 0; i < rawN; i++) {
            acc = (uint8_t)(acc + src[i]);
            dst[i] = acc;
        }
        return rawN;
    }
};

// Per-segment automatic stride selection ("mini-OpenZL"). Game archives mix
// texture payloads (plane-friendly) with serialized headers (plane-hostile)
// inside one file; a global stride loses both ways. For each 64KB segment we
// trial-compress every candidate with zstd-1 (offline cost only) and keep
// the winner; decode side just reads 1 tag byte per segment.
// Layout: [u8 tag per segment][transformed segments back to back].
void deltaFwd(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t prev = 0;
    for (size_t i = 0; i < n; i++) { dst[i] = (uint8_t)(src[i] - prev); prev = src[i]; }
}
void deltaInv(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) { acc = (uint8_t)(acc + src[i]); dst[i] = acc; }
}

class AutoPlaneTransform final : public ITransform {
public:
    static constexpr size_t SEG = 64 * 1024;
    // candidate modes per segment; PxDy = plane stride x then byte delta
    // (delta after the split models slowly-varying same-significance bytes,
    // i.e. float/int columns inside serialized objects)
    enum : uint8_t {
        TAG_COPY = 0, TAG_P2 = 1, TAG_P4 = 2, TAG_P8 = 3, TAG_P16 = 4,
        TAG_D8 = 5, TAG_P4D = 6, TAG_P8D = 7, TAG_COUNT
    };

    const char* name() const override { return "autoplane"; }
    size_t maxOutput(size_t n) const override { return n + numSegs(n) + 16; }

    size_t forward(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) const override {
        const size_t nSeg = numSegs(n);
        if (cap < nSeg + n) return 0;
        uint8_t* tags = dst;
        uint8_t* out = dst + nSeg;

        thread_local std::vector<uint8_t> cand, probe;
        for (size_t s = 0; s < nSeg; s++) {
            const size_t off = s * SEG;
            const size_t len = (n - off < SEG) ? (n - off) : SEG;
            const uint8_t* sp = src + off;
            uint8_t* op = out + off;

            if (cand.size() < len) cand.resize(len);
            size_t pb = ZSTD_compressBound(len);
            if (probe.size() < pb) probe.resize(pb);

            size_t bestSize = zstdProbe(sp, len, probe); // candidate: copy
            uint8_t bestTag = TAG_COPY;
            for (uint8_t tag = 1; tag < TAG_COUNT; tag++) {
                applyFwd(sp, len, cand.data(), tag);
                size_t sz = zstdProbe(cand.data(), len, probe);
                // require a real margin: the probe codec (zstd-1) is weaker
                // than the final backend, and the split costs cross-segment
                // matches; small probe wins don't transfer
                if (sz + (bestSize >> 4) < bestSize) {
                    bestSize = sz;
                    bestTag = tag;
                }
            }
            tags[s] = bestTag;
            applyFwd(sp, len, op, bestTag);
        }
        return nSeg + n;
    }

    size_t inverse(const uint8_t* src, size_t encN, uint8_t* dst, size_t rawN) const override {
        const size_t nSeg = numSegs(rawN);
        if (encN != nSeg + rawN) return 0;
        const uint8_t* tags = src;
        const uint8_t* in = src + nSeg;
        thread_local std::vector<uint8_t> tmp;
        for (size_t s = 0; s < nSeg; s++) {
            const size_t off = s * SEG;
            const size_t len = (rawN - off < SEG) ? (rawN - off) : SEG;
            const uint8_t* ip = in + off;
            uint8_t* op = dst + off;
            switch (tags[s]) {
                case TAG_COPY: std::memcpy(op, ip, len); break;
                case TAG_P2:  planeInv(ip, len, op, 2); break;
                case TAG_P4:  planeInv(ip, len, op, 4); break;
                case TAG_P8:  planeInv(ip, len, op, 8); break;
                case TAG_P16: planeInv(ip, len, op, 16); break;
                case TAG_D8:  deltaInv(ip, len, op); break;
                case TAG_P4D:
                case TAG_P8D:
                    if (tmp.size() < len) tmp.resize(len);
                    deltaInv(ip, len, tmp.data());
                    planeInv(tmp.data(), len, op, tags[s] == TAG_P4D ? 4 : 8);
                    break;
                default: return 0;
            }
        }
        return rawN;
    }

private:
    static size_t numSegs(size_t n) { return (n + SEG - 1) / SEG; }

    static size_t zstdProbe(const uint8_t* p, size_t n, std::vector<uint8_t>& buf) {
        size_t r = ZSTD_compress(buf.data(), buf.size(), p, n, 1);
        return ZSTD_isError(r) ? n : r;
    }

    static void applyFwd(const uint8_t* src, size_t n, uint8_t* dst, uint8_t tag) {
        thread_local std::vector<uint8_t> tmp;
        switch (tag) {
            case TAG_P2:  planeFwd(src, n, dst, 2); break;
            case TAG_P4:  planeFwd(src, n, dst, 4); break;
            case TAG_P8:  planeFwd(src, n, dst, 8); break;
            case TAG_P16: planeFwd(src, n, dst, 16); break;
            case TAG_D8:  deltaFwd(src, n, dst); break;
            case TAG_P4D:
            case TAG_P8D:
                if (tmp.size() < n) tmp.resize(n);
                planeFwd(src, n, tmp.data(), tag == TAG_P4D ? 4 : 8);
                deltaFwd(tmp.data(), n, dst);
                break;
            default: std::memcpy(dst, src, n); break;
        }
    }
};

} // namespace

const ITransform* getTransform(const std::string& name) {
    static PlaneTransform s_p2("plane2", 2);
    static PlaneTransform s_p4("plane4", 4);
    static PlaneTransform s_p8("plane8", 8);
    static PlaneTransform s_p16("plane16", 16);
    static Delta8Transform s_d8;
    static AutoPlaneTransform s_auto;
    if (name == "plane2") return &s_p2;
    if (name == "plane4") return &s_p4;
    if (name == "plane8") return &s_p8;
    if (name == "plane16") return &s_p16;
    if (name == "delta8") return &s_d8;
    if (name == "autoplane") return &s_auto;
    return nullptr;
}

} // namespace gdc
