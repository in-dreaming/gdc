#include "codec.h"

// Unity's vendored old lz4 (r1xx API, symbols quarantined to UNITY_LZ4_*).
#include "lz4.h"
#include "lz4hc.h"

namespace gdc {

// Level mapping notes:
// - lz4 (fast path) has no level in this API version; cfg level is ignored.
// - lz4hc maps cfg level 0..9 -> HC compressionLevel via (level + 4),
//   so the dataset default level 5 lands on the HC default (9).

namespace {
constexpr size_t MAX_INPUT = 0x7E000000; // LZ4_MAX_INPUT_SIZE
}

class Lz4Codec final : public ICodec {
public:
    explicit Lz4Codec(bool hc) : m_hc(hc) {}
    CompType type() const override { return m_hc ? CompType::LZ4HC : CompType::LZ4; }
    size_t compressBound(size_t srcSize) const override {
        return (size_t)LZ4_compressBound((int)srcSize);
    }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const override {
        if (srcSize > MAX_INPUT) return 0;
        int r;
        if (m_hc) {
            int hcLevel = level + 4;
            if (hcLevel < 1) hcLevel = 1;
            if (hcLevel > 16) hcLevel = 16;
            r = LZ4_compressHC2_limitedOutput((const char*)src, (char*)dst, (int)srcSize, (int)dstCap, hcLevel);
        } else {
            r = LZ4_compress_limitedOutput((const char*)src, (char*)dst, (int)srcSize, (int)dstCap);
        }
        return r > 0 ? (size_t)r : 0;
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        int r = LZ4_decompress_safe((const char*)src, (char*)dst, (int)compSize, (int)rawSize);
        return (r >= 0 && (size_t)r == rawSize) ? rawSize : 0;
    }

private:
    bool m_hc;
};

const ICodec* createLz4Codec(bool hc) {
    static Lz4Codec s_fast(false);
    static Lz4Codec s_hc(true);
    return hc ? &s_hc : &s_fast;
}

} // namespace gdc
