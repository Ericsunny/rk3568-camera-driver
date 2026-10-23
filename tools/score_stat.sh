#!/bin/sh
# 逐样本统计 person(cls=0) 分数的 min/avg/max 与框高
for d in /root/det_samples/*/; do
  n=$(basename "$d")
  [ -f "$d/det.txt" ] || continue
  grep 'cls=0 ' "$d/det.txt" 2>/dev/null \
  | sed -e 's/.*cls=0 s=//' -e 's/ .*//' \
  | awk -v N="$n" '
    {v=$1+0; if(NR==1){mn=v;mx=v}; if(v<mn)mn=v; if(v>mx)mx=v; s+=v}
    END{ if(NR>0) printf "%-14s n=%2d  person_score min=%.2f avg=%.2f max=%.2f\n", N, NR, mn, s/NR, mx; else printf "%-14s n= 0\n", N }'
done
