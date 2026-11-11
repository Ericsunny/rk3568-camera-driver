# 双路自然收尾尝试：场景不满足条件

2026-11-11 约 10:54—10:56，在双路检测及 RTSP 客户端运行期间，尝试按预定的无人、入镜、离开顺序验证自动收尾。IMX415 在预定无人阶段已持续检测并打开事件；现场从 RTSP 临时读取的预览中观察到画面边缘有其他人，屏幕中也有人像，因此不能将此阶段作为无人样本，也不能据此把持续命中认定为模型误检。预览仅用于现场判断，已从电脑和板端临时目录删除；事件录像继续留在板端，不随日志导出。

OV8858 在本轮有 `target_left` 事件，IMX415 有按最大时长分段及后续 `target_left` 记录；由于双路无人基线不成立，且无法仅凭日志区分画面中的人物，不将本轮计为双路受控验收通过。[IMX 日志](dual-event-scheduled-20261111/run/imx415.log) · [OV 日志](dual-event-scheduled-20261111/run/ov8858.log) · [结束状态](dual-event-scheduled-20261111/watch-status.json)

发现画面含其他人后通过停止文件结束，两路服务退出 0，3A active，采集节点空闲。现场日志与元数据包 [scheduled-evidence-raw.tar.gz](scheduled-evidence-raw.tar.gz) 不含画面或录像，双端 SHA-256 均为 `d4446fc3cb164f4757cd13120fc224d9086b981dd6b1e384e02ae09bfdd8788d`。
