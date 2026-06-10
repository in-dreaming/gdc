#pragma once

#include "common.h"

#include <cstddef>
#include <string>

namespace gdc {

// Stateless one-shot codec interface. Implementations must be thread-safe
// (no shared mutable state across calls; thread_local scratch is fine).
class ICodec {
public:
    virtual ~ICodec() = default;
    virtual CompType type() const = 0;
    virtual std::string name() const { return compTypeName(type()); }

    virtual size_t compressBound(size_t srcSize) const = 0;
    // Returns compressed size, 0 on failure.
    virtual size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const = 0;
    // rawSize is known by the caller. Returns rawSize on success, 0 on failure.
    virtual size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const = 0;

    // Optional shared-dictionary support (T6). decompressDict must receive
    // the same dict that was used to compress.
    virtual bool supportsDict() const { return false; }
    virtual size_t compressDict(const void* src, size_t srcSize, void* dst, size_t dstCap, int level,
                                const void* dict, size_t dictSize) const {
        (void)dict; (void)dictSize;
        return compress(src, srcSize, dst, dstCap, level);
    }
    virtual size_t decompressDict(const void* src, size_t compSize, void* dst, size_t rawSize,
                                  const void* dict, size_t dictSize) const {
        (void)dict; (void)dictSize;
        return decompress(src, compSize, dst, rawSize);
    }
};

// Returns nullptr for unknown type. Instances are process-wide singletons.
const ICodec* getCodec(CompType t);

// A codec spec resolved from a string like:
//   "zstd:9"                   plain backend
//   "plane16+zstd:9"           transform chain + backend
//   "plane16+delta8+gdc1:7"    multiple transforms
struct ResolvedCodec {
    const ICodec* codec = nullptr;
    int level = 5;
    std::string key; // stats key, e.g. "plane16+zstd@9"
};

// Resolves a spec item (must be called before worker threads start).
// Returns false and fills err on bad spec.
bool resolveCodecSpec(const std::string& item, ResolvedCodec& out, std::string& err);

} // namespace gdc
