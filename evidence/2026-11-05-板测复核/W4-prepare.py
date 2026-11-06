#!/usr/bin/env python3
"""Create an isolated, instrumented zc_npu copy; never edit its input."""
from pathlib import Path
import sys


def replace_once(text, old, new, count=1):
    found = text.count(old)
    if found != count:
        raise ValueError('expected %d occurrences, got %d for %r' % (count, found, old))
    return text.replace(old, new)


def main():
    if len(sys.argv) != 3:
        raise ValueError('usage: W4-prepare.py ORIGINAL_SOURCE NEW_SOURCE')
    original, output = map(Path, sys.argv[1:])
    text = original.read_text(encoding='utf-8')
    text = replace_once(text, '/root/s4_det.ppm', 's4_det.ppm', 3)
    text = replace_once(text, 'do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);\n\treturn r;',
                        'do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);\n'
                        '\tif (r < 0) { perror("W4 ioctl"); exit(1); }\n\treturn r;')
    text = replace_once(text, '\t\tmem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);',
                        '\t\tmem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);\n'
                        '\t\tif (mem[i] == MAP_FAILED) { perror("W4 mmap"); return 1; }')
    text = replace_once(text, '\t\t\timresize(s, d);',
                        '\t\t\tif (imresize(s, d) != IM_STATUS_SUCCESS) { fprintf(stderr, "W4 RGA failed\\n"); break; }', 2)
    text = replace_once(text, '\t\t\t\tIM_STATUS st = imresize(s, d);',
                        '\t\t\t\tIM_STATUS st = imresize(s, d);\n'
                        '\t\t\t\tif (st != IM_STATUS_SUCCESS) { fprintf(stderr, "W4 RGA failed\\n"); break; }')
    text = replace_once(text, '\tint total_boxes = 0;\n\n\tfor (int f = 0; f < frames; f++)',
                        '\tint total_boxes = 0;\n\tint completed = 0;\n\n\tfor (int f = 0; f < frames; f++)')
    text = replace_once(text, '\t\t}\n\t\tdouble t4 = now_ms();\n\t\tt_run += t4 - t3;',
                        '\t\t}\n\t\tif (ret < 0) break;\n\t\tdouble t4 = now_ms();\n\t\tt_run += t4 - t3;')
    text = replace_once(text, '\t\txioctl(fd, VIDIOC_QBUF, &b);\n\t}\n\n\txioctl(fd, VIDIOC_STREAMOFF, &btype);',
                        '\t\txioctl(fd, VIDIOC_QBUF, &b);\n\t\tcompleted++;\n\t}\n\n'
                        '\txioctl(fd, VIDIOC_STREAMOFF, &btype);')
    text = replace_once(text, '\tdouble n = frames;\n\tdouble cpu_ms =',
                        '\tprintf("W4 completed=%d requested=%d wall_ms=%.2f throughput_fps=%.2f\\n",\n'
                        '\t       completed, frames, wall1-wall0, completed*1000.0/(wall1-wall0));\n'
                        '\tif (completed != frames) return 1;\n\tdouble n = frames;\n\tdouble cpu_ms =')
    with output.open('x', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('created isolated source:', output)


if __name__ == '__main__':
    main()
