# 本项目维护约定

> 本仓库 = RK3568 相机子系统工程:自写 OV8858 V4L2 subdev 驱动 + 采集/检测/事件录像应用 evrec + 全部实测证据(evidence/)。
> **文档是唯一事实源**:一切以 docs/ 与 git log 为准;本文件只做入口与纪律。

## 铁律(任何一条冲突即停手问用户)

1. **不 force-push、不改写已推送历史**;打 tag / 发 Release / 合并 PR 必须用户明确批准。
2. **刷写类操作逐次请批**:覆盖 /boot、分区、bootloader、整片 eMMC 都算;/boot 历史上只允许过"覆盖 Image 文件"这一种。
3. **代码改动**走 `feature/*`、`fix/*` 分支 + PR 进 main;**docs/ 编号文档的新增/修订、README 数字同步、只读体检的报告文件,按仓库惯例可直接提交 main**(历届会话如此);CI 红了先修 CI 再谈合并。
4. 提交/推送前跑 `tools/check_secrets.sh`(本地脚本,.gitignore 已排除):evidence/ 历史日志曾混入出厂默认凭据,新增证据**必须先脱敏再入库**。
5. 板子与 PC 是共享硬件:**串口独占**(COM7,用前确认 MobaXterm/putty 已断开);板上危险命令(rm -rf、dd、flash 写)逐条请批。
6. 结论措辞克制:不写"永久""全部排除""必然";只写证据支撑得住的口径(用户 review 逐项核对数字,极严)。

## 环境地图(Windows 主机)

| 项 | 值 |
|---|---|
| 真板 | `ssh rkboard`(192.168.10.2,Linux);串口 **COM7 @ 1500000**,工具 `D:\tools\serial-console\board.py`(子命令 ports/probe/run/send/listen) |
| 编译 VM | `ssh rk3568`(192.168.10.3,x86 Ubuntu 20);内核树 `~/rk3568/RK_SDK/kernel` |
| 内核产物 | sensor/rkcif 驱动**内建 =y,不可 insmod**:改驱动必须重编 Image 并覆盖 /boot/Image;板上时钟漂移会让 make 拒编,先 `date -s` 校钟 |
| 板端 evrec | `gcc -DHAVE_RKNN -DHAVE_RGA -lm`;rknn_api.h 补到 /usr/include;librknnrt 2.3.2(与模型配套,勿动);板端后台常驻用 nohup+disown |
| TFTP | PC `D:\vm\tftp_serverd.py`(计划任务常驻;PC 重启后 bind 失败→重拉即愈) |
| ffmpeg | `D:\vm\rk_recovery\ffbin\`(ffprobe/拉流/解码测试用) |
| gh CLI | `C:\Users\Administrator\AppData\Local\Programs\gh\bin\gh.exe`(已登录 keyring,HTTPS 走代理;**不在默认 PATH**,沙箱里用全路径或先 export) |
| 板约束 | 板无公网(apt 不可用,GitHub 慢→主机 curl 下载再 scp);/tmp 是 tmpfs 重启清空;**scp 后必须 md5 双端核对**(scp 有静默失败前科) |

## 已知坑(前科实锤,别再踩)

- U-Boot autoboot 倒计时 **0 秒**:事后补发按键必然错过;拦截必须从 `reboot` 发出那一刻起持续灌键,现成脚本 `tools/uboot_catch.py`(灌 回车/Ctrl+C,见到 `=>` 立即收手)。
- dash 不支持 `${var:0:8}`(整脚本 exit 2);`pgrep -x` 有 15 字符截断(comm 只保留前 15),查长名进程用 `pgrep -f`。
- python `s.replace` 无 assert 静默失败——任何文本替换后必须验证产物含预期字符串。
- 在线 API 查 commit 数可能拿到过期缓存;**commit 数以 `git rev-list --count HEAD` 为准**。
- `ssh` 别名必须用别名本身(rkboard/rk3568);裸 IP 会撞 Host key verification failed。

## 文档地图(docs/ 是事实源,按需读,不要通读)

- **`docs/archive/项目状态与后续任务.md` = 项目状态与后续任务,从这里开始**(现状、板子状态、T1-T5、挂起件清单)。
- 历史编号 08–41 文档中部分已归档,入口见 `docs/archive/README.md`:项目状态、34(终极验收)、16(动手手册,GPIO/上电时序/I2C)、29(V4L2 节点归属)、35(3A 运行口径)、36–38(启动故障根因、事件治理、多客户端)、代码审查与修复记录。
- `evidence/`:逐次实验原始日志,**只增不改**;README 首屏读数与仓库实况要同步(历史上 CI 核对过,改文档记得带 README)。

## 当前任务队列

→ `docs/archive/项目状态与后续任务.md` §2。做完一件更新一件:代码/文档/证据同一批 commit;任务状态、板子状态、README 首屏读数三处同步。
