#pragma once

#include <cstddef>
#include <cstdint>

namespace gdc {
namespace gdc1 {

// 自研后端 ("mini-Kraken" 架构):
//   LZ77 (hash-chain 匹配 + lazy parsing + rep-offset, 64KB 窗口)
//   → 序列拆分为 5 个独立流 (tokens / lenExts / literals / offLo / offHi)
//   → 每流择优 {raw, rANS-o0(内嵌表/静态表), rANS-o1(仅 lits)}
//
// level 0..9: 控制 chain 搜索深度、lazy 开关与熵编码开关。
// 码流确定性: 相同输入+level 输出比特一致 (无浮点、无地址依赖)。

size_t compressBound(size_t srcSize);
size_t compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap, int level);
size_t decompress(const uint8_t* src, size_t compSize, uint8_t* dst, size_t rawSize);

// Prefix-dictionary variants (T6): the last <=64KB of dict acts as match
// window prefix; decompress must receive the same dict. Greedy parse only.
size_t compressDict(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCap, int level,
                    const uint8_t* dict, size_t dictSize);
size_t decompressDict(const uint8_t* src, size_t compSize, uint8_t* dst, size_t rawSize,
                      const uint8_t* dict, size_t dictSize);

// 离线训练钩子 (单线程使用): 设置后 compress() 会把 5 条流的原始字节
// 回调给 hook(streamKind 0..4, data, n), 用于聚合直方图、生成静态频率表。
void setTrainHook(void (*hook)(int streamKind, const uint8_t* data, size_t n));

} // namespace gdc1
} // namespace gdc
