# zerocopy —— V4L2 零拷贝:EXPBUF → dmabuf → RGA → NPU

把相机帧从"采集 → memcpy → CPU 缩放 → 喂 NPU"改成"**VB2 缓冲直接导出 dmabuf → RGA 硬件缩放 → NPU 直接消费**",并用可复现的数字量化收益。

载体:**IMX415 的 NV12 流**(`/dev/video5`,rkisp mainpath,3840×2160);消费端:yolov8n RKNN(输入 640×640 int8)。

## 工具

| 文件 | 作用 |
|---|---|
| `zc_s1.c` | **S1** 抓一帧 NV12 + `VIDIOC_EXPBUF` 导出 dmabuf,校验 fd 与 size |
| `zc_s23b.cpp` | **S2/S3** dmabuf 与 vb2 mmap 全帧比对;RGA 四条输入路径(fd/va)× CPU 参考,比 PSNR |
| `npu_info.cpp` | 打印 RKNN 模型的输入/输出张量属性(量化参数、layout) |
| `zc_npu.cpp` | **S4** 三种数据通路 + yolov8n 推理:`a`=拷贝基线 / `b`=RGA / `c`=全零拷贝 / `f`,`g`=文件输入验证 |
| `zc_pipe.cpp` | **流水线版**:采集/前处理/推理三线程重叠,3 槽位各一套 rknn context |
| `ystat.c` | 抓帧统计 Y 平面亮度(mean/p50/p99/max),用于扫曝光增益 |

板载编译(板上有 gcc 9.4 / g++):

```bash
make            # 六个目标全编
./zc_s1 /dev/video5
./zc_s23b /dev/video5
./zc_npu c 30 /dev/video5     # 零拷贝 + 推理 30 帧
./zc_pipe 60 /dev/video5 1    # 流水线,每帧推理 1 次
```

## 关键结果

### S1/S2:导出的 dmabuf 就是相机那块内存

```
export fd  : 4
exp_name   : videobuf2_dma_sg          ← 真 dma-buf
逐字节比对 : 完全一致 ✓ (12,441,600 字节,不同字节数 0)
```

### S3:RGA 能直接吃 dmabuf fd

| 输入方式 | 单次耗时 | PSNR(vs CPU 参考) |
|---|---|---|
| `imresize` + **dmabuf fd** | **21.89 ms** | 43.58 dB |
| `c_RkRgaBlit` + dmabuf fd | 22.52 ms | 43.58 dB |
| `c_RkRgaBlit` + 虚拟地址 | 26.04 ms | 43.58 dB |
| CPU 参考(盒式平均+色彩转换) | 54.57 ms | 基准 |

### S4/S5:三模式 perf 对比(各 30 帧)

| 模式 | preproc | rknn_run | postproc | **合计** | 帧率 | 每帧拷贝 | **CPU/帧** | CPU 占墙钟 |
|---|---|---|---|---|---|---|---|---|
| **A** 拷贝基线 | 34.39 | 39.34 | 13.73 | **92.5 ms** | 10.8 fps | 13,670,400 B | 38.28 ms | 38.8% |
| **B** RGA | 24.18 | 39.80 | 14.40 | **83.4 ms** | 12.0 fps | 1,228,800 B | 29.58 ms | 32.9% |
| **C** 全零拷贝 | **23.51** | 40.94 | **0.42** | **69.9 ms** | **14.3 fps** | **0 B** | **12.97 ms** | **17.4%** |

延迟 −24%、CPU −66%、拷贝 −100%。

### 多路并行(同一输入驱动 N 条独立流水线)

| 并发 | 拷贝版 | 零拷贝版 |
|---|---|---|
| 1 | 16.5 inf/s,28.2% CPU | **20.1 inf/s,12.5% CPU** |
| 2 | 20.5,41.4% | **22.8,16.2%** |
| 3 | 21.9,47.6% | **23.3,21.7%** |
| 4 | 22.7,**55.3%** | **23.5,27.5%** |

聚合吞吐被 NPU 卡在 ~23.5 inf/s(单次推理 41–43 ms),**零拷贝换来的是同样吞吐下一半的 CPU**,不是更多路。

### 流水线(3 线程 × 3 槽位)

| 负载 | 串行 | 流水线 |
|---|---|---|
| 每帧 1 次推理(相机 12–15 fps,采集受限) | 69.9 ms/帧 | 69.2 ms/帧(持平) |
| 每帧 2 次推理 | 110.2 ms/帧(9.1 fps) | **88.0 ms/帧(11.4 fps)** |

计算量超过采集周期后,流水线快 **25%**。

### 双路同时接 NPU(争用)

| 场景 | IMX415 | OV8858 | 聚合 | CPU |
|---|---|---|---|---|
| 单路 A | 14.38 fps(NPU 41.0 ms) | — | 14.38 inf/s | 18.2% |
| 单路 B | — | 17.24 fps(NPU 40.7 ms) | 17.24 inf/s | 28.3% |
| **双路** | 9.57 fps(NPU **77.1 ms**) | 9.47 fps(NPU **72.9 ms**) | **19.04 inf/s** | 30.3% |

`rknn_run` 墙钟 41 → 77 ms 不是 NPU 变慢,是**调用在等另一路**(时间片争用);聚合 19.04 inf/s 比单路上限 24.5 低 22%。两路需求 45 inf/s,**缺口 2.4 倍**——RK3568 的 NPU 只够一路实时分析。

## 踩过的坑(都很值钱)

1. **`rknn_set_io_mem()` 必须用 `RKNN_QUERY_NATIVE_INPUT_ATTR` + `pass_through=1`**  
   用普通 `INPUT_ATTR`(pass_through=0)时 runtime 每帧还要做一次内部转换:`rknn_run` 75.6 ms / CPU 49.9 ms,零拷贝反而最慢;换 native 后 41.2 ms / 23.2 ms。

2. **RGA 的 YUV→RGB 是 limited range(16–235)**,而 rkisp 报 `Full Range`  
   参考实现按 full range 写,暗场景会被压到全黑、PSNR 只有 25 dB。判据:输入 Y=100 时 RGA 输出 97.3 = `(100−16)×255/219`。

3. **int8 偏置要写成 8 字节异或**  
   模型输入 zp=−128,RGA2 没有 NN 单元。逐字节 `-128` 要 **12.15 ms**(uncached 内存),改成 `(v ^ 0x80)` 的 8 字节位运算后 **1.97 ms**。

4. **uncached 内存上绝对不要逐字节访问**  
   `ystat` 逐字节扫 4K Y 平面一帧要 **3.5 秒**(≈2 MB/s),同块内存 `memcpy` 是 **1 GB/s**。

5. **`VIDIOC_EXPBUF` 要缓存**  
   每帧导出/关闭本来以为很贵,实测 4 个 buffer 一次性导出只要 **0.12 ms**——不是瓶颈。优化前先量。

6. **相机本身只有 12–15 fps**(4K 与 1080p 相同),所以串行版 69.9 ms/帧其实已经贴着采集上限;要验证流水线收益必须加大计算负载。

## 已知限制

- 只验证了 RGA2(rga2 驱动)与 RKNN 2.3.2;`librga` 与内核驱动版本不匹配但仍可用
- NPU 以 DRM 节点暴露(`/dev/dri/card1`、`renderD129`,`DRIVER=RKNPU`),不是 `/dev/rknpu`
- 每帧仍需要一次 1.2 MB 的 int8 偏置位运算(RGA2 无 NN;RGA3 平台可省)
