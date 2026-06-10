#pragma once

#include <cstddef>
#include <cstdint>

namespace gdc {
namespace gdc1 {

// 自研后端 v1 ("mini-Kraken" 架构):
//   LZ77 (hash-chain 匹配 + lazy parsing, 64KB 窗口)
//   → 序列拆分为 3 个独立流 (tokens+长度扩展 / literals / offsets)
//   → 每个流独立选择 rANS 熵编码或 raw 存储 (按实际大小择优)
//
// level 0..9: 控制 chain 搜索深度、lazy 开关与熵编码开关。
// 码流确定性: 相同输入+level 输出比特一致 (无浮点、无地址依赖)。

size_t compressBound(size_t srcSize);
size_t compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap, int level);
size_t decompress(const uint8_t* src, size_t compSize, uint8_t* dst, size_t rawSize);

} // namespace gdc1
} // namespace gdc
