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
    // dict experiments (T6); the plain compress/decompress paths above are
    // untouched (baseline semantics preserved)
    bool supportsDict() const override { return true; }
    size_t compressDict(const void* src, size_t srcSize, void* dst, size_t dstCap, int level,
                        const void* dict, size_t dictSize) const override {
        if (level <= 0) level = ZSTD_CLEVEL_DEFAULT;
        thread_local ZSTD_CCtx* cctx = ZSTD_createCCtx();
        size_t r = ZSTD_compress_usingDict(cctx, dst, dstCap, src, srcSize, dict, dictSize, level);
        return ZSTD_isError(r) ? 0 : r;
    }
    size_t decompressDict(const void* src, size_t compSize, void* dst, size_t rawSize,
                          const void* dict, size_t dictSize) const override {
        thread_local ZSTD_DCtx* dctx = ZSTD_createDCtx();
        size_t r = ZSTD_decompress_usingDict(dctx, dst, rawSize, src, compSize, dict, dictSize);
        return (!ZSTD_isError(r) && r == rawSize) ? rawSize : 0;
    }
};

const ICodec* createZstdCodec() {
    static ZstdCodec s_inst;
    return &s_inst;
}

} // namespace gdc
