# 板载编译(RK3568 上直接 gcc/g++)
RGA_INC   := /usr/include/rga
RKNN_INC  := /root/npu/3rdparty/rknpu2/include
CFLAGS    := -O2 -Wall
CXXFLAGS  := -O2 -Wall -I$(RGA_INC) -I$(RKNN_INC)
LDLIBS    := -lrga -lrknnrt -lpthread

BINS := zc_s1 zc_s23b zc_npu zc_pipe ystat npu_info capbench

all: $(BINS)

zc_s1: zc_s1.c
	$(CC) $(CFLAGS) -o $@ $<

ystat: ystat.c
	$(CC) $(CFLAGS) -o $@ $<

capbench: capbench.c
	$(CC) $(CFLAGS) -o $@ $<

npu_info: npu_info.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lrknnrt

zc_s23b: zc_s23b.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lrga

zc_npu: zc_npu.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

zc_pipe: zc_pipe.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BINS)
.PHONY: all clean
