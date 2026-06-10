# gdceval — 游戏数据压缩评估工程

对 `tmp_ref/inner_data` 数据集做压缩基准评估。

> 数据安全：本工具只在本机读取 `raw/` 内文件字节做压缩/解压与统计，
> 不输出、不落盘、不上传任何文件内容；报告中仅含路径、尺寸与耗时统计。

## 构建

依赖：VS2022 (MSVC)、CMake ≥ 3.20。压缩库源码直接引用 `tmp_ref/compression`（zstd、lz4、lz3、oodle2）。

```powershell
cd eval
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# 产物: build/Release/gdceval.exe
```

注意：oodle2 以无平台汇编内核方式编译（C++ fallback），**速度数字低估正式版 Oodle**，压缩率不受影响。

## 用法

```powershell
# 基准：按数据集自身 cfg 策略（每个 block 的 type+level）压缩并统计
.\build\Release\gdceval.exe baseline --data ..\tmp_ref\inner_data --out out

# 对比矩阵：所有文件分别用指定后端整文件压缩
.\build\Release\gdceval.exe matrix --data ..\tmp_ref\inner_data `
    --codecs lz4,lz4hc:5,zstd:3,zstd:9,kraken:5,leviathan:5,mermaid:5,selkie:5,lz3:5,lz3huf:5,gdc0 `
    --out out

# 快速冒烟
.\build\Release\gdceval.exe baseline --data ..\tmp_ref\inner_data --limit 2000

# 计时更稳（best-of-3），单线程测速
.\build\Release\gdceval.exe matrix --data ..\tmp_ref\inner_data --codecs zstd:5 --threads 1 --repeat 3
```

输出：stdout 摘要表 + `out/<mode>_by_codec.csv`、`out/<mode>_by_category.csv`、`out/<mode>_by_pkg.csv`。

## cfg 解析约定

`raw_cfg/*_cfg.txt` 每行：`hash|-|-|raw相对路径|源文件路径|blocks`；
blocks 为 `;` 分隔的 `offset,comp,size`，`comp = type | (level<<16)`。

实测语义：多个 block 按 offset 把文件划分为若干 region（最后一个到文件尾）；
`size` 是该 region 内的**分块压缩粒度**（如 16384 = 16KB 随机访问页），`size==0` 表示整个 region 一次压缩。
另外部分文件落盘在源文件路径而非 raw 相对路径，工具会两者都尝试。

type 枚举（QTSF）：0 none / 1 lz4 / 2 lz4hc / 3 kraken / 4 leviathan / 5 mermaid / 6 selkie / 7 lz3 / 8 lz3huf / 9 zstd。

## level 映射假设（如与运行时不符需修正）

| 后端 | cfg level 0–9 映射 |
|:--|:--|
| lz4 | 忽略（acceleration=1） |
| lz4hc | `clamp(level+4, 3, 12)`（默认5→HC 9 = LZ4HC 默认） |
| oodle 系 | 直接映射 `OodleLZ_CompressionLevel`（0=None … 9=Optimal5），与枚举一一对应 |
| lz3/lz3huf | 直接映射 `LZ3_CLevel`（clamp 1–9）；>64KB 输入按 0xFF80 分块、每块 4B 长度框架 |
| zstd | 直接作为 zstd level（0→默认3） |

## 自研方案（感知压缩 + 自研后端）

- **transform 层**（`src/gdc/transforms.cpp`）：`plane2/4/8/16` 字节平面拆分、`delta8` 字节差分；
  与任意后端组成管线：`--codecs plane16+zstd:9,plane16+delta8+gdc1:5`。
- **自研后端**：
  - `gdc0`：贪心 LZ77 占位实现（`src/codec.cpp`）。
  - `gdc1`：v1 正式架构（`src/gdc/gdc1.cpp`）——LZ77 hash-chain + lazy 解析，
    序列拆 tokens/literals/offsets 三流，每流独立 rANS（`src/gdc/rans.cpp`）或 raw 择优；
    level 0–9 控制搜索深度/lazy/熵编码。
- **实验过滤**：`--cat ress`（按源文件类型）、`--pkg`（按包）、`--chunk 16384`（页粒度随机访问模拟）。

持续迭代的技术方案与调优方向见 `program.md`（autoresearch 实验程序）。
