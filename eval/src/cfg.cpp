#include "cfg.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace gdc {

static bool splitFields(const std::string& line, char sep, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (true) {
        size_t p = line.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(line.substr(start));
            return true;
        }
        out.push_back(line.substr(start, p - start));
        start = p + 1;
    }
}

static bool parseU64(const std::string& s, uint64_t& v) {
    auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

static bool parseBlocks(const std::string& field, std::vector<BlockCfg>& out) {
    out.clear();
    std::vector<std::string> blocks, nums;
    splitFields(field, ';', blocks);
    for (auto& b : blocks) {
        if (b.empty()) continue;
        splitFields(b, ',', nums);
        if (nums.size() != 3) return false;
        uint64_t a = 0, c = 0, comp = 0;
        if (!parseU64(nums[0], a) || !parseU64(nums[1], comp) || !parseU64(nums[2], c)) return false;
        BlockCfg blk;
        blk.offset = a;
        blk.comp = static_cast<uint32_t>(comp);
        blk.size = c;
        out.push_back(blk);
    }
    return !out.empty();
}

std::vector<FileEntry> parseCfgDir(const std::string& cfgDir, uint64_t& badLines) {
    std::vector<FileEntry> entries;
    badLines = 0;
    std::vector<fs::path> cfgFiles;
    for (auto& e : fs::directory_iterator(cfgDir)) {
        if (e.is_regular_file() && e.path().extension() == ".txt") cfgFiles.push_back(e.path());
    }
    std::sort(cfgFiles.begin(), cfgFiles.end());

    std::vector<std::string> fields;
    for (auto& cf : cfgFiles) {
        std::string pkg = cf.stem().string();
        // strip trailing "_cfg"
        if (pkg.size() > 4 && pkg.compare(pkg.size() - 4, 4, "_cfg") == 0) pkg.resize(pkg.size() - 4);
        std::ifstream in(cf);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty()) continue;
            splitFields(line, '|', fields);
            if (fields.size() != 6) { badLines++; continue; }
            FileEntry fe;
            fe.pkg = pkg;
            fe.rawRel = fields[3];
            fe.srcPath = fields[4];
            if (!fe.rawRel.empty() && (fe.rawRel[0] == '/' || fe.rawRel[0] == '\\')) fe.rawRel.erase(0, 1);
            if (!parseBlocks(fields[5], fe.blocks)) { badLines++; continue; }
            entries.push_back(std::move(fe));
        }
    }
    return entries;
}

std::string srcCategory(const std::string& srcPath) {
    std::string s = srcPath;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    size_t slash = s.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? s : s.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "(noext)";
    return name.substr(dot + 1);
}

} // namespace gdc
