

https://arxiv.org/abs/2512.11134

https://github.com/fraunhoferhhi/nncodec


如果把“数据压缩”定义为**利用神经网络学习数据分布，然后用概率模型+熵编码进行压缩**，那么目前已经形成了一条完整技术路线，尤其在图像、视频、3D资产、纹理压缩领域发展很快。

可以按技术代际来看。

---

# 第一代：AutoEncoder 压缩

最经典方案：

```
原始数据
    ↓ Encoder
隐变量 Latent
    ↓ Quantize
离散码流
    ↓ Decoder
重建数据
```

例如：

* AutoEncoder
* Denoising AutoEncoder
* Sparse AutoEncoder

核心思想：

```
x → z → x'
```

其中：

* x 原始数据
* z 压缩后的表示
* x' 重建结果

训练目标：

```
Loss = Reconstruction Error
```

例如：

```
MSE(x,x')
```

问题：

* 不知道 z 的概率分布
* 无法直接熵编码
* 压缩率不高

因此后来逐渐被淘汰。

---

# 第二代：Variational AutoEncoder (VAE)

突破点：

学习 latent 的概率分布。

结构：

```
Encoder
    ↓
μ,σ
    ↓
Sampling
    ↓
Latent
    ↓
Decoder
```

损失：

```
Loss =
Reconstruction Loss
+
KL Divergence
```

即：

L = L_{recon} + D_{KL}(q(z|x)||p(z))

优势：

* latent 更规整
* 可以估计概率
* 能做熵编码

很多现代压缩算法都来自 VAE 思想。

---

# 第三代：Learned Image Compression（LIC）

2018后成为主流。

代表人物：

Johannes Ballé

代表论文：

* End-to-End Optimized Image Compression
* Hyperprior Compression

结构：

```
Image
 ↓
Analysis Transform
 ↓
Latent y
 ↓
Hyper Encoder
 ↓
Hyper Latent z
 ↓
Entropy Model
 ↓
Arithmetic Coding
```

即：

不仅压缩图片

还压缩：

```
latent 的统计特征
```

用于预测概率。

---

流程：

```
Image
 ↓
CNN
 ↓
Latent
 ↓
Probability Model
 ↓
Arithmetic Encoder
 ↓
Bitstream
```

解码：

```
Bitstream
 ↓
Arithmetic Decoder
 ↓
Latent
 ↓
CNN Decoder
 ↓
Image
```

这已经接近 JPEG2000 的理论极限。

---

# 第四代：Hyperprior

Google 提出的重要改进。

思想：

传统压缩：

```
P(y)
```

实际：

```
P(y | z)
```

更准确。

其中：

* y = latent
* z = latent的统计特征

即：

```
图片
 ↓
latent
 ↓
hyper latent
```

两层压缩。

这样：

```
Entropy ↓
Bitrate ↓
```

通常提升：

```
10~20%
```

---

# 第五代：Context Model

类似 GPT 的预测。

压缩当前 token 前：

```
预测当前 token 出现概率
```

例如：

```
P(y_i | y_0...y_{i-1})
```

结构：

```
Masked CNN
```

后来变成：

```
Transformer
```

代表：

* Minnen 2018
* Google Compression Transformer

思想和 LLM 本质一致：

```
预测下一个符号
```

概率越准：

```
Entropy 越低
```

压缩率越高。

---

# 第六代：Transformer Compression

近几年热点。

结构：

```
Input
 ↓
Transformer Encoder
 ↓
Latent Tokens
 ↓
Entropy Coding
```

代表：

* TIC
* ViT Compression
* CompressAI Transformer

优势：

长距离相关性。

例如：

```
天空
...
天空
```

CNN 看不到。

Transformer 可以。

---

# 第七代：LLM Compression

最近两年开始出现。

本质：

```
数据压缩 = 概率建模
```

而 LLM：

```
P(token_n | token_<n)
```

恰好就是最强概率模型。

因此：

```
LLM + Arithmetic Coding
```

理论上：

```
压缩率≈交叉熵
```

例如：

* DeepMind
* Meta
* OpenAI相关研究方向

文本压缩效果已经接近理论极限。

---

# 视频压缩方向

代表：

* Deep Video Compression
* DCVC
* Neural Video Codec

结构：

```
Frame_t
 ↓
Motion Network
 ↓
Residual Network
 ↓
Entropy Coding
```

替代：

* H264
* H265
* AV1

微软的 DCVC 系列已经非常强。

---

# 3D资产压缩

游戏更相关。

## Neural Geometry Compression

压缩：

* Mesh
* Point Cloud
* 3DGS
* NeRF

代表：

* MPEG Neural Compression
* Draco + Neural
* Neural Point Cloud Compression

---

## NeRF Compression

NeRF 本身就是压缩。

原来：

```
1000张图片
≈ 数GB
```

变成：

```
MLP参数
≈ 数MB
```

实际上：

```
神经网络 = 压缩结果
```

---

# 游戏领域值得关注的方向

如果你在研究游戏引擎和资源压缩，我认为最有潜力的是：

### 1. Texture Compression + Neural

替代：

* ASTC
* BC7

例如：

```
Texture
 ↓
Neural Encoder
 ↓
Latent
 ↓
GPU Decoder
```

微软、NVIDIA、ARM 都在研究。

相关概念：

* Neural Texture Compression（NTC）
* RTX Neural Texture Compression

其中 NVIDIA 的 Neural Texture Compression 已经展示出比 BC7/ASTC 更高的压缩率和接近实时解码能力。

---

### 2. Animation Compression

压缩：

```
骨骼动画
BlendShape
肌肉参数
```

典型：

```
Pose → Latent → Pose
```

很多 AAA 已开始使用。

---

### 3. Neural Asset Compression

未来可能是：

```
Mesh
Texture
Animation
Material
```

统一压缩成：

```
Latent Asset
```

运行时解码。

类似：

```
Asset → Foundation Model → Latent
```

---

# 从数学本质看

传统压缩：

```
LZ77
Huffman
Arithmetic Coding
```

本质：

```
寻找重复模式
```

神经压缩：

```
学习数据分布
```

即：

```
Compression
=
Modeling Probability Distribution
```

对于任意数据，如果神经网络能更准确地预测下一个符号：

```
Entropy ↓
Compression Ratio ↑
```

因此从信息论角度：

```text
最先进的压缩算法
≈
最先进的生成模型
≈
最先进的概率模型
```

这也是为什么如今 LLM、Transformer、Diffusion、VAE 与压缩理论正在逐渐汇合——它们本质上都在学习数据分布。
