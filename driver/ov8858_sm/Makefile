obj-m += ov8858_sm.o

# The cross toolchain is auto-detected under the SDK prebuilts dir,
# so the long path never has to be typed by hand.
SDK    ?= $(HOME)/rk3568/RK_SDK
PREDIR := $(SDK)/prebuilts/gcc/linux-x86/aarch64
TOOLCHAIN ?= $(firstword $(wildcard $(PREDIR)/gcc-buildroot-*/))

ifeq ($(strip $(TOOLCHAIN)),)
$(error Cannot find toolchain under $(PREDIR) - pass TOOLCHAIN=/path/to/toolchain)
endif

TOOLCHAIN := $(patsubst %/,%,$(TOOLCHAIN))/

KDIR := $(SDK)/kernel
ARCH := arm64
CROSS_COMPILE := $(TOOLCHAIN)bin/aarch64-rockchip-linux-gnu-
export LD_LIBRARY_PATH := $(TOOLCHAIN)lib

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean