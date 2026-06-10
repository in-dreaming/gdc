#include "bench.h"
#include "codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace gdc;

static void usage() {
    std::printf(
        "gdceval - game data compression evaluation harness\n"
        "\n"
        "usage:\n"
        "  gdceval baseline --data <root> [options]\n"
        "      compress every block exactly as prescribed by raw_cfg (the\n"
        "      dataset's own compression strategy) and report ratio/speed.\n"
        "  gdceval matrix --data <root> --codecs <spec> [options]\n"
        "      run codec list over every file (whole file or --chunk pages).\n"
        "      spec: comma list of [transform+...]backend[:level], e.g.\n"
        "            lz4,zstd:5,kraken:5,gdc1:7,plane16+zstd:9,plane16+delta8+gdc1:5\n"
        "      backends:   none lz4 lz4hc kraken leviathan mermaid selkie lz3 lz3huf zstd gdc0 gdc1\n"
        "      transforms: plane2 plane4 plane8 plane16 delta8\n"
        "\n"
        "options:\n"
        "  --data <dir>     data root containing raw/ and raw_cfg/ (required)\n"
        "  --out <dir>      output dir for csv reports (default: out)\n"
        "  --threads <n>    worker threads (default: hw/2)\n"
        "  --repeat <n>     best-of-n timing per block (default: 1)\n"
        "  --limit <n>      only process first n file entries (smoke test)\n"
        "  --pkg <substr>   only packages whose name contains substr\n"
        "  --cat <ext>      only files whose source category (extension) equals ext\n"
        "  --chunk <n>      matrix: compress in independent n-byte pages (random-access sim)\n"
        "  --no-verify      skip roundtrip memcmp\n");
}

static bool parseCodecs(const std::string& spec, std::vector<ResolvedCodec>& out) {
    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) {
            ResolvedCodec rc;
            std::string err;
            if (!resolveCodecSpec(item, rc, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            out.push_back(std::move(rc));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return !out.empty();
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string mode = argv[1];
    if (mode != "baseline" && mode != "matrix") { usage(); return 1; }

    RunOptions opt;
    opt.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    std::string codecSpec;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(1); }
            return argv[++i];
        };
        if (a == "--data") opt.dataRoot = need("--data");
        else if (a == "--out") opt.outDir = need("--out");
        else if (a == "--threads") opt.threads = std::atoi(need("--threads"));
        else if (a == "--repeat") opt.repeat = std::max(1, std::atoi(need("--repeat")));
        else if (a == "--limit") opt.limitFiles = std::strtoull(need("--limit"), nullptr, 10);
        else if (a == "--skip") opt.skipFiles = std::strtoull(need("--skip"), nullptr, 10);
        else if (a == "--trace") opt.trace = true;
        else if (a == "--pkg") opt.pkgFilter = need("--pkg");
        else if (a == "--cat") opt.catFilter = need("--cat");
        else if (a == "--codecs") codecSpec = need("--codecs");
        else if (a == "--chunk") opt.chunkSize = std::strtoull(need("--chunk"), nullptr, 10);
        else if (a == "--no-verify") opt.verify = false;
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 1; }
    }
    if (opt.dataRoot.empty()) { std::fprintf(stderr, "--data is required\n"); return 1; }

    RunResult result;
    if (mode == "baseline") {
        result = runBaseline(opt);
    } else {
        if (!parseCodecs(codecSpec, opt.matrixCodecs)) {
            std::fprintf(stderr, "--codecs is required for matrix mode\n");
            return 1;
        }
        result = runMatrix(opt);
    }
    writeReports(result, opt, mode);
    return result.total.failures > 0 ? 2 : 0;
}
