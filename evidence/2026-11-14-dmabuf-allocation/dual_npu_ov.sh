#!/bin/sh
exec strace -D -f -yy -e trace=ioctl -P /dev/rga -o /root/zc-allocation-20261114-9juwLl/dual-npu-rga-ov.trace /root/zc-allocation-20261114-9juwLl/candidate-v2/evrec "$@"
