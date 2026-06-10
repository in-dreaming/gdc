#include "codec.h"

#include "lz3.h"

#include <cstring>

namespace gdc {

// LZ3 one-shot API is limited to LZ3_MAX_BLOCK_SIZE (~64K) per call, so we
// chunk internally and frame each chunk with its u32 compressed size.
// Chunk raw sizes are derived from rawSize on decode, so no raw size field
// is needed. The 4-byte/chunk framing is counted in the compressed size.

namespace {

constexpr size_t CHUNK = 0xFF80; // <= LZ3_MAX_BLOCK_SIZE (0xFF81)

LZ3_CLevel toLevel(int level) {
    if (level < (int)LZ3_CLevel_Min) return LZ3_CLevel_Min;
    if (level > (int)LZ3_CLevel_Max) return LZ3_CLevel_Max;
    return (LZ3_CLevel)level;
}

} // namespace

class Lz3Codec final : public ICodec {
public:
    explicit Lz3Codec(bool huf) : m_huf(huf) {}
    CompType type() const override { return m_huf ? CompType::LZ3HUF : CompType::LZ3; }
    size_t compressBound(size_t srcSize) const override {
        size_t chunks = srcSize / CHUNK + 1;
        return srcSize + srcSize / 8 + chunks * (4 + 64) + 256;
    }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const override {
        const uint8_t* ip = (const uint8_t*)src;
        uint8_t* op = (uint8_t*)dst;
        size_t remaining = srcSize;
        size_t total = 0;
        while (remaining > 0) {
            size_t n = remaining < CHUNK ? remaining : CHUNK;
            size_t need = 4 + n + n / 8 + 256;
            if (dstCap - total < need) return 0;
            uint32_t c = m_huf
                ? LZ3_compress_HUF(ip, op + 4, (uint32_t)n, toLevel(level))
                : LZ3_compress(ip, op + 4, (uint32_t)n, toLevel(level));
            if (c == 0) return 0;
            std::memcpy(op, &c, 4);
            op += 4 + c;
            total += 4 + c;
            ip += n;
            remaining -= n;
        }
        return total;
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        const uint8_t* ip = (const uint8_t*)src;
        const uint8_t* iend = ip + compSize;
        uint8_t* op = (uint8_t*)dst;
        size_t remaining = rawSize;
        while (remaining > 0) {
            if (iend - ip < 4) return 0;
            uint32_t c;
            std::memcpy(&c, ip, 4);
            ip += 4;
            if ((size_t)(iend - ip) < c) return 0;
            size_t n = remaining < CHUNK ? remaining : CHUNK;
            uint32_t used = m_huf
                ? LZ3_decompress_HUF_fast(ip, op, (uint32_t)n)
                : LZ3_decompress_fast(ip, op, (uint32_t)n);
            if (used != c) return 0;
            ip += c;
            op += n;
            remaining -= n;
        }
        return ip == iend ? rawSize : 0;
    }

private:
    bool m_huf;
};

const ICodec* createLz3Codec(bool huf) {
    static Lz3Codec s_plain(false);
    static Lz3Codec s_huf(true);
    return huf ? &s_huf : &s_plain;
}

} // namespace gdc
