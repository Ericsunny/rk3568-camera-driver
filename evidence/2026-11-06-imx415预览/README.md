# IMX415 采集预览（2026-11-06）

这张照片用于展示现场采集画面，不是画质测试或修复前后对比。玩偶、光线与历史照片不同，不能据此比较性能或色彩准确性。

- 场景：室内自然与室内照明，红白玩偶；采集前由现场人员调整镜头和物体位置。
- 设备：IMX415 → RK ISP mainpath → `/dev/video5`；现役内核 `4.19.232 #52`，`rkaiq_3A_server` 运行中。板上镜像与当前仓库驱动源码是否一致未核实。
- 当前格式查询：NV12，3840×2160，bytesperline 3840，sizeimage 12,441,600 字节；未修改格式或 3A 配置。
- 采集时间：2026-11-06 11:01:08 CST，板端时钟；未与外部时间源核对。
- 采集：`timeout 20 v4l2-ctl -d /dev/video5 --stream-mmap=4 --stream-count=12 --stream-to=frames.nv12`，返回 0，采集到 149,299,200 字节。保留展示第 12 帧，未验证 3A 此时是否收敛。
- 提取：`dd if=frames.nv12 of=frame12.nv12 bs=12441600 skip=11 count=1`；转换：`ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format nv12 -video_size 3840x2160 -color_range pc -i frame12.nv12 -frames:v 1 frame12.png`。
- [frame12.png](frame12.png) 的 MD5 为 `a666bacdad9bb2666dd8a09af09a26dd`，复制前板端与复制后主机一致。原始 NV12 临时文件未入库；PNG 是经 FFmpeg 转换的图像，并非原始传感器数据。

照片存在偏蓝和倾斜，不能据此宣称自动白平衡或图像质量已验证。没有拍摄同场景的处理前后组。
