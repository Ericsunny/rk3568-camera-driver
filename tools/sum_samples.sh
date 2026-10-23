#!/bin/sh
# 汇总所有检测样本的分数与框
for d in /root/det_samples/*/; do
  n=$(basename "$d")
  [ -f "$d/det.txt" ] || continue
  sm=$(grep -o 'score_max": [0-9.]*' "$d/metrics.json" 2>/dev/null | tail -n 1 | cut -d' ' -f2)
  pl=$(grep 'cls=0 ' "$d/det.txt" 2>/dev/null | tail -n 1)
  tf=$(grep -c 'det=' "$d/det.txt" 2>/dev/null)
  pf=$(grep -c 'cls=0 ' "$d/det.txt" 2>/dev/null)
  echo "=== $n  frames=$tf person_frames=$pf score_max=$sm"
  echo "    last_person: $pl"
done
