#!/usr/bin/env python3
"""Graph-selection checks with synthetic media-ctl output; no board needed."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from resolve_camera import resolve

graph = '''driver          rkisp-vir0
- entity 1: m00_b_imx415 1-001a (1 pad, 1 link)
    pad0: Source
        -> "dphy":0 [ENABLED]
- entity 2: dphy (2 pads, 2 links)
    pad1: Source
        -> "rkisp-isp-subdev":0 [ENABLED]
- entity 3: rkisp-isp-subdev (2 pads, 2 links)
    pad1: Source
        -> "rkisp_mainpath":0 [ENABLED]
- entity 4: rkisp_mainpath (1 pad, 1 link)
    device node name /dev/video5
'''
assert resolve('imx415-isp', [('/dev/media1', graph)]) == ('/dev/media1', '/dev/video5')
cif = '''driver rkcif
model rkcif_mipi_lvds
- entity 1: m01_b_ov8858-sm 5-0036 (1 pad, 1 link)
    -> "rockchip-mipi-csi2":0 [ENABLED]
- entity 2: rockchip-mipi-csi2 (2 pads, 1 link)
    -> "stream_cif_mipi_id0":0 [ENABLED]
- entity 3: stream_cif_mipi_id0 (1 pad, 1 link)
'''
isp = '''driver rkisp-vir1
model rkisp1
- entity 1: rkcif_mipi_lvds (1 pad, 1 link)
    -> "rkisp-isp-subdev":0 [ENABLED]
- entity 2: rkisp-isp-subdev (2 pads, 1 link)
    -> "rkisp_mainpath":0 [ENABLED]
- entity 3: rkisp_mainpath (1 pad, 1 link)
    device node name /dev/video14
'''
assert resolve('ov8858-isp', [('/dev/media0', cif), ('/dev/media2', isp)]) == ('/dev/media2', '/dev/video14')
for profile, inputs in [
    ('imx415-isp', [('/dev/media1', graph.replace('[ENABLED]', '[]', 1))]),
    ('imx415-isp', [('/dev/media1', graph), ('/dev/media2', graph)]),
    ('ov8858-isp', [('/dev/media1', graph)]),
    ('ov8858-isp', [('/dev/media0', cif.replace('[ENABLED]', '[]', 1)), ('/dev/media2', isp)]),
]:
    try:
        resolve(profile, inputs)
    except ValueError:
        continue
    raise AssertionError('unconnected or ambiguous camera accepted: ' + profile)
print('PASS enabled sensor-to-mainpath graph selection and rejection')
