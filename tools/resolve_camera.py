#!/usr/bin/env python3
"""Resolve an ISP mainpath only when its enabled media graph reaches the sensor."""
from collections import deque
import glob
import re
import subprocess
import sys

PROFILES = {
    'imx415-isp': ('rkisp-vir0', 'imx415'),
    'ov8858-isp': ('rkisp-vir1', 'ov8858'),
}


def graph(text):
    driver = re.search(r'^driver\s+(\S+)', text, re.MULTILINE)
    model = re.search(r'^model\s+(\S+)', text, re.MULTILINE)
    entities, edges = {}, {}
    current = None
    for line in text.splitlines():
        match = re.match(r'- entity \d+: (.*?) \(', line)
        if match:
            current = match.group(1)
            entities[current] = None
            edges[current] = set()
            continue
        if current is None:
            continue
        match = re.search(r'device node name\s+(/dev/\S+)', line)
        if match:
            entities[current] = match.group(1)
        match = re.search(r'-> "([^"]+)":\d+ \[([^]]+)\]', line)
        if match and 'ENABLED' in match.group(2):
            edges[current].add(match.group(1))
    return (driver.group(1) if driver else None), (model.group(1) if model else None), entities, edges


def reachable(edges, starts):
    queue = deque(starts)
    visited = set(queue)
    while queue:
        for nxt in edges.get(queue.popleft(), ()):
            if nxt not in visited:
                visited.add(nxt)
                queue.append(nxt)
    return visited


def resolve(profile, topologies):
    if profile not in PROFILES:
        raise ValueError('unknown camera profile')
    wanted_driver, sensor = PROFILES[profile]
    candidates = []
    parsed = [(media, *graph(text)) for media, text in topologies]
    for media, driver, model, entities, edges in parsed:
        if driver != wanted_driver:
            continue
        starts = [name for name in entities if sensor in name.lower()]
        if profile == 'ov8858-isp' and not starts:
            # BSP represents CIF/SDITF and ISP as separate media graphs. The
            # ISP bridge entity must match the unique CIF media model, and
            # that CIF graph must have an enabled sensor-to-capture route.
            bridges = []
            for _, other_driver, other_model, upstream, upstream_edges in parsed:
                if other_driver != 'rkcif' or not other_model or other_model not in entities:
                    continue
                upstream_starts = [name for name in upstream if sensor in name.lower()]
                path = reachable(upstream_edges, upstream_starts)
                if 'rockchip-mipi-csi2' in path and 'stream_cif_mipi_id0' in path:
                    bridges.append(other_model)
            if len(bridges) == 1:
                starts = bridges
        visited = reachable(edges, starts)
        node = entities.get('rkisp_mainpath')
        if 'rkisp_mainpath' in visited and node:
            candidates.append((media, node))
    if len(candidates) != 1:
        raise ValueError('expected exactly one enabled sensor-to-ISP-mainpath graph; found %d' % len(candidates))
    return candidates[0]


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in PROFILES:
        print('usage: resolve_camera.py imx415-isp|ov8858-isp', file=sys.stderr)
        return 2
    topologies = []
    for media in sorted(glob.glob('/dev/media[0-9]*')):
        result = subprocess.run(['media-ctl', '-d', media, '-p'],
                                text=True, capture_output=True, timeout=10)
        if result.returncode:
            print('cannot query ' + media + ': ' + result.stderr.strip(), file=sys.stderr)
            return 1
        topologies.append((media, result.stdout))
    try:
        media, node = resolve(sys.argv[1], topologies)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print('%s: %s -> %s' % (sys.argv[1], media, node), file=sys.stderr)
    print(node)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
