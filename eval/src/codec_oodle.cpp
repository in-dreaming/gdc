#include "codec.h"

// Building against the oodle2 core sources (OODLE_BUILDING_DATA is defined
// project-wide for this target via CMake on oodle_ref; we define it here for
// the public header pull-in).
#ifndef OODLE_BUILDING_DATA
#define OODLE_BUILDING_DATA 1
#endif
#ifndef OODLE_BUILDING_LIB
#define OODLE_BUILDING_LIB 1
#endif
#include "oodlecore.h"
#include "oodlelzpub.h"

// oodle2 core declares everything inside its namespace (OODLE_NS -> OO2_NS).
using namespace OODLE_NS;

namespace gdc {

namespace {

OodleLZ_Compressor toCompressor(CompType t) {
    switch (t) {
        case CompType::OodleKraken: return OodleLZ_Compressor_Kraken;
        case CompType::OodleLeviathan: return OodleLZ_Compressor_Leviathan;
        case CompType::OodleMermaid: return OodleLZ_Compressor_Mermaid;
        case CompType::OodleSelkie: return OodleLZ_Compressor_Selkie;
        default: return OodleLZ_Compressor_Invalid;
    }
}

// cfg level 0..9 maps 1:1 onto OodleLZ_CompressionLevel (None..Optimal5).
OodleLZ_CompressionLevel toLevel(int level) {
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    return (OodleLZ_CompressionLevel)level;
}

} // namespace

class OodleCodec final : public ICodec {
public:
    explicit OodleCodec(CompType t) : m_type(t) {}
    CompType type() const override { return m_type; }
    size_t compressBound(size_t srcSize) const override {
        return (size_t)OodleLZ_GetCompressedBufferSizeNeeded(toCompressor(m_type), (OO_SINTa)srcSize);
    }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const override {
        if (dstCap < compressBound(srcSize)) return 0;
        OO_SINTa r = OodleLZ_Compress(toCompressor(m_type), src, (OO_SINTa)srcSize, dst, toLevel(level));
        return r > 0 ? (size_t)r : 0;
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        OO_SINTa r = OodleLZ_Decompress(src, (OO_SINTa)compSize, dst, (OO_SINTa)rawSize,
                                        OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No,
                                        OodleLZ_Verbosity_None);
        return (r == (OO_SINTa)rawSize) ? rawSize : 0;
    }

private:
    CompType m_type;
};

const ICodec* createOodleCodec(CompType t) {
    static OodleCodec s_kraken(CompType::OodleKraken);
    static OodleCodec s_leviathan(CompType::OodleLeviathan);
    static OodleCodec s_mermaid(CompType::OodleMermaid);
    static OodleCodec s_selkie(CompType::OodleSelkie);
    switch (t) {
        case CompType::OodleKraken: return &s_kraken;
        case CompType::OodleLeviathan: return &s_leviathan;
        case CompType::OodleMermaid: return &s_mermaid;
        case CompType::OodleSelkie: return &s_selkie;
        default: return nullptr;
    }
}

} // namespace gdc
