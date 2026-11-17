#!/usr/bin/env python3
import json
from pathlib import Path
import re
import sys

base = Path(__file__).resolve().parent
result = {}
for name in ('imx', 'ov'):
    trace = (base / ('dual-npu-rga-' + name + '.trace')).read_text(errors='replace')
    queries = re.findall(r'^(\d+) ioctl\([^\n]*_IOC\(_IOC_READ, 0x72, 0x1, 0x1c\)[^\n]*= -1 EINVAL', trace, re.M)
    submits = re.findall(r'^(\d+) ioctl\([^\n]*_IOC\(_IOC_NONE, 0x50, 0x17, 0\)[^\n]*= 0$', trace, re.M)
    result[name] = {'version_query_pids': queries, 'successful_rga_blits': len(submits),
                    'submit_pids': sorted(set(submits)),
                    'pass': len(queries) == 1 and len(submits) > 0 and set(submits) == set(queries)}
result['pass'] = all(result[name]['pass'] for name in ('imx', 'ov'))
(base / 'dual-npu-trace-summary.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
sys.exit(0 if result['pass'] else 1)
