# IMX415 单路测量与发布记录

2026-11-17，RK3568 / Linux4.19.232 #62、IMX415、1080p NV12、标称15fps/GOP15/4000kbps。主服务设计见[架构](../../docs/overview/architecture.md)，负载、收益与限制见[结果](../../docs/improvement/project-review.md)。

## 内容与复算

- `abba/`：同工作量串行/流水线四轮的原始frame CSV/meta，流水线另有detect CSV/meta，字节一致导出。
- `person/`：1638帧人员场景的同类观测，不含人员影像。
- `encoder-cpu.json`：检测/观测关闭、四轮/模式CPU计数及汇总。
- `pipeline-abba.json`：性能r1四轮与r3资源轮结构化摘要，明确二进制身份。
- `person.json`：结果年龄、文件核对元数据和严格双轮协议false。
- `provenance.json`：公开CSV的来源相对路径、原始字节哈希，以及摘要输入哈希。
- `runtime-source-hashes.json`：导出时实验源码字节；`published-source-hashes.json`：Git规范化后的发布源码字节，两者区分行尾格式。

```sh
python3 evidence/2026-11-17-single-camera-release/verify.py
python3 tools/analyze_frame_metrics.py evidence/2026-11-17-single-camera-release/person/frame-metrics.csv
```

使用 `--check-current-source` 可另核对当前源码是否与本次发布指纹相同；后续版本可仅复算历史数据。复算检查CSV来源哈希、软件吞吐/驱动缺口、检测覆盖与P95、CPU加权成本。它不重跑摄像头、SDK或神经网络。

## 范围与保留问题

性能r1的ABBA与后续r3人员/资源场景不是同一构建，不能移用二进制身份。DQ到应用是软件年龄，不是物理入镜或显示发光时间。人员原计划两轮，另有一次计划外入镜，严格双轮协议false保留，不计算准确率。

完整本地控制记录、设备日志和录像未随本数据组公开。事件帧数与独立解码结论保存为元数据，不能仅凭该组CSV重新解码媒体。旧manual约300ms光学基线也没有在此提供录像，不能据此重做物理误差校准。

公开发布只改变仓库源码和文档，不触发板端部署。主机回归命令及结果见`release-checks.json`与`host-tests.log`（发布准备阶段产生）。
