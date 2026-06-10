#pragma once

#include "cfg.h"
#include "codec.h"
#include "common.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gdc {

struct Agg {
    uint64_t files = 0;
    uint64_t blocks = 0;
    uint64_t rawBytes = 0;
    uint64_t compBytes = 0;
    double compSec = 0;
    double decompSec = 0;
    uint64_t failures = 0;

    void add(const Agg& o) {
        files += o.files; blocks += o.blocks;
        rawBytes += o.rawBytes; compBytes += o.compBytes;
        compSec += o.compSec; decompSec += o.decompSec;
        failures += o.failures;
    }
};

struct RunOptions {
    std::string dataRoot;   // contains raw/ and raw_cfg/
    std::string outDir = "out";
    int threads = 1;
    int repeat = 1;         // best-of-N timing
    uint64_t limitFiles = 0; // 0 = all
    uint64_t skipFiles = 0;
    std::string pkgFilter;  // substring match on package name
    std::string catFilter;  // exact match on source category (extension)
    bool verify = true;
    bool trace = false;     // log each entry path to stderr before processing
    // matrix mode only:
    std::vector<ResolvedCodec> matrixCodecs;
    uint64_t chunkSize = 0; // 0 = whole file; else compress in independent N-byte pages
};

struct RunResult {
    std::map<std::string, Agg> byCodec;    // "lz4@5"
    std::map<std::string, Agg> byCategory; // source extension
    std::map<std::string, Agg> byPkg;
    Agg total;
    uint64_t missingFiles = 0;
    uint64_t badLines = 0;
};

// baseline: compress each block exactly as its cfg prescribes.
RunResult runBaseline(const RunOptions& opt);

// matrix: run each codec spec over every file (whole file as one block).
RunResult runMatrix(const RunOptions& opt);

void writeReports(const RunResult& r, const RunOptions& opt, const std::string& mode);

} // namespace gdc
