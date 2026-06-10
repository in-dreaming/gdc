#include "codec.h"

#include "zstd.h"

namespace gdc {

// Level mapping note: the cfg level 0..9 is passed straight to zstd
// (0 -> ZSTD_CLEVEL_DEFAULT). If the runtime uses a different mapping,
// adjust here.

class ZstdCodec final : public ICodec {
public:
    CompType type() const override { return CompType::ZSTD; }
    size_t compressBound(size_t srcSize) const override { return ZSTD_compressBound(srcSize); }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const override {
        if (level <= 0) level = ZSTD_CLEVEL_DEFAULT;
        size_t r = ZSTD_compress(dst, dstCap, src, srcSize, level);
        return ZSTD_isError(r) ? 0 : r;
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        size_t r = ZSTD_decompress(dst, rawSize, src, compSize);
        return (!ZSTD_isError(r) && r == rawSize) ? rawSize : 0;
    }
};

const ICodec* createZstdCodec() {
    static ZstdCodec s_inst;
    return &s_inst;
}

} // namespace gdc
