#!/usr/bin/env python3
"""Test the actual H.264 packet classification without camera hardware."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'evrec.c').read_text(encoding='utf-8')
start = source.index('static int h264_packet_flags(')
end = source.index('\n}', start) + 2
test = r'''
#include <stdio.h>
FUNCTION
int main(void) {
    const unsigned char complete[] = {0,0,0,1,0x67,9,0,0,1,0x68,9,0,0,1,0x65,9};
    const unsigned char sps[] = {0,0,1,0x67};
    const unsigned char bare_idr[] = {0,0,1,0x65};
    const unsigned char backwards[] = {0,0,1,0x65,0,0,1,0x67,0,0,1,0x68};
    if ((h264_packet_flags(complete, sizeof(complete)) & 9) != 9) return 1;
    if (h264_packet_flags(sps, sizeof(sps)) != 2) return 2;
    if (h264_packet_flags(bare_idr, sizeof(bare_idr)) != 1) return 3;
    if (h264_packet_flags(backwards, sizeof(backwards)) & 8) return 4;
    puts("IDR and independently decodable start classification: PASS");
    return 0;
}
'''.replace('FUNCTION', source[start:end])
with tempfile.TemporaryDirectory(prefix='evrec-nal-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(test, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
