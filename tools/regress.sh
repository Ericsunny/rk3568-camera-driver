#!/bin/sh
# regress.sh —— camd 自动回归:启停循环 / 长跑 / 队列策略 / 慢消费者 / 故障恢复 / 泄漏
# 用法: sh regress.sh [标签]
# 产物: /root/regress_<标签>/{report.md, *.json, *.log}
set -u
TAG=${1:-$(date +%m%d_%H%M)}
OUT=/root/regress_$TAG
mkdir -p $OUT
CAM=/root/camd
REPORT=$OUT/report.md
# 统一配置:8 buffer + rt 深度 3 + ev 深度 2(满足 nbufs >= rt+ev+2,见 docs/29 §6)
CFG="-b 8 --rt-depth 3 --ev-depth 2 --ev-fps 10"

say() { echo "$@" | tee -a $REPORT; }
hr() { echo "--------------------------------------------------------------" | tee -a $REPORT; }

row() {  # row <tag> <json> <备注>
  python3 - "$1" "$2" "${3:-}" <<'PY' | tee -a $REPORT
import json, sys
tag, path, note = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    d = json.load(open(path))
except Exception:
    print("| %-10s | 无 JSON | | | | | %s |" % (tag, note)); raise SystemExit
print("| %-10s | %5d | %5d | %6s | %7.2f | %6.2f | %s |" % (
    tag, d["frames"], d["frames_returned"], d["qbuf_errors"], d["fps"],
    d["first_frame_ms"], note))
PY
}

say "# camd 回归报告 ($TAG)"
say ""
say "kernel: $(uname -r) $(uname -v) | camd md5: $(md5sum $CAM | cut -d' ' -f1)"
say "IQ md5: $(md5sum /etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json 2>/dev/null | cut -d' ' -f1)"
say "统一配置: \`$CFG\`"
say ""

hr
say "## 1. 启停循环(10 次,各 30 帧)"
say ""
say "| 轮次 | rc | 耗时(s) | frame==returned 且 qbuf=0 |"
say "|---|---|---|---|"
i=1
ST_FAIL=0
while [ $i -le 10 ]; do
  t0=$(date +%s%N)
  $CAM -n 30 $CFG --json $OUT/cycle_$i.json >$OUT/cycle_$i.log 2>&1
  rc=$?
  t1=$(date +%s%N)
  ms=$(( (t1 - t0) / 1000000 ))
  eq=$(python3 -c "import json;d=json.load(open('$OUT/cycle_$i.json'));print('yes' if d['rc']==0 and d['frames']==d['frames_returned'] and d['qbuf_errors']==0 else 'NO')" 2>/dev/null || echo "?")
  [ "$eq" = "yes" ] || ST_FAIL=$((ST_FAIL+1))
  say "| $i | $rc | $(awk "BEGIN{printf \"%.1f\", $ms/1000}") | $eq |"
  i=$((i+1))
done
say ""
say "**启停循环结论: 失败轮次 = $ST_FAIL / 10**"
say ""

hr
say "## 2. 长跑(1800 帧)"
say ""
$CAM -n 1800 $CFG --json $OUT/long.json >$OUT/long.log 2>&1
say "rc=$?"
row long $OUT/long.json "长跑 1800 帧"
python3 - $OUT/long.json <<'PY' | tee -a $REPORT
import json, sys
d = json.load(open(sys.argv[1]))
print("")
print("RT p95/p99 = %s / %s us, EV p95/p99 = %s / %s us" % (
    d["rt"]["p95_us"], d["rt"]["p99_us"], d["ev"]["p95_us"], d["ev"]["p99_us"]))
print("RT dropped = %s, EV dropped = %s" % (d["rt"]["dropped"], d["ev"]["dropped"]))
print("首帧 %.1f ms, 帧率 %.2f fps, frames == returned → %s" % (
    d["first_frame_ms"], d["fps"],
    "无泄漏" if d["frames"] == d["frames_returned"] else "有泄漏!"))
PY
say ""

hr
say "## 3. 队列策略 / 慢消费者(300 帧)"
say ""
say "| 臂 | 说明 |"
say "|---|---|"
say "| A_drop | 取证队列丢最旧(默认) |"
say "| B_block | 取证队列有界阻塞(背压对照) |"
say "| C_slow | 实时消费者 200 ms/帧(慢消费者) |"
say ""
$CAM -n 300 $CFG --ev-policy drop  --json $OUT/A_drop.json  >$OUT/A_drop.log 2>&1
$CAM -n 300 $CFG --ev-policy block --json $OUT/B_block.json >$OUT/B_block.log 2>&1
$CAM -n 300 $CFG --fault slow --ev-policy drop --json $OUT/C_slow.json >$OUT/C_slow.log 2>&1

say "| 臂 | frames | returned | qbuf_err | fps | 首帧(ms) | 备注 |"
say "|---|---|---|---|---|---|---|"
row A_drop  $OUT/A_drop.json  "默认:丢最旧"
row B_block $OUT/B_block.json "对照:背压"
row C_slow  $OUT/C_slow.json  "慢消费者 200ms/帧"
say ""
python3 - $OUT/A_drop.json $OUT/B_block.json $OUT/C_slow.json <<'PY' | tee -a $REPORT
import json, sys
a, b, c = [json.load(open(p)) for p in sys.argv[1:4]]
print("A_drop  fps=%.2f, RT dropped=%d, EV dropped=%d" % (a["fps"], a["rt"]["dropped"], a["ev"]["dropped"]))
print("B_block fps=%.2f (背压把采集压到取证速率), EV dropped=%d" % (b["fps"], b["ev"]["dropped"]))
print("C_slow  fps=%.2f, RT dropped=%d → 采集%s被慢消费者拖住" % (
    c["fps"], c["rt"]["dropped"], "未" if c["fps"] > 0.8 * a["fps"] else "已"))
PY
say ""

hr
say "## 4. 故障注入与有界恢复"
say ""
say "### 4.1 一次性故障(--fault fail --fault-once):应恢复并继续跑到 60 帧"
$CAM -n 60 $CFG --fault fail --fault-once --recover-attempts 1 --json $OUT/D_recover_ok.json >$OUT/D_recover_ok.log 2>&1
DO_RC=$?
say "退出码 = $DO_RC"
grep -e "FATAL" -e "有界恢复" $OUT/D_recover_ok.log | sed 's/^/    /' | tee -a $REPORT
row D_recover_ok $OUT/D_recover_ok.json "一次性故障后恢复"
say ""
say "### 4.2 持续故障(--fault fail):应在 1 次恢复后放弃"
$CAM -n 0 $CFG --fault fail --recover-attempts 1 --json $OUT/E_recover_fail.json >$OUT/E_recover_fail.log 2>&1
DR_RC=$?
say "退出码 = $DR_RC(非 0 表示按设计放弃)"
grep -e "FATAL" -e "有界恢复" $OUT/E_recover_fail.log | sed 's/^/    /' | tee -a $REPORT
say ""
DIAG=$(ls -dt /root/camd_diag_* 2>/dev/null | head -n 1)
if [ -n "${DIAG:-}" ]; then
  say "诊断包: $DIAG"
  say "包含: $(ls $DIAG | tr '\n' ' ')"
else
  say "!! 未找到诊断包"
fi
say ""

hr
say "## 5. 结论"
say ""
python3 - $OUT $DO_RC $DR_RC $ST_FAIL <<'PY' | tee -a $REPORT
import json, os, sys
d, do_rc, dr_rc, st_fail = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
def load(n):
    p = os.path.join(d, n)
    return json.load(open(p)) if os.path.exists(p) else None
long = load("long.json"); a = load("A_drop.json"); b = load("B_block.json")
c = load("C_slow.json"); ok = load("D_recover_ok.json")
okall = True
if st_fail == 0:
    print("- 启停循环 10/10 成功")
else:
    print("- 启停循环失败 %d/10" % st_fail); okall = False
if long:
    leak = long["frames"] - long["frames_returned"]
    print("- 长跑 buffer 归还: %d/%d 差 %d → %s" % (
        long["frames"], long["frames_returned"], leak, "无泄漏" if leak == 0 else "有泄漏!"))
    print("  长跑首帧 %.1f ms, 帧率 %.2f fps, RT p99 %s us" % (
        long["first_frame_ms"], long["fps"], long["rt"]["p99_us"]))
    okall &= leak == 0
if a and b:
    print("- 背压对照: A_drop %.2f fps vs B_block %.2f fps → %s" % (
        a["fps"], b["fps"], "丢最旧避免背压" if a["fps"] > b["fps"] * 1.2 else "需复核"))
if a and c:
    print("- 慢消费者: 采集 %.2f fps(A 臂 %.2f)→ %s" % (
        c["fps"], a["fps"], "未被拖住" if c["fps"] > 0.8 * a["fps"] else "被拖住"))
if ok:
    print("- 一次性故障恢复: rc=%d, 恢复后跑满 %d 帧 → %s" % (
        do_rc, ok["frames"], "恢复成功" if do_rc == 0 and ok["frames"] >= 60 else "恢复失败"))
    okall &= do_rc == 0
print("- 持续故障: rc=%d → %s" % (dr_rc, "按设计放弃" if dr_rc != 0 else "未按设计退出"))
print("")
print("**总体: %s**" % ("PASS" if okall and dr_rc != 0 else "FAIL"))
PY

echo ""
echo "报告: $REPORT"
