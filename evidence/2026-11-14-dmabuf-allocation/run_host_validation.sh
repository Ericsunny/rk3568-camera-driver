#!/bin/sh
set -eu
sha256sum tools/evrec.c tools/run_camera_demo.sh tools/tests/test_evrec_dmabuf.py
sh -n tools/run_camera_demo.sh
python3 tools/tests/test_evrec_dmabuf.py
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 tools/tests/test_evrec_dmabuf.py
python3 tools/tests/test_camera_demo_profile.py
python3 tools/tests/test_evrec_host.py
python3 tools/tests/test_recovery_rtsp_contract.py
python3 tools/tests/test_evrec_ring_bounds.py
python3 tools/tests/test_evrec_nal_start.py
python3 tools/tests/test_evrec_preroll.py
