#!/usr/bin/env python3
"""Host-only integration tests for evrec_fault_preload.c; no camera needed.

The harness compiles the actual evw_close/ev_flush and SIGTERM/target-left
branches extracted from evrec.c. It does not execute the complete application.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "tools/evrec.c").read_text(encoding="utf-8")


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 0
    for pos in range(opening, len(SOURCE)):
        if SOURCE[pos] == "{":
            depth += 1
        elif SOURCE[pos] == "}":
            depth -= 1
            if not depth:
                return SOURCE[start:pos + 1]
    raise ValueError("unterminated function: " + signature)


def block(start, end):
    begin = SOURCE.index(start)
    return SOURCE[begin:SOURCE.index(end, begin)]


PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
static FILE *g_ev_fp;
static char g_ev_path[512];
static unsigned char ev_buf[64];
static unsigned ev_bn, g_ev_event_id = 7, g_ev_segment = 2;
static unsigned g_ev_coalesced, g_ev_cdskip;
static unsigned long g_ev_bytes;
static double g_ev_first_ts = 1, g_ev_last_ts = 2, g_last_detect_ms = 1;
static int g_ev_werr, g_events, g_max_events = 1;
static double now_ms(int clock_id) { (void)clock_id; return 2000.0; }
static long disk_free_mb(void) { return 1234; }
static void evw_event_end(const char *reason) { (void)reason; g_events++; }
'''

HARNESS = r'''
static int signal_close(unsigned trig_seq, double trig_ts)
{
    int got_term = 1, rc = 0, reached_max = 0;
SIGNAL_BLOCK
EXIT_BLOCK
    return 99;
}
static int target_left(unsigned trig_seq, double trig_ts)
{
    int rc = 0, reached_max = 0, post_sec = 1;
    for (;;) {
NORMAL_BLOCK
        break;
    }
    (void)reached_max;
    return rc;
}
int main(int argc, char **argv)
{
    char path[600];
    int rc;
    if (argc != 3) return 98;
    snprintf(g_ev_path, sizeof(g_ev_path), "%s/event-01", argv[1]);
    snprintf(path, sizeof(path), "%s.part", g_ev_path);
    g_ev_fp = fopen(path, "wb");
    if (!g_ev_fp) return 97;
    memcpy(ev_buf, "video", 5); ev_bn = 5;
    rc = strcmp(argv[2], "sigterm") == 0 ? signal_close(12, 3.0)
                                          : target_left(12, 3.0);
    printf("RESULT rc=%d events=%d event_id=%u segment=%u write_error=%d\n",
           rc, g_events, g_ev_event_id, g_ev_segment, g_ev_werr);
    return rc;
}
'''

PROBE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    char path[4096]; FILE *f;
    if (argc != 3) return 2;
    snprintf(path, sizeof(path), "%s/%s", argv[1], argv[2]);
    f = fopen(path, "w");
    if (!f) return 3;
    if (fprintf(f, "untouched=%d\n", 1) < 0) return 4;
    return fclose(f) ? 5 : 0;
}
'''

SEQUENCE = r'''
#include <stdio.h>
int main(int argc, char **argv)
{
    char path[4096]; FILE *f; int i, ok[3];
    if (argc != 2) return 2;
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/event-%d.json", argv[1], i);
        f = fopen(path, "w"); ok[i] = f != NULL;
        if (f && fclose(f)) return 3;
    }
    printf("%d %d %d\n", ok[0], ok[1], ok[2]);
    return 0;
}
'''


def run(command, **kwargs):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, **kwargs)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    signal = block('if (got_term && g_ev_fp != NULL && !rc) {',
                   '\n\t/* 信号 handler 只置 g_stop')
    exit_block = block('if (got_term && !rc) {', '\n\t/* 触发到 dump')
    normal = block('if (g_ev_fp != NULL && g_last_detect_ms > 0 &&',
                   '\n\t\tcontinue;\n\nlink_fault:')
    source = (PRELUDE + function('static void ev_flush(void)') + '\n' +
              function('static int evw_close(') + '\n' +
              HARNESS.replace('SIGNAL_BLOCK', signal)
                     .replace('EXIT_BLOCK', exit_block)
                     .replace('NORMAL_BLOCK', normal))
    with tempfile.TemporaryDirectory(prefix='evfi-test-') as tmp:
        tmp = Path(tmp).resolve()
        library = tmp / 'libevfi.so'
        probe = tmp / 'probe'
        sequence = tmp / 'sequence'
        (tmp / 'harness.c').write_text(source, encoding='utf-8')
        (tmp / 'probe.c').write_text(PROBE, encoding='utf-8')
        (tmp / 'sequence.c').write_text(SEQUENCE, encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-pthread',
                        str(ROOT / 'tools/evrec_fault_preload.c'), '-ldl',
                        '-o', str(library)], check=True)
        for src, output in [('probe.c', probe), ('sequence.c', sequence)]:
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(tmp / src), '-o', str(output)], check=True)
        scenarios = ('json-open', 'json-write', 'json-close', 'video-write', 'video-rename')
        for variant, flags in [('ordinary', []), ('fortified', ['-D_FORTIFY_SOURCE=2'])]:
            harness = tmp / ('harness-' + variant)
            subprocess.run(['cc', '-std=c11', '-O2', *flags, '-Wall', '-Wextra',
                            '-Werror', str(tmp / 'harness.c'), '-o', str(harness)], check=True)
            for close_kind in ('sigterm', 'target-left'):
                for fault in scenarios:
                    run_dir = tmp / (variant + '-' + close_kind + '-' + fault)
                    run_dir.mkdir()
                    env = os.environ.copy()
                    env.update(LD_PRELOAD=str(library), EVFI_DIR=str(run_dir),
                               EVFI_PREFIX='event-', EVFI_MODE=fault, EVFI_HIT='1')
                    result = run([str(harness), str(run_dir), close_kind], env=env)
                    require(result.returncode == 1, (fault, close_kind, result.stdout, result.stderr))
                    require(result.stderr.count('[evfi] HIT ') == 1 and
                            'operation=' + fault in result.stderr and
                            'path=' + str(run_dir / 'event-01') in result.stderr and
                            '[evfi] RESULT operation=' + fault in result.stderr,
                            (fault, close_kind, result.stderr))
                    require('RESULT rc=1 events=0 event_id=7 segment=2' in result.stdout,
                            (fault, close_kind, result.stdout))
                    require((run_dir / 'event-01.part').exists() == (fault == 'video-rename'), fault)
                    require((run_dir / 'event-01.h264').exists() == (fault != 'video-rename'), fault)
                    if fault == 'json-open' or fault == 'video-rename':
                        require(not (run_dir / 'event-01.json').exists(), fault)
                    if fault == 'json-write':
                        require((run_dir / 'event-01.pending.json').read_bytes() == b'{', fault)
                    require(not (run_dir / 'event-01.json').exists(), fault)
                    if fault == 'video-write':
                        require((run_dir / 'event-01.h264').read_bytes() == b'vide', fault)
                # A non-matching occurrence is a normal, successful control.
                run_dir = tmp / (variant + '-' + close_kind + '-control')
                run_dir.mkdir()
                env = os.environ.copy()
                env.update(LD_PRELOAD=str(library), EVFI_DIR=str(run_dir),
                           EVFI_PREFIX='event-', EVFI_MODE='json-open', EVFI_HIT='2')
                result = run([str(harness), str(run_dir), close_kind], env=env)
                expected_events = 0 if close_kind == 'sigterm' else 1
                require(result.returncode == 0 and '[evfi] HIT ' not in result.stderr and
                        f'RESULT rc=0 events={expected_events}' in result.stdout and
                        (run_dir / 'event-01.json').is_file(),
                        (variant, close_kind, result.stdout, result.stderr))
                print(variant + ' ' + close_kind + ': control and five failures PASS')

            for fault in scenarios:
                run_dir = tmp / (variant + '-unrelated-' + fault)
                run_dir.mkdir()
                env = os.environ.copy()
                env.update(LD_PRELOAD=str(library), EVFI_DIR=str(run_dir),
                           EVFI_PREFIX='different-', EVFI_MODE=fault, EVFI_HIT='1')
                result = run([str(harness), str(run_dir), 'target-left'], env=env)
                require(result.returncode == 0 and '[evfi] HIT ' not in result.stderr and
                        'RESULT rc=0 events=1' in result.stdout,
                        (variant, fault, result.stdout, result.stderr))
            print(variant + ' unrelated event paths are unaffected: PASS')

        # Armed but deliberately not matching: success and no false HIT.
        for case in ('directory', 'prefix', 'suffix', 'symlink', 'occurrence'):
            run_dir = tmp / ('scope-' + case)
            run_dir.mkdir()
            outside = tmp / ('outside-' + case)
            outside.mkdir()
            env = os.environ.copy()
            env.update(LD_PRELOAD=str(library), EVFI_DIR=str(run_dir),
                       EVFI_PREFIX='event-', EVFI_MODE='json-open',
                       EVFI_HIT='2' if case == 'occurrence' else '1')
            dest = outside if case == 'directory' else run_dir
            name = 'other.json' if case == 'prefix' else 'event-1.txt' if case == 'suffix' else 'event-1.json'
            if case == 'symlink':
                (run_dir / name).symlink_to(outside / 'safe.json')
            result = run([str(probe), str(dest), name], env=env)
            require(result.returncode == 0 and '[evfi] HIT ' not in result.stderr,
                    (case, result.returncode, result.stderr))
        print('scope, symlink and unhit occurrence: PASS')

        run_dir = tmp / 'second-occurrence'
        run_dir.mkdir()
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(library), EVFI_DIR=str(run_dir),
                   EVFI_PREFIX='event-', EVFI_MODE='json-open', EVFI_HIT='2')
        result = run([str(sequence), str(run_dir)], env=env)
        require(result.returncode == 0 and result.stdout.strip() == '1 0 1' and
                result.stderr.count('[evfi] HIT ') == 1 and
                'occurrence=2' in result.stderr and
                'path=' + str(run_dir / 'event-1.json') in result.stderr,
                ('one-shot second occurrence', result.stdout, result.stderr))
        print('selected occurrence injects only once: PASS')

        env = os.environ.copy()
        env.update(LD_PRELOAD=str(library), EVFI_DIR='/', EVFI_PREFIX='event-',
                   EVFI_MODE='json-open', EVFI_HIT='1')
        result = run([str(probe), str(tmp), 'event-1.json'], env=env)
        require(result.returncode == 125 and 'CONFIG ERROR' in result.stderr,
                ('invalid configuration', result.returncode, result.stderr))
        print('invalid configuration fails closed: PASS')


if __name__ == '__main__':
    main()
