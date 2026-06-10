#include "transforms.h"

#include <cstring>

namespace gdc {

namespace {

// Splits the input into <stride> byte planes: all byte-0s of each block,
// then all byte-1s, ... Tail bytes (n % stride) are copied verbatim.
// Turns "interleaved bit-field blocks" (ASTC/BCn) into per-field streams
// that LZ + entropy stages model far better.
class PlaneTransform final : public ITransform {
public:
    PlaneTransform(const char* name, size_t stride) : m_name(name), m_stride(stride) {}
    const char* name() const override { return m_name; }

    void forward(const uint8_t* src, size_t n, uint8_t* dst) const override {
        const size_t stride = m_stride;
        const size_t blocks = n / stride;
        for (size_t p = 0; p < stride; p++) {
            uint8_t* out = dst + p * blocks;
            const uint8_t* in = src + p;
            for (size_t i = 0; i < blocks; i++) out[i] = in[i * stride];
        }
        std::memcpy(dst + blocks * stride, src + blocks * stride, n - blocks * stride);
    }

    void inverse(const uint8_t* src, size_t n, uint8_t* dst) const override {
        const size_t stride = m_stride;
        const size_t blocks = n / stride;
        for (size_t p = 0; p < stride; p++) {
            const uint8_t* in = src + p * blocks;
            uint8_t* out = dst + p;
            for (size_t i = 0; i < blocks; i++) out[i * stride] = in[i];
        }
        std::memcpy(dst + blocks * stride, src + blocks * stride, n - blocks * stride);
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
    void forward(const uint8_t* src, size_t n, uint8_t* dst) const override {
        uint8_t prev = 0;
        for (size_t i = 0; i < n; i++) {
            dst[i] = (uint8_t)(src[i] - prev);
            prev = src[i];
        }
    }
    void inverse(const uint8_t* src, size_t n, uint8_t* dst) const override {
        uint8_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            acc = (uint8_t)(acc + src[i]);
            dst[i] = acc;
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
    if (name == "plane2") return &s_p2;
    if (name == "plane4") return &s_p4;
    if (name == "plane8") return &s_p8;
    if (name == "plane16") return &s_p16;
    if (name == "delta8") return &s_d8;
    return nullptr;
}

} // namespace gdc
