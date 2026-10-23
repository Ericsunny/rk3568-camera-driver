#!/bin/sh
# regress.sh —— camd 自动回归:启停循环 / 长跑 / 双消费者 / 故障恢复
# 用法: sh regress.sh [标签]
# 产物: /root/regress_<标签>/ {report.md, *.json, *.log}
set -u
TAG=${1:-$(date +%m%d_%H%M)}
OUT=/root/regress_$TAG
mkdir -p $OUT
CAM=/root/camd
REPORT=$OUT/report.md

say() { echo "$@" | tee -a $REPORT; }

hr() { echo "--------------------------------------------------------------" | tee -a $REPORT; }

# 从 JSON 里取字段做表(用 python3,板上自带 3.8)
row() {  # row <tag> <json> [备注]
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
say ""

hr
say "## 1. 启停循环(10 次,各 30 帧)"
say ""
say "| 轮次 | rc | 耗时(s) | frame==returned |"
say "|---|---|---|---|"
i=1
ST_FAIL=0
while [ $i -le 10 ]; do
  t0=$(date +%s%N)
  $CAM -n 30 --analyze-ms 2 --ev-fps 5 --ev-policy drop --json $OUT/cycle_$i.json >$OUT/cycle_$i.log 2>&1
  rc=$?
  t1=$(date +%s%N)
  ms=$(( (t1 - t0) / 1000000 ))
  eq=$(python3 -c "import json;d=json.load(open('$OUT/cycle_$i.json'));print('yes' if d['frames']==d['frames_returned'] and d['qbuf_errors']==0 else 'NO')" 2>/dev/null || echo "?")
  [ "$eq" = "yes" ] || ST_FAIL=$((ST_FAIL+1))
  say "| $i | $rc | $(awk "BEGIN{printf \"%.1f\", $ms/1000}") | $eq |"
  i=$((i+1))
done
say ""
say "启停循环结论: 失败轮次 = $ST_FAIL / 10"
say ""

hr
say "## 2. 长跑(1800 帧,约 60 s)"
say ""
$CAM -n 1800 --analyze-ms 2 --ev-fps 10 --ev-policy drop --json $OUT/long.json >$OUT/long.log 2>&1
LRC=$?
say "rc=$LRC"
row long $OUT/long.json "长跑 1800 帧"
python3 - $OUT/long.json <<'PY' | tee -a $REPORT
import json, sys
d = json.load(open(sys.argv[1]))
print("")
print("RT p95/p99 = %s / %s us, EV p95/p99 = %s / %s us" % (
    d["rt"]["p95_us"], d["rt"]["p99_us"], d["ev"]["p95_us"], d["ev"]["p99_us"]))
print("RT dropped = %s, EV dropped = %s, 丢帧率 RT=%.2f%%" % (
    d["rt"]["dropped"], d["ev"]["dropped"],
    100.0 * d["rt"]["dropped"] / max(1, d["rt"]["processed"] + d["rt"]["dropped"])))
print("首帧 %.1f ms, 有效帧率 %.2f fps" % (d["first_frame_ms"], d["fps"]))
PY
say ""

hr
say "## 3. 双消费者策略对照(300 帧)"
say ""
say "| 策略 | 说明 |"
say "|---|---|"
say "| A_drop | 取证队列丢最旧,采集不被拖慢 |"
say "| B_block | 取证队列有界阻塞(背压),采集被拖慢 |"
say "| C_slow | 实时消费者每帧 200 ms(慢消费者) |"
say ""
$CAM -n 300 --analyze-ms 0 --ev-fps 5 --ev-policy drop  --json $OUT/A_drop.json  >$OUT/A_drop.log 2>&1
$CAM -n 300 --analyze-ms 0 --ev-fps 5 --ev-policy block --json $OUT/B_block.json >$OUT/B_block.log 2>&1
$CAM -n 300 --analyze-ms 20 --fault slow --ev-fps 5 --ev-policy drop --json $OUT/C_slow.json >$OUT/C_slow.log 2>&1

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
print("B_block fps=%.2f, RT dropped=%d, EV dropped=%d" % (b["fps"], b["rt"]["dropped"], b["ev"]["dropped"]))
print("C_slow  fps=%.2f, RT dropped=%d, RT p99=%sus (采集是否被慢消费者拖住 → 看 fps)" % (
    c["fps"], c["rt"]["dropped"], c["rt"]["p99_us"]))
print("结论: A vs B 的 fps 差 = %.2f fps(%s)" % (
    a["fps"] - b["fps"],
    "丢最旧确实避免背压" if a["fps"] > b["fps"] * 1.2 else "两者接近,需复核"))
PY
say ""

hr
say "## 4. 故障注入与有界恢复"
say ""
say "注入:实时消费者内部故障(--fault fail),恢复次数上限 1"
$CAM -n 0 --analyze-ms 0 --fault fail --recover-attempts 1 --ev-fps 5 --ev-policy drop --json $OUT/D_fail.json >$OUT/D_fail.log 2>&1
DRC=$?
say ""
say "退出码 = $DRC (非 0 表示故障按预期上报)"
say "日志关键行:"
grep -e "FATAL" -e "有界恢复" -e "诊断包" $OUT/D_fail.log | sed 's/^/    /' | tee -a $REPORT
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
python3 - $OUT <<'PY' | tee -a $REPORT
import json, os, sys
d = sys.argv[1]
def load(n):
    p = os.path.join(d, n)
    return json.load(open(p)) if os.path.exists(p) else None
long = load("long.json"); a = load("A_drop.json"); b = load("B_block.json"); c = load("C_slow.json")
ok = True
if long:
    leak = long["frames"] - long["frames_returned"]
    print("长跑 buffer 归还: frames=%d returned=%d 差=%d → %s" % (
        long["frames"], long["frames_returned"], leak, "无泄漏" if leak == 0 else "有泄漏!"))
    ok &= leak == 0
    print("长跑首帧 %.1f ms, 帧率 %.2f fps" % (long["first_frame_ms"], long["fps"]))
if a and b:
    print("背压对照: A_drop %.2f fps vs B_block %.2f fps" % (a["fps"], b["fps"]))
if c:
    print("慢消费者: 采集 %.2f fps, RT 丢弃 %d 帧 → 采集%s被拖住" % (
        c["fps"], c["rt"]["dropped"], "未" if c["fps"] > 20 else "已"))
print("")
print("总体:", "PASS" if ok else "FAIL")
PY

echo ""
echo "报告: $REPORT"
