# rk3568-camera-driver — 顶层 Makefile
#
# 用途: 在 RK3568 板上一键编译本仓库的板端工具。
# 内核侧 driver/ov8858_sm 在本 BSP 是内建(CONFIG_VIDEO_OV8858_SM=y),改动必须
# 重编 Image 刷 /boot —— 交叉编译流程见 tools/README.md,不在本文件覆盖范围。
#
# 常用:
#   make              # 全部板端目标(自动探测 RGA/RKNN,缺了会降级并提示)
#   make tools        # 只编纯 glibc 工具(任何 aarch64 板都能过)
#   make apps         # evrec / camd(需 librockchip_mpp + RGA/RKNN;
#                     #   cam_rtsp 已删除(docs/38 H6), 产品入口=evrec --rtsp)
#   make spy          # ioctlspy(LD_PRELOAD 注入库,-shared)
#   make clean
#
# 例外(本 Makefile 有意不编,见 tools/README.md):
#   compat_test / hdr_cfg —— 需要 vendor UAPI 头(linux/rk-camera-module.h 等),
#   设计为 VM 交叉编译(32/64 两版),流程在 tools/README。
#
# 可覆盖: make RKNN_INC=/path/to/rknn/include

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
PTH     := -pthread

# ---- 能力探测: 头文件在才定义宏, 源码 #ifdef 自动降级 ----
RGA_INC  ?= /usr/include/rga
RKNN_INC ?= /root/npu/3rdparty/rknpu2/include

HAVE_RGA  := $(shell test -f $(RGA_INC)/im2d.h      && echo 1)
HAVE_RKNN := $(shell test -f $(RKNN_INC)/rknn_api.h && echo 1)

DEFS := -DHAVE_RGA -DHAVE_RKNN   # apps 组专用; tools 组不依赖
INCS := -I$(RGA_INC) -I$(RKNN_INC)
APP_LIBS := -lrockchip_mpp -lrga -lrknnrt -lm

ifndef HAVE_RGA
$(warning RGA 头未找到: apps 将编译失败(可 make tools 先行))
endif
ifndef HAVE_RKNN
$(warning RKNN 头未找到($(RKNN_INC)): 可 make RKNN_INC=... 指定)
endif

# ---- 板端纯 glibc 工具 ----
TOOLS := reg_switch fi_probe fi_test streamoff_test tryfmt_test \
         focus_score st_burn

APP_BIN := evrec camd

.PHONY: all tools apps spy clean

all: tools apps spy

tools: $(TOOLS) hdr_demo

apps: $(APP_BIN)

spy: ioctlspy.so

# ---- 规则 ----
reg_switch: tools/regression/reg_switch.c
	$(CC) $(CFLAGS) $(PTH) -o $@ $< -lm

fi_probe fi_test streamoff_test tryfmt_test focus_score st_burn: %: tools/%.c
	$(CC) $(CFLAGS) $(PTH) -o $@ $< -lm

hdr_demo: tools/hdr_demo.c
	$(CC) $(CFLAGS) -std=gnu99 -o $@ $< -lm

evrec: tools/evrec.c tools/rtsp_hub.c
	$(CC) $(CFLAGS) $(PTH) $(DEFS) $(INCS) -o $@ tools/evrec.c tools/rtsp_hub.c $(APP_LIBS)

camd: tools/camd.c
	$(CC) $(CFLAGS) $(PTH) $(DEFS) $(INCS) -o $@ $< $(APP_LIBS)

# LD_PRELOAD 注入库(无 main)
ioctlspy.so: tools/ioctlspy.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

clean:
	rm -f $(TOOLS) $(APP_BIN) hdr_demo ioctlspy.so
