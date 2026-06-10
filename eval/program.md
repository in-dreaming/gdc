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

- 后端：`lz4 lz4hc lz3 lz3huf zstd kraken leviathan mermaid selkie none`（对照组），`gdc0`（贪心LZ占位）、**`gdc1`（自研后端，主迭代对象）**。
- `gdc1` 架构（`src/gdc/gdc1.cpp`，M3 状态）：LZ77 hash-chain + 激进单步 lazy + rep-offset（64KB 窗口）；**level 9 = 两遍价格模型 optimal parse**（贪心初遍 → 流直方图定价 → 前向 DP，超长匹配贪心跳过，>256KB 文件链深降 128）→ 序列拆 **5 流**（tokens / lenExts / literals / offLo / offHi）→ 每流择优 {raw, rANS-o0 内嵌表, rANS-o0 **静态预置表**（离线训练 5 张表/流类型，`static_tables.h`，0x8000|id 标签自动分发）, rANS-o1(仅 lits，3% 门槛)}。rANS：12-bit、16-bit renorm、4 路交织、RLE 频率表（`src/gdc/rans.cpp`）。解码：打包查表（静态表缓存免建表）+ match wild-copy + 短 lit 16B fast-path。**前缀字典**：`compressDict/decompressDict`（dict 尾部 ≤64KB 作窗口前缀，贪心解析）。
- 转换层（`src/gdc/transforms.cpp`）：`plane2/4/8/16`、`delta8`、**`autoplane`**（64KB 分段每段试压选优，支持变长输出 + 自描述 tag）；管线语法 `autoplane+gdc1:9` 任意组合 transform 链 + 后端（PipelineCodec 每个 transform 存 u32 中间尺寸）。
- 字典实验：matrix 加 `--dict N`（从前 64 个 entry 头部采样拼接 N 字节共享字典，gdc1/zstd 走 dict 路径，其余 codec 不受影响）。
- 自测：`gdctest`（`tests/rans_o1_test.cpp`）= rANS o0/o1 + gdc1 全级别 + dict 模式 fuzz roundtrip；带文件参数可对单个真实文件做诊断（只输出统计，不打印内容）；`train <root> <n>` 模式重新生成静态频率表。**改熵编码/码流格式后必跑。**
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

### 1.4 已消化的实验结论（务必先读，避免重复试错）

详细日志见 `results.tsv`。截至 M3 里程碑（19 个实验，16 keep / 2 revert / 1 bugfix）：

**有效（已合入 gdc1/transform 层）**

- B8 频率表 RLE 序列化（512B→~60-250B）+ 熵编码门槛 1024→256：16KB 页 +3.2%。
- B3 解码 match wild-copy（8B 块拷贝，dist≥8）：解压 +8%。
- B1 rANS 解码三连升级：2 路交织+打包查表(+57%) → 16-bit renorm 单分支(+0~20%) → 4 路交织(+16%)。解压 207→400+ MB/s。
- B4 rep-offset（offset=0 表示重复上一个）：比率 +0.1%~，offsets 流变得高度偏斜利于 rANS。
- B7 offsets 拆 lo/hi 两条独立熵流：ress +1.0%；lenExt 独立成第 5 条流：再 +0.2~0.4%。
- P1 激进 lazy（len2>len 即换）：+0.3~0.6%。
- B2 literals order-1 rANS（16 上下文=前字节高 nibble，**流级 3% 收益门槛**）：asset +0.9%，无收益处自动回退 o0 保速度。
- T1/B9 `autoplane` 变换（64KB 分段，每段 zstd-1 试压 {copy,p2,p4,p8,p16,d8,p4+d,p8+d} 选优，6% 门槛，1 字节 tag/段）：**asset +9~10%**（序列化资产内含数值数组段），anim/ress 中性。transform 对 zstd 同样有效（backend 无关）。
- B8b 静态预置频率表（gdctest train 离线训练 5 张表/流类型，编码端与内嵌表逐流择优，0x8000|id 标签）+ 熵门槛 256→64：16KB 页 +0.8%、prefab +2.7%、bytes +3.4%；**解压 +45%**（静态表解码端缓存 slot table，免每页建表）。大文件中性（内嵌表已摊销）。
- B6 价格模型 optimal parse（仅 level 9）：两遍法（贪心 → 流直方图 -log2(p) 定价 → 前向 DP，rep 沿路径近似跟踪）。ress +1.0%、page16k +0.5%、asset +0.4%、prefab +0.35%；autoplane+gdc1:9 asset 达 1.778 ≈ kraken@7 原文件水平。**平价 DP（无价格模型）≈ lazy，白做**——价格模型质量是 optimal parsing 的全部意义。压缩降至 2-5 MB/s（存档档位专用；level 8 保持 lazy 35 MB/s）。坑：长重复区 DP 逐位置 find() 是 O(n·len)，须 ≥1024 匹配贪心跳过。
- D1 解码 lit fast-path（litLen<15 → 固定 16B wild copy，尾部 64B 边界余量）：ress 解压 +17%（483 MB/s）、prefab +10%（770）、page16k +6%。
- T6 共享前缀字典（64KB，头部采样拼接）：**bytes 2.28→5.33 (+134%)**、prefab +3%；纹理 16KB 页仅 +0.4%（字典内容不对口，需按类型训练）。zstd+dict 6.06 仍领先（其字典含熵表预热）——gdc1 字典价值后续可叠加静态表按类型化。

**无效/教训**

- 全文件 plane16（首轮）与 autoplane-on-ress：`.ress` 纹理 payload 并非裸块流，平面拆分中性甚至负收益。感知压缩必须"先识别布局、再变换"。
- B5 大窗口(2MB)+3B offsets：offset 成本 > 窗口收益（ress -1%），压缩速度崩(2MB/s)。64KB 窗口 + 2B offset 是当前数据的甜点。**回退**。
- o1 literals 无门槛全开：ress 解压 396→184 MB/s 换 +0.5% 比率，Pareto 负。上下文链使 4 路交织失去 ILP——o1 只该用在收益大的流上。

**正确性事故（必读）**

- rANS `f == SCALE(4096)`（单符号上下文）时 `((L>>12)<<16)*f` 溢出 u32 → 编解码失步。fuzz 没抓到（需要"某 ctx 下 100% 单一符号"的真实数据）。修复：u64 xMax。**任何熵编码改动后必须跑 `gdctest`（fuzz）+ 大子集 roundtrip**。

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
- ~~T6 共享字典~~ **已完成 v0**（`--dict N` 头部采样拼接字典；bytes +134%）。剩余空间：按类型/包训练专用字典、真正的后缀自动机式字典构造（zstd 源码无 dictBuilder，可自研简化版 cover 算法）、dict + 静态熵表联动、optimal parse 的 dict 支持。
- **T7 对象分组与页策略**（直接回应业务痛点）：研究 prefab 多 object 拆/合的读放大模型——用 `--chunk` 扫 4K/16K/64K/256K 画 ratio-vs-页大小曲线，结合"对象平均大小"给出推荐粒度；进一步试"按 object 边界对齐页"（需要 T4 的解析）。产出应是策略表：什么类型 × 什么大小 → 什么粒度/分组。

### 3.2 自研后端 gdc1 → gdc2（速度与比率双修）

- ~~B1 rANS 多路交织~~ **已完成**（4 路 + 16-bit renorm + 打包查表，解压 207→400+）。剩余空间：SIMD（SSE/AVX2 4-8 路向量化）、双 token 解码。
- ~~B2 literals o1~~ **已完成**（16 ctx + 3% 门槛）。剩余空间：ctx 设计（pos&stride / 全前字节 256 ctx + 表共享）、tokens 流的 o1。
- ~~B3 wild-copy~~ **已完成**（8B 块；dist<8 仍逐字节，可加 pattern 展开）。
- ~~B4 rep-offset~~ **已完成**（offset=0 编码）。剩余空间：rep 参与 lazy 决策、2-3 字节短匹配 + rep（kraken 风格）。
- ~~B5 更大窗口~~ **已试，负收益回退**（offset 成本 > 窗口收益；64KB+2B 是甜点）。若重试需配合 offset varint/熵建模。
- ~~B6 optimal parsing~~ **已完成**（两遍价格模型 DP，全子集 +0.4~1.0%）。剩余空间：多候选匹配（每位置保留 top-K (len,dist) 而非仅最优）、DP 支持 dict 前缀、价格迭代（DP 结果再喂价格再 DP）。
- ~~B7 流细分~~ **已完成**（5 流：tokens/exts/lits/offLo/offHi）。
- ~~B8 freq 表 RLE~~ **已完成**。~~B8b 静态预置表~~ **已完成**（全局训练 5 张表）。剩余空间：**按数据类型分组训练**（纹理/序列化/表格各一套表，压缩端多试几张选优，解码端按 id 分发——现有 0x8000|id 机制直接支持扩表）。
- ~~B9 块级自动选择~~ **autoplane 已实现**（变换级）。剩余空间：把 backend 候选也纳入（每段选 lz4-style vs gdc1 vs raw）。

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
| 2026-06-10 | M1: gdc1 v1 + plane/delta transforms | 1.382 (gdc1@9), 207 MB/s | +1.5% | -7.9% | plane16 对 .ress 整文件负收益（容器未分段） |
| 2026-06-10 | M2: 13 实验迭代（B8/B3/B1/B4/B7/P1/B2/autoplane） | 1.409 (gdc1@9), 413 MB/s | +3.5% | -6.1% | 解压速度翻倍；asset 上 autoplane+gdc1 1.759 超 zstd 8.9% |
| 2026-06-11 | M3: B8b 静态表 + B6 optimal parse + D1 fast-path + T6 字典 | **1.4245** (gdc1@9), 483 MB/s | +2.2%* | -7.1%* | *zstd/kraken 未重测，对比 M2 参考值；bytes+dict 2.28→5.33 |

M2 各类数据全景（`--limit 2000` × `--repeat 3`，ratio / decomp MB/s）：

| 子集 | gdc1@9 | autoplane+gdc1@9 | zstd@9 | kraken@7* | gdc 相对 zstd |
|:--|:--|:--|:--|:--|--:|
| ress2k | **1.441** / 315 | 1.440 / 312 | 1.394 / 785 | 1.534 / 982 | **+3.4%** |
| asset2k | 1.611 / 369 | **1.759** / 399 | 1.615 / 949 | 1.958 / 807 | **+8.9%** |
| prefab2k | 3.034 / 747 | 3.031 / 777 | 3.138 / 1105 | 3.357 / 1742 | -3.3% |
| bytes2k | 2.202 / 1660 | 2.191 / 1668 | 2.462 / 465 | 2.363 / 1084 | -10.6%（需 T6 字典） |
| anim2k | 1.286 / 651 | 1.286 / 638 | 1.294 / 3464 | 1.319 / 2805 | -0.6% |

*kraken 为无汇编构建，速度低估。

16KB 页粒度（ress600 + `--chunk 16384`，对齐业务现状）：

| codec | ratio | decomp MB/s | 备注 |
|:--|--:|--:|:--|
| leviathan@5（基准策略） | 1.432 | 676* | *无汇编构建，速度低估 |
| kraken@7 | 1.417 | 1028* | |
| zstd@9 | 1.353 | 655 | |
| **gdc1@9 (M3)** | **1.3465** | 489 | M2 时 1.329/343 → B8b+B6+D1；与 zstd 差 -0.5%，与 leviathan 差 -6.0% |
| lz4hc@9 | 1.274 | 4168 | 速度档对照 |

M3 其它要点（详见 results.tsv）：ress1500 整文件 1.4245/483；asset autoplane+gdc1@9 **1.7784** ≈ kraken@7（1.78 量级，原文件粒度）；prefab 3.1261/770；bytes+64KB 字典 5.33/1018（zstd+dict 6.06）。

下一步高优先方向：
1. **静态表/字典按类型化**：page16k 残余差距与 bytes 落后 zstd+dict 的共同根因——一套全局表/一个头部字典不够对口。扩展 0x8000|id 静态表至按类型多套 + 按类型训练字典。
2. **B6 延展**：DP 支持 dict 前缀（目前 dict 走贪心）、top-K 候选匹配。
3. **解压速度**：距 zstd（655-950）仍差 ~30%。方向：双 token 解码、offset 流预解码消除分支、rANS SIMD。
4. **T1 .ress 布局感知**（一直未做，潜在大头）：识别纹理 payload 边界，分段 transform 而非整文件。

---

## 5. 文件地图

```
eval/
  program.md            ← 本文件（迭代时维护 §4）
  results.tsv           ← 实验日志
  src/gdc/              ← 主迭代区
    gdc1.{h,cpp}          自研后端（LZ + 5流 + rANS + optimal parse + dict）
    rans.{h,cpp}          rANS（o0 / o1 / 静态表）
    static_tables.h       离线训练的静态频率表（gdctest train 再生成）
    transforms.{h,cpp}    感知压缩 transform 注册表
  src/codec.cpp         ← codec/transform spec 解析（"plane16+gdc1:7"）
  src/bench.{h,cpp}     ← 评估 harness（baseline 路径勿动；matrix 支持 --chunk/--dict）
docs/research/方案研究-自研游戏数据压缩.md   ← 总体方案（档位 A/B/C/D 定义）
```
