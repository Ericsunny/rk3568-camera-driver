#!/usr/bin/env python3
"""Instrument an isolated W6 copy; never edit production or evidence sources."""
from pathlib import Path
import sys


HELPER = r'''
/* Test-only markers: these are instrumentation, not driver-generated records. */
#include <unistd.h>
#include <errno.h>
#include <string.h>
static int w6_init_marker(const char *phase)
{
    const char *token = getenv("W6_TOKEN");
    char message[160];
    if (!token || strlen(token) != 32 || strspn(token, "0123456789abcdef") != 32) {
        fprintf(stderr, "W6 marker failed: invalid token\n");
        return -1;
    }
    int n = snprintf(message, sizeof(message), "<6>W6_INIT:%s:%ld:%s\n",
                     token, (long)getpid(), phase);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("W6 marker open failed");
        return -1;
    }
    ssize_t written = write(fd, message, (size_t)n);
    int close_rc = close(fd);
    if (written != n || close_rc != 0) {
        fprintf(stderr, "W6 marker write/close failed\n");
        return -1;
    }
    return 0;
}

'''


def prepare(text):
    if 'w6_init_marker(' in text:
        raise ValueError('input already instrumented')
    anchor = 'int main('
    if text.count(anchor) != 1:
        raise ValueError('expected one main')
    text = text.replace(anchor, HELPER + anchor, 1)
    begin = 'fprintf(stderr, "W6_BEGIN:%d\\n", f);'
    if text.count(begin) != 3:
        raise ValueError('expected three W6 begin sites')
    text = text.replace(begin, 'if (f == 0 && w6_init_marker("BEGIN")) break;\n\t\t\t' + begin)
    for value, expected in (('w6_status', 2), ('st', 1)):
        end = 'fprintf(stderr, "W6_END:%d:%d\\n", f, (int)' + value + ');'
        if text.count(end) != expected:
            raise ValueError('unexpected W6 end sites')
        text = text.replace(end, end + '\n\t\t\tif (f == 0 && w6_init_marker("END")) break;')
    return text


def main():
    if len(sys.argv) != 3:
        raise ValueError('usage: w6_kmsg_prepare.py W6_SOURCE NEW_SOURCE')
    text = prepare(Path(sys.argv[1]).read_text(encoding='utf-8'))
    with Path(sys.argv[2]).open('x', encoding='utf-8', newline='\n') as out:
        out.write(text)
    print('created isolated kernel-marker W6 source:', sys.argv[2])


if __name__ == '__main__':
    main()
