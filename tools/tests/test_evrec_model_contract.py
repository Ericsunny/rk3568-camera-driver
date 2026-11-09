#!/usr/bin/env python3
"""Extract actual geometry/postprocess functions; synthetic tensors, no NPU."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1]/'evrec.c').read_text(encoding='utf-8')
def function(signature):
    start=source.index(signature); opening=source.index('{',start); depth=0
    for i in range(opening,len(source)):
        if source[i]=='{': depth+=1
        elif source[i]=='}':
            depth-=1
            if not depth: return source[start:i+1]+'\n'
    raise ValueError(signature)

prelude=r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct { int dims[4], zp; float scale; } rknn_tensor_attr;
typedef struct { unsigned index; int want_float, is_prealloc; void *buf; unsigned size; } rknn_output;
struct dbox { float x1,y1,x2,y2,score; int cls; };
#define NMS_THRESH 0.45f
static float g_obj_thresh=0.35f;
static int g_no_obj_mul=1;
'''
test=r'''
static void require(int ok) { if (!ok) exit(1); }
int main(void) {
    int rw,rh,i,s,k,n;
    unsigned char rgb[4*4*3];
    struct dbox result[8];float peak;
    int8_t reg[3][64], cls[3][80], sum[3][1], *data[9];
    rknn_tensor_attr attr[9];
    rknn_output outputs[9];
    memset(outputs,255,sizeof(outputs));prepare_npu_outputs(outputs,9);
    for(i=0;i<9;i++)require(outputs[i].index==(unsigned)i && !outputs[i].want_float && !outputs[i].is_prealloc && !outputs[i].buf);
    letterbox_size(1920,1080,640,640,&rw,&rh);require(rw==640 && rh==360);
    letterbox_size(1080,1920,640,640,&rw,&rh);require(rw==360 && rh==640);
    memset(rgb,0,sizeof(rgb));memset(rgb,10,12);memset(rgb+12,20,12);
    pad_rgb(rgb,4,4,4,2);
    for(i=0;i<12;i++)require(rgb[i]==114 && rgb[12+i]==10 && rgb[24+i]==20 && rgb[36+i]==114);
    memset(attr,0,sizeof(attr));memset(cls,0,sizeof(cls));memset(sum,10,sizeof(sum));
    memset(reg,-100,sizeof(reg));
    for(s=0;s<3;s++) {
        for(k=0;k<4;k++)reg[s][k*16+(s==0?2:1)]=100;
        for(k=0;k<3;k++) {attr[s*3+k].dims[2]=attr[s*3+k].dims[3]=1;attr[s*3+k].scale=k?0.01f:1.f;}
        data[s*3]=reg[s];data[s*3+1]=cls[s];data[s*3+2]=sum[s];
    }
    cls[0][0]=80;
    n=decode_yolov8(attr,data,result,8,&peak);require(n==1 && fabsf(result[0].score-.8f)<.001f);
    /* score_sum=.1 is not multiplied into class confidence=.8. */
    cls[1][56]=90;
    n=decode_yolov8(attr,data,result,8,&peak);require(n==2); /* different classes survive overlap */
    cls[1][56]=0;cls[1][0]=90;
    n=decode_yolov8(attr,data,result,8,&peak);require(n==1 && result[0].cls==0 && result[0].score>.89f);
    puts("letterbox, RGB padding, class confidence and class-aware NMS: PASS");
    return 0;
}
'''
code=prelude+''.join(function(s) for s in ['static void letterbox_size(', 'static void pad_rgb(',
     'static float box_iou(', 'static int cmp_box(', 'static int decode_yolov8(', 'static void prepare_npu_outputs('])+test
with tempfile.TemporaryDirectory(prefix='model-contract-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
