# autoresearch program — 自研游戏数据压缩 (GDC)

这是一个让 LLM 自主迭代压缩方案的实验程序（参考 `tmp_ref/autoresearch` 的模式）。
目标：在真实游戏数据集上，持续改进**感知压缩转换层 + 自研后端**，逼近并超越现有基准。

---

## 1. 背景与现状

### 1.1 数据集与基准（已建立，不要修改）

- 数据：`tmp_ref/inner_data`（raw/ 约 7.45GB、245,825 个文件；raw_cfg/ 为业务打包配置）。
- **数据安全红线：不允许把 `tmp_ref/inner_data/raw/` 里的文件内容打印到终端/日志/报告，更不允许上传。只允许程序在本机内存中读取并输出统计数字。**
- 基准（业务自身策略，`gdceval baseline` 全量复现）：

| 策略 | 文件数 | 原始 | 压缩后 | 压缩率 |
|:--|--:|--:|--:|--:|
| leviathan@5 (大部分 16KB 页粒度) | 197,005 | 6137 MB | 3272 MB | 1.876 |
| lz4@5 | 48,787 | 1068 MB | 790 MB | 1.351 |
| zstd@9 | 20,431 | 245 MB | 21 MB | 11.49 |
| **合计** | **245,825** | **7450 MB** | **4084 MB** | **1.824** |

- 体积大头：`ress` 分块压缩纹理 4.2GB（ratio≈1.93）、`usm` 视频 600MB（不可压）、`asset`/`prefab`/`anim` 序列化对象约 1.5GB（ratio 1.6–3.5）、`bytes` 表格 168MB（3.2）。

### 1.2 关键业务语义（影响策略设计）

- cfg 的 block 很多时候对应一个逻辑"文件/对象"：`.ress` 内可能含多个分块压缩纹理（mipmap/atlas），业务层按 block 划分；`size` 字段是 region 内的**分块压缩粒度**（16384 = 16KB 随机访问页）。
- prefab 自身可能含大量 object（如 ParticleSystem），打包时可能被拆开、也可能被组合。
- **Unity 反序列化是乱序读取 object 的**：页/块粒度太大 → 随机读一个 object 要解压大量无关数据（读放大 → CPU 上升）；粒度太小 → 压缩率差、字典上下文浪费。引擎层有多级 cache，但冗余解压依然是真实成本。
- 因此每个策略都要同时回答三个数：**压缩率、解压吞吐、读放大（页粒度）**。

### 1.3 已实现的实验设施（`eval/`，C++17 + CMake）

- 后端：`lz4 lz4hc lz3 lz3huf zstd kraken leviathan mermaid selkie none`（对照组），`gdc0`（贪心LZ占位）、**`gdc1`（自研后端 v1，主迭代对象）**。
- `gdc1` 架构（`src/gdc/gdc1.cpp`）：LZ77 hash-chain + 单步 lazy（64KB 窗口）→ 序列拆 3 流（tokens/literals/offsets）→ 每流独立 rANS（order-0, 12-bit，`src/gdc/rans.cpp`）或 raw 择优。level 0–9 控制搜索深度/lazy/熵开关。
- 转换层（`src/gdc/transforms.cpp`）：`plane2/4/8/16`（字节平面拆分）、`delta8`（字节差分）；管线语法 `plane16+delta8+gdc1:7` 任意组合 transform 链 + 后端。
- 评估命令（必须 roundtrip memcmp 全过，failures=0 才算合法实验）：

```powershell
cd eval
cmake -B build -G "Visual Studio 17 2022" -A x64   # 一次性
cmake --build build --config Release -- /m

# 快速子集 A/B（按类型/包过滤，secs 级反馈）
.\build\Release\gdceval.exe matrix --data ..\tmp_ref\inner_data `
    --cat ress --limit 1500 --threads 12 --codecs zstd:9,gdc1:9,plane16+zstd:9

# 页粒度/读放大实验（16KB 页对齐业务现状）
.\build\Release\gdceval.exe matrix --data ..\tmp_ref\inner_data `
    --cat ress --limit 1500 --chunk 16384 --codecs gdc1:5,zstd:9

# 全量基准复现（里程碑时跑）
.\build\Release\gdceval.exe baseline --data ..\tmp_ref\inner_data --threads 24
```

### 1.4 首轮 A/B 结果（务必先消化，避免重复试错）

ress 子集（1500 文件 / 418MB，整文件压缩）：

| codec | ratio | comp MB/s | decomp MB/s | 结论 |
|:--|--:|--:|--:|:--|
| kraken@7 | 1.500 | 5.1 | 934 | 对照上限（无汇编构建，解压速度被低估） |
| zstd@9 | 1.362 | 32 | 827 | 通用对照 |
| **gdc1@9** | **1.382** | 25 | 207 | **比率已超 zstd@9**，解压速度差 4x（主要瓶颈：单状态 rANS + 逐字节匹配拷贝） |
| plane16+zstd@9 | 1.314 | 37 | 597 | **负收益！** |
| plane8+zstd@9 | 1.267 | 38 | 621 | 更差 |
| plane16+delta8+zstd@9 | 1.275 | 36 | 558 | 更差 |

**重要教训**：`.ress` 不是裸 ASTC 块流——它是 Unity 序列化容器（可能多张纹理 + 多 mip 级 + 对齐填充），盲目按 16B stride 做平面拆分破坏了 LZ 可见的重复结构。感知压缩必须"先识别布局、再变换"，而不是全文件套公式（这正是方案文档里"块感知"的本意）。

---

## 2. 实验循环

LOOP FOREVER（自主运行，不要停下来问人）：

1. 提出一个假设（从 §3 调优方向挑，或自创）。
2. 修改代码：可改 `eval/src/gdc/*`（transforms、rans、gdc1、新增文件）与 codec 注册/spec 解析；**不可改** 基准语义（`bench.cpp` 的 baseline 路径、cfg 解析）、对照后端（lz4/lz3/zstd/oodle wrapper）、验证逻辑（roundtrip memcmp）。
3. 编译；跑相应子集实验（选对 `--cat`/`--limit`/`--chunk`，控制在分钟级）。
4. 记录到 `eval/results.tsv`（tab 分隔；不提交 git）：

```
commit	experiment	dataset	ratio	comp_MBps	decomp_MBps	fail	status	description
a1b2c3d	gdc1:9	ress1500	1.382	25.0	206.8	0	keep	baseline gdc1 v1
```

5. `git commit`（只提交代码）。有效（比率↑且解压速度不恶化超阈，或速度↑且比率不降）→ 保留；无效 → `git reset` 回退。
6. **任何 failures>0 的实验一律视为 crash，必须修复或回退**——码流正确性是硬约束。
7. 里程碑（每 10–20 个实验）：跑一次更大子集或全量，对照 §1.1 基准表与 kraken 对照组，更新本文件 §4 进展表。

### 评价标准（按优先级）

1. **正确性**：roundtrip 全过，确定性（同输入同输出）。
2. **主指标 Pareto**：(ratio, decomp_MBps) 双目标。参考线：
   - 档位 A 候选：ratio ≥ kraken@7 × 0.97 且 decomp ≥ 800MB/s（本机、单线程）；
   - 档位 D 候选：decomp ≥ 2GB/s 且 ratio ≥ lz4 × 1.1；
   - 16KB 页（`--chunk 16384`）下相对基准 leviathan@5 的 ratio 差距是"能否替换业务现状"的关键数。
3. **简单性**：同等收益取简单实现；负收益的复杂度立即删除。
4. 压缩速度是软约束（离线档位可慢，但别慢到无法实验迭代）。

### 数据子集约定（保证实验可比）

| 名称 | 命令参数 | 用途 |
|:--|:--|:--|
| ress1500 | `--cat ress --limit 1500` (418MB) | 纹理负载主战场 |
| asset2k | `--cat asset --limit 2000` | 序列化对象 |
| prefab2k | `--cat prefab --limit 2000` | 含多 object 的 prefab |
| bytes2k | `--cat bytes --limit 2000` | 表格/配置 |
| anim2k | `--cat anim --limit 2000` | 动画 |
| page16k | 任意子集 + `--chunk 16384` | 随机访问页粒度 |
| full | baseline 全量 | 里程碑 |

---

## 3. 技术方案全集与调优方向（弹药库）

按"预期收益 × 实现难度"排序，每条都注明现状。来源：`docs/research/方案研究-自研游戏数据压缩.md` + 首轮实验教训。

### 3.1 感知压缩（transform 层）——收益大头

- **T1 `.ress` 布局感知拆分（最高优先）**：解析 .ress 的内部结构（多纹理/多 mip 的拼接边界；可从同名序列化文件读 image data offset/size，或用启发式探测 ASTC/ETC 块边界与对齐填充），对每段**纹理 payload 单独**做 stride 拆分，头部/填充直通。首轮全文件 plane16 失败的根因就是没分段。
- **T2 ASTC 端点/权重位域分离**：ASTC 128-bit 块内是变长位域（分区、CEM、端点、权重）。比 byte-plane 更进一步：按 block mode 分组后做位域级重排（参考 BC7Prep 思路）。需要解析 ASTC 块头。业界空白，方案文档预期纹理 +10~25%。
- **T3 ETC2/BC 块的 8B stride 与子通道拆分**：移动端纹理可能混 ETC2(8B)。识别格式后选择 stride，并试"颜色端点字节 vs 调制位字节"分流。
- **T4 序列化对象列化**：prefab/asset 是 Unity TypeTree 序列化流，同类型 object 的字段交错出现。试：同类 object 分组 + 字段列化（float 流、int 流、引用流分开）+ delta/varint。需要轻量解析序列化头（version、TypeTree 可从数据特征猜或借 UnityFS 文档）。预期 +30~50%（小文件场景配合字典）。
- **T5 float 流 byte-grouping（ZipNN 思路）**：识别 float32/float16 数组区段（动画曲线、mesh），按字节平面拆 + 符号/指数分组。anim/fbx 类预期 +15~30%。
- **T6 共享字典**：zstd dictionary API 已在源码里，按类型训练字典（`ZDICT_trainFromBuffer`），对 <16KB 的小 object 文件收益 30–50%。需要给 eval 加字典训练/加载路径。
- **T7 对象分组与页策略**（直接回应业务痛点）：研究 prefab 多 object 拆/合的读放大模型——用 `--chunk` 扫 4K/16K/64K/256K 画 ratio-vs-页大小曲线，结合"对象平均大小"给出推荐粒度；进一步试"按 object 边界对齐页"（需要 T4 的解析）。产出应是策略表：什么类型 × 什么大小 → 什么粒度/分组。

### 3.2 自研后端 gdc1 → gdc2（速度与比率双修）

- **B1 rANS 多路交织**：当前单状态、逐字节 renorm，解压 ~200MB/s。改 2/4 路交织状态 + 32-bit renorm（一次读 2 字节）可到 1GB/s 量级。参考 ryg 4-way SIMD 思路（compress2 报告有 NEON 版设计）。
- **B2 literals 的 o1/拆分上下文**：literals 按 (pos&15) 或前字节高位分桶，多张 rANS 表；对纹理类数据等效于隐式 plane 拆分（可能比显式 transform 干净）。
- **B3 匹配拷贝 wild-copy**：解码 match 改 16B 块拷贝（带 overlap 特判），literals memcpy 已有；预期解压 +30~50%。
- **B4 rep-offset**：上一个 offset 复用（zstd/kraken 都有），token 腾 1 个 code 表示 rep；结构化数据收益明显。
- **B5 更大窗口/2 级哈希**：64KB→1MB 窗口（offset 改 varint 或 3B），配合长距匹配表；大文件（db/usm 之外的 fbx、asset 批量）收益。注意运行时内存预算。
- **B6 optimal parsing**：level 9 改 price-based 最优解析（compress2 报告有设计）；比率 +3~8%，仅离线档位。
- **B7 offsets/length 流的专用编码**：offset 高低字节分流、length 直方图单独建模——目前 3 流已经分开，可再细分实验。
- **B8 freq 表开销**：512B/流/块，16KB 页下显著。试：压缩 freq 表（增量/游程）、或小块退化为静态预置表（按数据类型离线训练几张表内置）。
- **B9 块级自动选择**：每块试 {raw, lz-only, lz+rans} 取最小（已部分实现）；扩展为每块自动选 transform（带 1 字节 tag），即"mini-OpenZL 图"。

### 3.3 容器化（GDC-Pack v0，迭代后期）

- C1 统一容器：magic/version、块表（offset/rawSize/compSize/codec tag/transform tag/字典 id）、页索引（随机访问）、xxhash 校验。
- C2 确定性 & patch 友好：固定块边界或 CDC，diff 实验（两版本数据集模拟热更）。
- C3 解码器统一入口：tag 驱动 transform 逆变换 + 后端解码（已有雏形 = PipelineCodec）。

### 3.4 禁区/注意

- 不做运行时神经推理路径（方案文档结论：3 年内不适合通用码流）；学习方法只允许离线（字典训练、参数搜索）。
- rANS 本体无专利问题，但不要照抄 Oodle 反汇编实现细节。
- oodle 对照组是无汇编构建：**比率可信、速度低估**，对照速度时心里乘个系数（官方 Kraken 解压约 1.5–2GB/s/核）。
- 大改 `bench.cpp` 公共路径前三思——它同时支撑基准复现。

---

## 4. 进展记录（每个里程碑更新）

| 日期 | 里程碑 | ress1500 ratio (gdc 最优) | vs zstd@9 | vs kraken@7 | 备注 |
|:--|:--|--:|--:|--:|:--|
| 2026-06-10 | gdc1 v1 + plane/delta transforms | 1.382 (gdc1@9) | +1.5% | -7.9% | plane16 对 .ress 整文件负收益（容器未分段） |

16KB 页粒度（ress600 + `--chunk 16384`，对齐业务现状）：

| codec | ratio | decomp MB/s | 备注 |
|:--|--:|--:|:--|
| leviathan@5（基准策略） | 1.432 | 477* | *无汇编构建，速度低估 |
| zstd@9 | 1.353 | 592 | |
| gdc1@9 | 1.279 | 632 | freq 表 512B×3/页 开销在 16KB 页下吃掉 ~5% → **B8 是高优先项** |

---

## 5. 文件地图

```
eval/
  program.md            ← 本文件（迭代时维护 §4）
  results.tsv           ← 实验日志（不提交）
  src/gdc/              ← 主迭代区
    gdc1.{h,cpp}          自研后端 v1（LZ + 3流 + rANS）
    rans.{h,cpp}          order-0 rANS
    transforms.{h,cpp}    感知压缩 transform 注册表
  src/codec.cpp         ← codec/transform spec 解析（"plane16+gdc1:7"）
  src/bench.{h,cpp}     ← 评估 harness（baseline 路径勿动）
docs/research/方案研究-自研游戏数据压缩.md   ← 总体方案（档位 A/B/C/D 定义）
```
