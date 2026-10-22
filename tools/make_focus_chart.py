from PIL import Image, ImageDraw, ImageFont
import math

DPI = 300
W, H = int(8.27 * DPI), int(11.69 * DPI)      # A4
img = Image.new("L", (W, H), 255)
d = ImageDraw.Draw(img)

def font(sz):
    for p in [r"C:\Windows\Fonts\arialbd.ttf", r"C:\Windows\Fonts\arial.ttf"]:
        try:
            return ImageFont.truetype(p, sz)
        except Exception:
            pass
    return ImageFont.load_default()

# 外框 + 定位十字
m = 120
d.rectangle([m, m, W - m, H - m], outline=0, width=6)
d.line([W // 2 - 60, m, W // 2 - 60, m + 60], fill=0, width=4)
d.line([W // 2 + 60, m, W // 2 + 60, m + 60], fill=0, width=4)

# 标题
d.text((m + 20, m + 24), "FOCUS TEST CHART  /  RK3568 IMX415 4K", font=font(54), fill=0)
d.text((m + 20, m + 100), "print at 100% (A4, no scaling) - keep camera fixed", font=font(34), fill=0)

# 西门子星（中心）
cx, cy, R = W // 2, 1100, 430
for i in range(36):
    a0 = math.radians(i * 10)
    a1 = math.radians(i * 10 + 5)
    d.polygon([(cx, cy),
               (cx + R * math.cos(a0), cy + R * math.sin(a0)),
               (cx + R * math.cos(a1), cy + R * math.sin(a1))], fill=0)
d.ellipse([cx - R - 30, cy - R - 30, cx + R + 30, cy + R + 30], outline=0, width=6)

# 斜边方块（4 个方向，用于量 MTF）
def slant(x, y, s, flip):
    d.polygon([(x, y), (x + s, y), (x + s, y + s)] if flip else
              [(x, y), (x + s, y + s), (x, y + s)], fill=0)
bs = 200
for i, (bx, by) in enumerate([(m + 140, 1700), (W - m - 140 - bs, 1700),
                              (m + 140, 2400), (W - m - 140 - bs, 2400)]):
    slant(bx, by, bs, i % 2 == 0)
    d.text((bx - 6, by + bs + 14), f"slant {i+1}", font=font(28), fill=0)

# 线宽阶梯（分辨率检查）
y0 = 2760
d.text((m + 20, y0 - 60), "line pairs (px at 300dpi): 1 2 3 4 6 8", font=font(32), fill=0)
x = m + 30
for lw, gap in [(1, 3), (2, 4), (3, 5), (4, 6), (6, 8), (8, 10)]:
    for k in range(6):
        d.line([x, y0, x, y0 + 130], fill=0, width=lw)
        x += lw + gap
    x += 40
    d.text((x - 40, y0 + 140), str(lw), font=font(26), fill=0)
    x += 30

# 距离刻度文字
d.text((m + 20, y0 + 220), "distances to test: 0.3 m / 0.5 m / 1.0 m / 1.5 m / 2.0 m / 3.0 m",
       font=font(36), fill=0)
d.text((m + 20, y0 + 280), "focus at the distance your app actually uses", font=font(36), fill=0)

img.save(r"D:\vm\tmp_ov8858\focus_chart_A4_300dpi.png", dpi=(DPI, DPI))
print("saved focus_chart_A4_300dpi.png", img.size)
