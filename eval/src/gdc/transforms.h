#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gdc {

// Content-aware byte transforms (感知压缩 forward/inverse pair).
// forward() runs at build time before the backend codec; inverse() runs at
// load time after backend decode. Both must be pure integer & deterministic.
// forward may emit a small self-describing header (e.g. per-segment mode
// tags), so output size can exceed input size up to maxOutput().
class ITransform {
public:
    virtual ~ITransform() = default;
    virtual const char* name() const = 0;
    virtual size_t maxOutput(size_t n) const { return n; }
    // Returns output size, 0 on failure (cap too small).
    virtual size_t forward(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) const = 0;
    // encN = forward() output size, rawN = original input size.
    // Returns rawN on success, 0 on corruption.
    virtual size_t inverse(const uint8_t* src, size_t encN, uint8_t* dst, size_t rawN) const = 0;
};

// Registry:
//   plane2/4/8/16  byte-plane split with stride N (BCn/ETC2 8B, ASTC/BC7 16B blocks)
//   delta8         byte delta (numeric streams)
//   autoplane      per-64KB-segment automatic stride selection via trial
//                  compression (offline cost only); 1 tag byte per segment
// Returns nullptr for unknown names.
const ITransform* getTransform(const std::string& name);

} // namespace gdc
