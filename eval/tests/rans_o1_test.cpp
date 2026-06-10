// Standalone fuzz/roundtrip test for rans::encodeO1/decodeO1 and gdc1.
#include "../src/gdc/rans.h"
#include "../src/gdc/gdc1.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace gdc;

static int failures = 0;

static void checkO1(const std::vector<uint8_t>& data, const char* tag) {
    std::vector<uint8_t> enc(data.size() + 65536);
    size_t e = rans::encodeO1(data.data(), data.size(), enc.data(), enc.size());
    if (e == 0) return; // didn't shrink, legal
    std::vector<uint8_t> dec(data.size(), 0xCD);
    size_t d = rans::decodeO1(enc.data(), e, dec.data(), data.size());
    if (d != data.size() || std::memcmp(dec.data(), data.data(), data.size()) != 0) {
        printf("O1 FAIL %s n=%zu enc=%zu\n", tag, data.size(), e);
        failures++;
    }
}

static void checkGdc1(const std::vector<uint8_t>& data, int level, const char* tag) {
    std::vector<uint8_t> enc(gdc1::compressBound(data.size()));
    size_t e = gdc1::compress(data.data(), data.size(), enc.data(), enc.size(), level);
    if (e == 0) { printf("GDC1 compress fail %s n=%zu\n", tag, data.size()); failures++; return; }
    std::vector<uint8_t> dec(data.size(), 0xCD);
    size_t d = gdc1::decompress(enc.data(), e, dec.data(), data.size());
    if (d != data.size() || std::memcmp(dec.data(), data.data(), data.size()) != 0) {
        printf("GDC1 FAIL %s n=%zu lvl=%d enc=%zu\n", tag, data.size(), level, e);
        failures++;
    }
}

// File mode: roundtrip a real file through gdc1 and the o1 coder directly.
// Prints only sizes/indices, never data content.
static int testFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)n);
    fread(data.data(), 1, (size_t)n, f);
    fclose(f);
    printf("file size=%ld\n", n);

    // gdc1 roundtrip
    std::vector<uint8_t> enc(gdc1::compressBound(data.size()));
    size_t e = gdc1::compress(data.data(), data.size(), enc.data(), enc.size(), 9);
    printf("gdc1 enc=%zu\n", e);
    if (e) {
        std::vector<uint8_t> dec(data.size(), 0xCD);
        size_t d = gdc1::decompress(enc.data(), e, dec.data(), data.size());
        if (d != data.size()) {
            printf("gdc1 decompress returned %zu\n", d);
        } else {
            size_t firstBad = SIZE_MAX;
            for (size_t i = 0; i < data.size(); i++) {
                if (dec[i] != data[i]) { firstBad = i; break; }
            }
            printf("gdc1 roundtrip %s firstBad=%zu\n", firstBad == SIZE_MAX ? "OK" : "MISMATCH", firstBad);
        }
    }

    // direct o1 roundtrip of the whole file (treat as one stream)
    std::vector<uint8_t> enc2(data.size() + 65536);
    size_t e2 = rans::encodeO1(data.data(), data.size(), enc2.data(), enc2.size());
    printf("o1 enc=%zu\n", e2);
    if (e2) {
        std::vector<uint8_t> dec2(data.size(), 0xCD);
        size_t d2 = rans::decodeO1(enc2.data(), e2, dec2.data(), data.size());
        size_t firstBad = SIZE_MAX;
        if (d2 == data.size()) {
            for (size_t i = 0; i < data.size(); i++) {
                if (dec2[i] != data[i]) { firstBad = i; break; }
            }
        }
        printf("o1 roundtrip ret=%zu %s firstBad=%zu\n", d2,
               (d2 == data.size() && firstBad == SIZE_MAX) ? "OK" : "MISMATCH", firstBad);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return testFile(argv[1]);
    std::mt19937 rng(12345);

    for (int iter = 0; iter < 300; iter++) {
        size_t n = 1 + (rng() % 300000);
        std::vector<uint8_t> data(n);
        int kind = iter % 5;
        if (kind == 0) { // skewed bytes
            for (auto& b : data) b = (uint8_t)(rng() % 16);
        } else if (kind == 1) { // text-ish
            for (auto& b : data) b = (uint8_t)('a' + rng() % 26);
        } else if (kind == 2) { // structured floats-ish
            for (size_t i = 0; i < n; i++) data[i] = (uint8_t)((i * 7) ^ (i >> 8));
        } else if (kind == 3) { // runs
            uint8_t v = 0;
            for (size_t i = 0; i < n; i++) { if (rng() % 50 == 0) v = (uint8_t)rng(); data[i] = v; }
        } else { // random (incompressible)
            for (auto& b : data) b = (uint8_t)rng();
        }
        checkO1(data, "fuzz");
        checkGdc1(data, (int)(rng() % 10), "fuzz");
    }
    printf(failures ? "FAILURES: %d\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
