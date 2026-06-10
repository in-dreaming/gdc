#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gdc {

// One block inside a packed file.
// comp encodes: type = comp & 0xff, level = (comp >> 16) & 0xff
struct BlockCfg {
    uint64_t offset = 0;
    uint32_t comp = 0;
    uint64_t size = 0; // 0 => until end of file

    uint32_t type() const { return comp & 0xffu; }
    uint32_t level() const { return (comp >> 16) & 0xffu; }
};

struct FileEntry {
    std::string pkg;     // cfg file stem, e.g. "1010010070"
    std::string rawRel;  // path relative to <data>/raw (leading '/' stripped)
    std::string srcPath; // original source asset path (used for type categorization)
    std::vector<BlockCfg> blocks;
};

// Parse all *_cfg.txt under cfgDir. Returns number of malformed lines via badLines.
std::vector<FileEntry> parseCfgDir(const std::string& cfgDir, uint64_t& badLines);

// Lower-cased extension-ish category of a source path.
// ".png.ress" -> "ress", "a.prefab" -> "prefab", no extension -> "(noext)"
std::string srcCategory(const std::string& srcPath);

} // namespace gdc
