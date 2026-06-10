#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gdc {

// Size-preserving, bijective byte transforms (感知压缩 forward/inverse pair).
// forward() runs at build time before the backend codec; inverse() runs at
// load time after backend decode. Both must be pure integer, deterministic,
// and fast (linear scan, SIMD-friendly).
class ITransform {
public:
    virtual ~ITransform() = default;
    virtual const char* name() const = 0;
    virtual void forward(const uint8_t* src, size_t n, uint8_t* dst) const = 0;
    virtual void inverse(const uint8_t* src, size_t n, uint8_t* dst) const = 0;
};

// Registry: "plane2/4/8/16" (byte-plane split with stride N, for fixed-size
// block data such as BCn/ETC2 8B and ASTC/BC7 16B blocks), "delta8" (byte
// delta, for numeric streams). Returns nullptr for unknown names.
const ITransform* getTransform(const std::string& name);

} // namespace gdc
