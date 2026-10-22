#!/usr/bin/env python3
"""HDR 多帧融合（路线 A 的最小实现）

输入 2~3 帧同场景、不同曝光的 NV12 文件，输出一帧融合后的 NV12。
做法：线性化 -> 按"曝光适中度 × 局部对比度"加权 -> 加权平均 -> 回 gamma。
Y 融合，UV 取中间那帧（避免彩噪与偏色跳变）。

用法:
    python3 hdr_fuse.py --in dim.nv12 mid.nv12 bright.nv12 --out fused.nv12 \
                        --size 3840x2160 [--png fused.png]

依赖: numpy (Pillow 仅 --png 需要)
"""
import argparse
import numpy as np

GAMMA = 2.2
SIGMA = 0.22          # 曝光适中度的高斯宽度（以 0..1 归一化亮度为准）
BOX_R = 8             # 权重平滑半径（像素）


def parse_size(s):
    w, h = s.lower().split("x")
    return int(w), int(h)


def load_nv12(path, w, h):
    y_size = w * h
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size < y_size + w * h // 2:
        raise ValueError(f"{path}: 期望 {y_size + w * h // 2} 字节，实际 {raw.size}")
    y = raw[:y_size].reshape(h, w)
    uv = raw[y_size:y_size + w * h // 2].reshape(h // 2, w // 2, 2)
    return y, uv


def well_exposed(y):
    return np.exp(-((y.astype(np.float32) / 255.0 - 0.5) ** 2) / (2 * SIGMA ** 2))


def local_contrast(y):
    yy = y.astype(np.float32)
    lap = np.abs(4 * yy[1:-1, 1:-1] - yy[:-2, 1:-1] - yy[2:, 1:-1]
                 - yy[1:-1, :-2] - yy[1:-1, 2:])
    out = np.zeros_like(yy)
    out[1:-1, 1:-1] = lap
    return out


def box_blur(a, r=BOX_R):
    k = 2 * r + 1
    c = np.cumsum(np.pad(a, ((r + 1, r), (0, 0)), mode="edge"), axis=0)
    a2 = (c[k:] - c[:-k]) / k
    c = np.cumsum(np.pad(a2, ((0, 0), (r + 1, r)), mode="edge"), axis=1)
    return (c[:, k:] - c[:, :-k]) / k


def stats(y, name=""):
    return {
        "frame": name,
        "meanY": round(float(y.mean()), 2),
        "p50": float(np.percentile(y, 50)),
        "clip_pct": round(float((y >= 250).mean() * 100), 2),
        "dark_pct": round(float((y < 16).mean() * 100), 2),
    }


def fuse(ys):
    lin, wts = [], []
    for y in ys:
        lin.append((y.astype(np.float32) / 255.0) ** GAMMA)
        w = (well_exposed(y) + 1e-3) * (1.0 + 0.5 * (local_contrast(y) / 64.0))
        wts.append(box_blur(w))
    wsum = sum(wts)
    fused_lin = sum(w * l for w, l in zip(wts, lin)) / wsum
    return np.clip((fused_lin ** (1 / GAMMA)) * 255.0, 0, 255).astype(np.uint8)


def nv12_to_rgb(y, uv):
    """limited-range BT.601，够用即可（不做色彩管理）"""
    yf = y.astype(np.float32) - 16
    u = np.repeat(np.repeat(uv[:, :, 0].astype(np.float32), 2, 0), 2, 1) - 128
    v = np.repeat(np.repeat(uv[:, :, 1].astype(np.float32), 2, 0), 2, 1) - 128
    r = 1.164 * yf + 1.596 * v
    g = 1.164 * yf - 0.392 * u - 0.813 * v
    b = 1.164 * yf + 2.017 * u
    return np.clip(np.dstack([r, g, b]), 0, 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inputs", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", default="3840x2160")
    ap.add_argument("--png", default=None)
    args = ap.parse_args()

    w, h = parse_size(args.size)
    frames = [load_nv12(p, w, h) for p in args.inputs]
    ys = [f[0] for f in frames]
    uv_pick = frames[len(frames) // 2][1]

    for p, y in zip(args.inputs, ys):
        print(stats(y, p))
    fused = fuse(ys)
    print(stats(fused, "fused"))

    with open(args.out, "wb") as f:
        f.write(fused.tobytes())
        f.write(uv_pick.tobytes())
    print("written", args.out)

    if args.png:
        from PIL import Image
        Image.fromarray(nv12_to_rgb(fused, uv_pick)).save(args.png)
        print("written", args.png)


if __name__ == "__main__":
    main()
