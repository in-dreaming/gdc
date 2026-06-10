#include "bench.h"
#include "codec.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace gdc {

namespace {

constexpr uint64_t MAX_FILE_BYTES = 1ull << 31; // skip >2GB files

std::string codecKey(CompType t, int level) {
    return std::string(compTypeName(t)) + "@" + std::to_string(level);
}

bool readFileBytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::error_code ec;
    uint64_t sz = fs::file_size(p, ec);
    if (ec || sz > MAX_FILE_BYTES) return false;
    out.resize((size_t)sz);
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    if (sz > 0 && !in.read((char*)out.data(), (std::streamsize)sz)) return false;
    return true;
}

struct LocalState {
    std::vector<uint8_t> fileBuf;
    std::vector<uint8_t> compBuf;
    std::vector<uint8_t> verifyBuf;
};

struct BlockTask {
    const ICodec* codec;
    int level;
    const uint8_t* src;
    size_t size;
    const uint8_t* dict = nullptr; // optional shared dict (dict-capable codecs only)
    size_t dictSize = 0;
};

// Runs one block through compress (+verify decompress), updating agg.
void runBlock(const BlockTask& t, int repeat, bool verify, LocalState& ls, Agg& agg) {
    const ICodec* codec = t.codec;
    if (!codec) { agg.failures++; return; }
    const bool useDict = t.dict && t.dictSize && codec->supportsDict();

    agg.blocks++;
    agg.rawBytes += t.size;
    if (t.size == 0) return;

    size_t bound = codec->compressBound(t.size);
    if (ls.compBuf.size() < bound) ls.compBuf.resize(bound);

    size_t compSize = 0;
    double bestC = 1e30;
    for (int i = 0; i < repeat; i++) {
        auto t0 = Clock::now();
        compSize = useDict
            ? codec->compressDict(t.src, t.size, ls.compBuf.data(), ls.compBuf.size(), t.level,
                                  t.dict, t.dictSize)
            : codec->compress(t.src, t.size, ls.compBuf.data(), ls.compBuf.size(), t.level);
        double dt = secondsSince(t0);
        bestC = std::min(bestC, dt);
        if (compSize == 0) break;
    }
    if (compSize == 0) { agg.failures++; return; }
    agg.compSec += bestC;
    agg.compBytes += compSize;

    if (ls.verifyBuf.size() < t.size) ls.verifyBuf.resize(t.size);
    size_t got = 0;
    double bestD = 1e30;
    for (int i = 0; i < repeat; i++) {
        auto t0 = Clock::now();
        got = useDict
            ? codec->decompressDict(ls.compBuf.data(), compSize, ls.verifyBuf.data(), t.size,
                                    t.dict, t.dictSize)
            : codec->decompress(ls.compBuf.data(), compSize, ls.verifyBuf.data(), t.size);
        double dt = secondsSince(t0);
        bestD = std::min(bestD, dt);
        if (got == 0) break;
    }
    agg.decompSec += bestD;
    if (got != t.size || (verify && std::memcmp(ls.verifyBuf.data(), t.src, t.size) != 0)) {
        agg.failures++;
    }
}

struct SharedStats {
    std::mutex mu;
    RunResult result;

    void merge(const std::string& codec, const std::string& cat, const std::string& pkg, const Agg& a) {
        std::lock_guard<std::mutex> lk(mu);
        result.byCodec[codec].add(a);
        result.byCategory[cat].add(a);
        result.byPkg[pkg].add(a);
        result.total.add(a);
    }
};

std::vector<FileEntry> loadEntries(const RunOptions& opt, RunResult& result) {
    fs::path cfgDir = fs::path(opt.dataRoot) / "raw_cfg";
    std::vector<FileEntry> entries = parseCfgDir(cfgDir.string(), result.badLines);
    if (!opt.pkgFilter.empty()) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const FileEntry& e) {
            return e.pkg.find(opt.pkgFilter) == std::string::npos;
        }), entries.end());
    }
    if (!opt.catFilter.empty()) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const FileEntry& e) {
            return srcCategory(e.srcPath) != opt.catFilter;
        }), entries.end());
    }
    if (opt.skipFiles > 0) {
        if (opt.skipFiles >= entries.size()) entries.clear();
        else entries.erase(entries.begin(), entries.begin() + (size_t)opt.skipFiles);
    }
    if (opt.limitFiles > 0 && entries.size() > opt.limitFiles) entries.resize((size_t)opt.limitFiles);
    return entries;
}

// Builds a shared raw-content dictionary by sampling head bytes of the first
// entries (T6 experiments). Deterministic; only aggregated bytes are kept in
// memory, nothing is printed.
std::vector<uint8_t> buildDict(const std::vector<FileEntry>& entries, const RunOptions& opt) {
    std::vector<uint8_t> dict;
    if (opt.dictSize == 0) return dict;
    fs::path rawRoot = fs::path(opt.dataRoot) / "raw";
    const size_t nSamples = 64;
    const size_t perFile = (size_t)(opt.dictSize + nSamples - 1) / nSamples;
    std::vector<uint8_t> buf;
    for (const FileEntry& fe : entries) {
        if (dict.size() >= opt.dictSize) break;
        fs::path fp = rawRoot / fs::u8path(fe.rawRel);
        buf.clear();
        if (!readFileBytes(fp, buf)) {
            std::string srcRel = fe.srcPath;
            if (!srcRel.empty() && (srcRel[0] == '/' || srcRel[0] == '\\')) srcRel.erase(0, 1);
            if (srcRel.empty() || !readFileBytes(rawRoot / fs::u8path(srcRel), buf)) continue;
        }
        size_t take = std::min(perFile, buf.size());
        take = std::min(take, (size_t)opt.dictSize - dict.size());
        dict.insert(dict.end(), buf.begin(), buf.begin() + take);
    }
    std::printf("dict: built %zu bytes from head samples\n", dict.size());
    return dict;
}

void runEntries(const std::vector<FileEntry>& entries, const RunOptions& opt,
                SharedStats& shared, bool baselineMode) {
    fs::path rawRoot = fs::path(opt.dataRoot) / "raw";
    std::vector<uint8_t> dict;
    if (!baselineMode) dict = buildDict(entries, opt);
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> missing{0};
    std::atomic<size_t> done{0};

    int nThreads = std::max(1, opt.threads);
    auto worker = [&]() {
        LocalState ls;
        for (;;) {
            size_t idx = next.fetch_add(1);
            if (idx >= entries.size()) break;
            const FileEntry& fe = entries[idx];
            if (opt.trace) {
                std::fprintf(stderr, "[%zu] %s\n", idx, fe.rawRel.c_str());
                std::fflush(stderr);
            }

            if (!ls.fileBuf.empty()) ls.fileBuf.clear();
            // Some dumps store files at their source path instead of the
            // packed (hash) path; try both. cfg files are UTF-8 (paths may
            // contain CJK characters), so decode with u8path.
            fs::path fp = rawRoot / fs::u8path(fe.rawRel);
            if (!readFileBytes(fp, ls.fileBuf)) {
                std::string srcRel = fe.srcPath;
                if (!srcRel.empty() && (srcRel[0] == '/' || srcRel[0] == '\\')) srcRel.erase(0, 1);
                fp = rawRoot / fs::u8path(srcRel);
                if (srcRel.empty() || !readFileBytes(fp, ls.fileBuf)) {
                    missing.fetch_add(1);
                    continue;
                }
            }
            uint64_t fileSize = ls.fileBuf.size();
            std::string cat = srcCategory(fe.srcPath);

            if (baselineMode) {
                // cfg semantics: each entry partitions the file into regions
                // [block[i].offset, block[i+1].offset) (last one ends at EOF);
                // within a region, data is compressed in chunks of block.size
                // bytes (0 = whole region as one chunk).
                std::vector<BlockCfg> blocks = fe.blocks;
                std::sort(blocks.begin(), blocks.end(),
                          [](const BlockCfg& a, const BlockCfg& b) { return a.offset < b.offset; });
                // group per (codec,level) within this file so "files" counts stay sane
                std::map<std::string, Agg> perKey;
                for (size_t bi = 0; bi < blocks.size(); bi++) {
                    const BlockCfg& b = blocks[bi];
                    uint64_t regionEnd = (bi + 1 < blocks.size()) ? blocks[bi + 1].offset : fileSize;
                    CompType type = (CompType)b.type();
                    int level = (int)b.level();
                    Agg& agg = perKey[codecKey(type, level)];
                    if (b.offset > regionEnd || regionEnd > fileSize) { agg.failures++; continue; }
                    uint64_t chunkSize = b.size == 0 ? (regionEnd - b.offset) : b.size;
                    for (uint64_t off = b.offset; off < regionEnd; off += chunkSize) {
                        uint64_t n = std::min(chunkSize, regionEnd - off);
                        BlockTask task{getCodec(type), level, ls.fileBuf.data() + off, (size_t)n};
                        runBlock(task, opt.repeat, opt.verify, ls, agg);
                    }
                }
                for (auto& kv : perKey) {
                    kv.second.files = 1;
                    shared.merge(kv.first, cat, fe.pkg, kv.second);
                }
            } else {
                uint64_t chunkSize = opt.chunkSize == 0 ? fileSize : opt.chunkSize;
                for (const ResolvedCodec& rc : opt.matrixCodecs) {
                    Agg agg;
                    agg.files = 1;
                    if (fileSize == 0) {
                        agg.blocks++;
                    } else {
                        for (uint64_t off = 0; off < fileSize; off += chunkSize) {
                            uint64_t n = std::min(chunkSize, fileSize - off);
                            BlockTask task{rc.codec, rc.level, ls.fileBuf.data() + off, (size_t)n,
                                           dict.empty() ? nullptr : dict.data(), dict.size()};
                            runBlock(task, opt.repeat, opt.verify, ls, agg);
                        }
                    }
                    shared.merge(rc.key, cat, fe.pkg, agg);
                }
            }
            size_t d = done.fetch_add(1) + 1;
            if (d % 5000 == 0) {
                std::fprintf(stderr, "  ... %zu / %zu files\n", d, entries.size());
            }
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < nThreads; i++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    shared.result.missingFiles = missing.load();
}

double mbps(uint64_t bytes, double sec) {
    return sec > 0 ? (double)bytes / (1024.0 * 1024.0) / sec : 0.0;
}

void writeCsv(const std::string& path, const char* keyName, const std::map<std::string, Agg>& m) {
    std::ofstream out(path);
    out << keyName << ",files,blocks,raw_bytes,comp_bytes,ratio,comp_MBps,decomp_MBps,failures\n";
    for (auto& kv : m) {
        const Agg& a = kv.second;
        double ratio = a.compBytes ? (double)a.rawBytes / (double)a.compBytes : 0.0;
        char line[512];
        std::snprintf(line, sizeof(line), "%s,%llu,%llu,%llu,%llu,%.4f,%.1f,%.1f,%llu\n",
                      kv.first.c_str(),
                      (unsigned long long)a.files, (unsigned long long)a.blocks,
                      (unsigned long long)a.rawBytes, (unsigned long long)a.compBytes,
                      ratio, mbps(a.rawBytes, a.compSec), mbps(a.rawBytes, a.decompSec),
                      (unsigned long long)a.failures);
        out << line;
    }
}

void printTable(const char* title, const std::map<std::string, Agg>& m, size_t maxRows = 0) {
    std::printf("\n## %s\n", title);
    std::printf("%-24s %10s %12s %12s %8s %10s %10s %6s\n",
                "key", "files", "raw_MB", "comp_MB", "ratio", "c_MB/s", "d_MB/s", "fail");
    // order by raw bytes desc
    std::vector<std::pair<std::string, const Agg*>> rows;
    for (auto& kv : m) rows.push_back({kv.first, &kv.second});
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second->rawBytes > b.second->rawBytes; });
    if (maxRows && rows.size() > maxRows) rows.resize(maxRows);
    for (auto& r : rows) {
        const Agg& a = *r.second;
        double ratio = a.compBytes ? (double)a.rawBytes / (double)a.compBytes : 0.0;
        std::printf("%-24s %10llu %12.2f %12.2f %8.3f %10.1f %10.1f %6llu\n",
                    r.first.c_str(), (unsigned long long)a.files,
                    a.rawBytes / 1048576.0, a.compBytes / 1048576.0, ratio,
                    mbps(a.rawBytes, a.compSec), mbps(a.rawBytes, a.decompSec),
                    (unsigned long long)a.failures);
    }
}

} // namespace

RunResult runBaseline(const RunOptions& opt) {
    SharedStats shared;
    std::vector<FileEntry> entries = loadEntries(opt, shared.result);
    std::printf("baseline: %zu file entries (badLines=%llu)\n",
                entries.size(), (unsigned long long)shared.result.badLines);
    runEntries(entries, opt, shared, /*baselineMode=*/true);
    return std::move(shared.result);
}

RunResult runMatrix(const RunOptions& opt) {
    SharedStats shared;
    std::vector<FileEntry> entries = loadEntries(opt, shared.result);
    std::printf("matrix: %zu file entries x %zu codecs (badLines=%llu)\n",
                entries.size(), opt.matrixCodecs.size(),
                (unsigned long long)shared.result.badLines);
    runEntries(entries, opt, shared, /*baselineMode=*/false);
    return std::move(shared.result);
}

void writeReports(const RunResult& r, const RunOptions& opt, const std::string& mode) {
    fs::create_directories(opt.outDir);
    writeCsv(opt.outDir + "/" + mode + "_by_codec.csv", "codec", r.byCodec);
    writeCsv(opt.outDir + "/" + mode + "_by_category.csv", "category", r.byCategory);
    writeCsv(opt.outDir + "/" + mode + "_by_pkg.csv", "pkg", r.byPkg);

    printTable((mode + " by codec").c_str(), r.byCodec);
    printTable((mode + " by category (top 25)").c_str(), r.byCategory, 25);

    const Agg& a = r.total;
    double ratio = a.compBytes ? (double)a.rawBytes / (double)a.compBytes : 0.0;
    std::printf("\n## total\nraw=%.2f MB comp=%.2f MB ratio=%.3f comp=%.1f MB/s decomp=%.1f MB/s "
                "files=%llu blocks=%llu failures=%llu missing=%llu\n",
                a.rawBytes / 1048576.0, a.compBytes / 1048576.0, ratio,
                mbps(a.rawBytes, a.compSec), mbps(a.rawBytes, a.decompSec),
                (unsigned long long)a.files, (unsigned long long)a.blocks,
                (unsigned long long)a.failures, (unsigned long long)r.missingFiles);
}

} // namespace gdc
