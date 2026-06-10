#include "codec.h"
#include "gdc/gdc1.h"
#include "gdc/transforms.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gdc {

const char* compTypeName(CompType t) {
    switch (t) {
        case CompType::None: return "none";
        case CompType::LZ4: return "lz4";
        case CompType::LZ4HC: return "lz4hc";
        case CompType::OodleKraken: return "kraken";
        case CompType::OodleLeviathan: return "leviathan";
        case CompType::OodleMermaid: return "mermaid";
        case CompType::OodleSelkie: return "selkie";
        case CompType::LZ3: return "lz3";
        case CompType::LZ3HUF: return "lz3huf";
        case CompType::ZSTD: return "zstd";
        case CompType::GDC0: return "gdc0";
        case CompType::GDC1: return "gdc1";
        default: return "unknown";
    }
}

bool compTypeFromName(const std::string& name, CompType& out) {
    static const CompType all[] = {
        CompType::None, CompType::LZ4, CompType::LZ4HC, CompType::OodleKraken,
        CompType::OodleLeviathan, CompType::OodleMermaid, CompType::OodleSelkie,
        CompType::LZ3, CompType::LZ3HUF, CompType::ZSTD, CompType::GDC0, CompType::GDC1,
    };
    for (CompType t : all) {
        if (name == compTypeName(t)) { out = t; return true; }
    }
    return false;
}

// ---------------- none ----------------

class NoneCodec final : public ICodec {
public:
    CompType type() const override { return CompType::None; }
    size_t compressBound(size_t srcSize) const override { return srcSize; }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int) const override {
        if (dstCap < srcSize) return 0;
        std::memcpy(dst, src, srcSize);
        return srcSize;
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        if (compSize != rawSize) return 0;
        std::memcpy(dst, src, rawSize);
        return rawSize;
    }
};

// ---------------- gdc0: self-built LZ backend (v0) ----------------
//
// Greedy LZ77 with a 64K window and LZ4-style byte-aligned token format:
//   [token: lit<<4 | (mlen-4)] [lit ext 0xff*] [literals] [offset u16le] [mlen ext 0xff*]
// The stream always ends with a literal-only sequence (offset omitted when
// output is complete). This is the slot where future GDC transforms +
// entropy stage will land; v0 establishes the harness plumbing.

namespace gdc0 {

static constexpr size_t MIN_MATCH = 4;
static constexpr size_t LAST_LITERALS = 5;
static constexpr size_t HASH_LOG = 16;

static inline uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

static inline uint32_t hash4(uint32_t v) {
    return (v * 2654435761u) >> (32 - HASH_LOG);
}

static size_t writeLen(uint8_t* dst, size_t len) {
    size_t n = 0;
    while (len >= 255) { dst[n++] = 255; len -= 255; }
    dst[n++] = (uint8_t)len;
    return n;
}

static size_t compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap) {
    if (srcSize == 0) return 0;
    std::vector<uint32_t> table(1u << HASH_LOG, 0); // pos+1, 0 = empty

    const uint8_t* const end = src + srcSize;
    const uint8_t* const mfLimit = (srcSize > MIN_MATCH + LAST_LITERALS + 8)
        ? end - (MIN_MATCH + LAST_LITERALS) : src;
    const uint8_t* anchor = src;
    const uint8_t* p = src;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dstCap;

    auto emit = [&](size_t litLen, const uint8_t* lits, size_t mLen, size_t offset) -> bool {
        // worst case: token + lit ext + lits + offset + mlen ext
        size_t need = 1 + litLen / 255 + 1 + litLen + 2 + (mLen ? mLen / 255 + 1 : 0) + 8;
        if ((size_t)(oend - op) < need) return false;
        uint8_t* token = op++;
        size_t litCode = litLen < 15 ? litLen : 15;
        if (litLen >= 15) op += writeLen(op, litLen - 15);
        std::memcpy(op, lits, litLen);
        op += litLen;
        if (mLen == 0) { *token = (uint8_t)(litCode << 4); return true; } // terminal
        size_t mCode = (mLen - MIN_MATCH) < 15 ? (mLen - MIN_MATCH) : 15;
        *token = (uint8_t)((litCode << 4) | mCode);
        op[0] = (uint8_t)(offset & 0xff);
        op[1] = (uint8_t)(offset >> 8);
        op += 2;
        if (mLen - MIN_MATCH >= 15) op += writeLen(op, mLen - MIN_MATCH - 15);
        return true;
    };

    while (p < mfLimit) {
        uint32_t h = hash4(read32(p));
        uint32_t candPlus = table[h];
        table[h] = (uint32_t)(p - src) + 1;
        if (candPlus) {
            const uint8_t* cand = src + candPlus - 1;
            size_t dist = (size_t)(p - cand);
            if (dist > 0 && dist <= 0xffff && read32(cand) == read32(p)) {
                // extend
                const uint8_t* q = p + MIN_MATCH;
                const uint8_t* c = cand + MIN_MATCH;
                const uint8_t* qlimit = end - LAST_LITERALS;
                while (q < qlimit && *q == *c) { q++; c++; }
                size_t mLen = (size_t)(q - p);
                if (!emit((size_t)(p - anchor), anchor, mLen, dist)) return 0;
                p += mLen;
                anchor = p;
                continue;
            }
        }
        p++;
    }
    // terminal literals
    if (!emit((size_t)(end - anchor), anchor, 0, 0)) return 0;
    return (size_t)(op - dst);
}

static size_t decompress(const uint8_t* src, size_t compSize, uint8_t* dst, size_t rawSize) {
    const uint8_t* ip = src;
    const uint8_t* const iend = src + compSize;
    uint8_t* op = dst;
    uint8_t* const oend = dst + rawSize;

    auto readLen = [&](size_t base) -> size_t {
        size_t len = base;
        if (base == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return SIZE_MAX;
                b = *ip++;
                len += b;
            } while (b == 255);
        }
        return len;
    };

    while (ip < iend) {
        uint8_t token = *ip++;
        size_t litLen = readLen(token >> 4);
        if (litLen == SIZE_MAX) return 0;
        if ((size_t)(iend - ip) < litLen || (size_t)(oend - op) < litLen) return 0;
        std::memcpy(op, ip, litLen);
        ip += litLen;
        op += litLen;
        if (op == oend) break; // terminal sequence
        if ((size_t)(iend - ip) < 2) return 0;
        size_t offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        size_t mLen = readLen(token & 0xf);
        if (mLen == SIZE_MAX) return 0;
        mLen += MIN_MATCH;
        if (offset == 0 || (size_t)(op - dst) < offset || (size_t)(oend - op) < mLen) return 0;
        const uint8_t* m = op - offset;
        for (size_t i = 0; i < mLen; i++) op[i] = m[i]; // overlap-safe
        op += mLen;
    }
    return op == oend ? rawSize : 0;
}

} // namespace gdc0

class Gdc0Codec final : public ICodec {
public:
    CompType type() const override { return CompType::GDC0; }
    size_t compressBound(size_t srcSize) const override { return srcSize + srcSize / 255 + 64; }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int) const override {
        return gdc0::compress((const uint8_t*)src, srcSize, (uint8_t*)dst, dstCap);
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        return gdc0::decompress((const uint8_t*)src, compSize, (uint8_t*)dst, rawSize);
    }
};

// ---------------- gdc1: self-built backend (v1) ----------------

class Gdc1Codec final : public ICodec {
public:
    CompType type() const override { return CompType::GDC1; }
    size_t compressBound(size_t srcSize) const override { return gdc1::compressBound(srcSize); }
    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const override {
        return gdc1::compress((const uint8_t*)src, srcSize, (uint8_t*)dst, dstCap, level);
    }
    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        return gdc1::decompress((const uint8_t*)src, compSize, (uint8_t*)dst, rawSize);
    }
};

// ---------------- transform pipeline ----------------

class PipelineCodec final : public ICodec {
public:
    PipelineCodec(std::vector<const ITransform*> tf, const ICodec* backend, std::string name)
        : m_tf(std::move(tf)), m_backend(backend), m_name(std::move(name)) {}

    CompType type() const override { return m_backend->type(); }
    std::string name() const override { return m_name; }
    size_t compressBound(size_t srcSize) const override { return m_backend->compressBound(srcSize); }

    size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCap, int level) const override {
        thread_local std::vector<uint8_t> a, b;
        const uint8_t* cur = (const uint8_t*)src;
        bool useA = true;
        for (const ITransform* t : m_tf) {
            std::vector<uint8_t>& out = useA ? a : b;
            if (out.size() < srcSize) out.resize(srcSize);
            t->forward(cur, srcSize, out.data());
            cur = out.data();
            useA = !useA;
        }
        return m_backend->compress(cur, srcSize, dst, dstCap, level);
    }

    size_t decompress(const void* src, size_t compSize, void* dst, size_t rawSize) const override {
        if (m_tf.empty()) return m_backend->decompress(src, compSize, dst, rawSize);
        thread_local std::vector<uint8_t> a, b;
        if (a.size() < rawSize) a.resize(rawSize);
        if (m_backend->decompress(src, compSize, a.data(), rawSize) != rawSize) return 0;
        const uint8_t* cur = a.data();
        // apply inverse transforms in reverse order; last one writes to dst
        for (size_t i = m_tf.size(); i-- > 0;) {
            uint8_t* out;
            if (i == 0) {
                out = (uint8_t*)dst;
            } else {
                std::vector<uint8_t>& buf = (cur == a.data()) ? b : a;
                if (buf.size() < rawSize) buf.resize(rawSize);
                out = buf.data();
            }
            m_tf[i]->inverse(cur, rawSize, out);
            cur = out;
        }
        return rawSize;
    }

private:
    std::vector<const ITransform*> m_tf;
    const ICodec* m_backend;
    std::string m_name;
};

// ---------------- spec resolution ----------------

namespace {
std::mutex g_pipeMutex;
std::vector<std::unique_ptr<ICodec>> g_pipelines;
} // namespace

bool resolveCodecSpec(const std::string& item, ResolvedCodec& out, std::string& err) {
    std::string spec = item;
    out.level = 5;
    size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        out.level = std::atoi(spec.c_str() + colon + 1);
        spec.resize(colon);
    }
    // split by '+': leading parts are transforms, last part is the backend
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t plus = spec.find('+', start);
        parts.push_back(spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (parts.empty() || parts.back().empty()) { err = "empty codec spec: " + item; return false; }

    CompType bt;
    if (!compTypeFromName(parts.back(), bt)) { err = "unknown backend: " + parts.back(); return false; }
    const ICodec* backend = getCodec(bt);
    if (!backend) { err = "backend unavailable: " + parts.back(); return false; }

    std::vector<const ITransform*> tf;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        const ITransform* t = getTransform(parts[i]);
        if (!t) { err = "unknown transform: " + parts[i]; return false; }
        tf.push_back(t);
    }

    out.key = spec + "@" + std::to_string(out.level);
    if (tf.empty()) {
        out.codec = backend;
    } else {
        std::lock_guard<std::mutex> lk(g_pipeMutex);
        g_pipelines.push_back(std::make_unique<PipelineCodec>(std::move(tf), backend, spec));
        out.codec = g_pipelines.back().get();
    }
    return true;
}

// ---------------- factory ----------------

const ICodec* createLz4Codec(bool hc);
const ICodec* createLz3Codec(bool huf);
const ICodec* createZstdCodec();
const ICodec* createOodleCodec(CompType t);

const ICodec* getCodec(CompType t) {
    static NoneCodec s_none;
    static Gdc0Codec s_gdc0;
    static Gdc1Codec s_gdc1;
    switch (t) {
        case CompType::None: return &s_none;
        case CompType::GDC0: return &s_gdc0;
        case CompType::GDC1: return &s_gdc1;
        case CompType::LZ4: return createLz4Codec(false);
        case CompType::LZ4HC: return createLz4Codec(true);
        case CompType::LZ3: return createLz3Codec(false);
        case CompType::LZ3HUF: return createLz3Codec(true);
        case CompType::ZSTD: return createZstdCodec();
        case CompType::OodleKraken:
        case CompType::OodleLeviathan:
        case CompType::OodleMermaid:
        case CompType::OodleSelkie:
            return createOodleCodec(t);
        default: return nullptr;
    }
}

} // namespace gdc
