# -*- coding: utf-8 -*-
# fix_mini3a.py - 合并 mini3a.c 的 223/224 断行 + 清理 97 行注释警告
import io

p = r'D:\vm\github\rk3568-camera-driver\mini3a\mini3a.c'
s = io.open(p, encoding='utf-8').read()

# 1) 223/224 断行合并
lines = s.split('\n')
out = []
i = 0
fixed = 0
while i < len(lines):
    l = lines[i]
    if ('--help' in l and l.rstrip().endswith('[-v] [--help]')
            and i + 1 < len(lines) and lines[i+1].lstrip().startswith('", argv[0]')):
        merged = l.rstrip() + '\\n' + lines[i+1].lstrip()
        assert 'argv[0]' in merged
        out.append(merged)
        fixed += 1
        i += 2
        continue
    out.append(l)
    i += 1
assert fixed == 1, 'fixed=%d' % fixed
s = '\n'.join(out)

# 2) 97 行注释(*y/*u/*v 触发 -Wcomment)
lines = s.split('\n')
for i, l in enumerate(lines):
    if '*y/*u/*v' in l:
        lines[i] = l.replace('*y/*u/*v', 'y u v (三个出参)')
s = '\n'.join(lines)
assert '*y/*u/*v' not in s

# 3) 终验:--help 的 printf 是完整单行
hit = [l for l in s.split('\n') if '--help' in l and 'printf' in l]
assert len(hit) == 1 and 'argv[0]' in hit[0], hit

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('OK: merged %d broken line(s) + cleaned comment warning' % fixed)
