# -*- coding: utf-8 -*-
u"""量「把 VOC 数据集中心裁成方形」的代价 —— **只读 XML，不碰图片**。

# ============================ 这个文件负责什么 ============================
负责：回答一个是非题 —— `shooting (2)` 这份数据集**能不能**靠「中心裁成方形」修好几何？
不负责：
  1. **不裁图、不写任何文件**（只解析 XML、算数、打印）；
  2. 不改数据集；
  3. 不判「模型好不好」—— 它只量几何。

# ⚠️ 为什么必须先量再裁
    正样本是 **416x234（16:9）**，中心裁成方形要**丢掉每侧 91 px**（宽度的 44%）。
    第一张图的框就是 `xmin=37, xmax=357`（**宽 320**）—— **比要裁的窗口（234）还宽**。
    如果大多数图都这样，裁出来的样本里**目标是被切掉两头的**，标签会贴到画面两边
    （框中心在窗内、框本身被钳到 0..234）⇒ 那是**把数据集弄坏**，不是修几何。

# ⚠️ 判据的口径（别自己发明）
    裁剪窗口 = 边长 S = min(W,H)、左上角 (x0,y0) = ((W-S)//2, (H-S)//2) ——
    与 `crop_center_240.py` 的 `plan_crop()` **同一套算法**，否则量的不是同一件事。
    框的保留策略也照抄它：**框中心**在窗内才留，留了但触边就钳制。

# ============================== 输出（纯 ASCII）==============================
    [sizes]            W x H 直方图
    [boxes]            框宽/框高的 min/median/max
    [crop S=234]       全量口径：keep / clipped / dropped / 丢空图 / 目标面积保留率
    [verdict]          一句话结论 + 判据
"""
import os
import sys
import xml.etree.ElementTree as ET
from collections import Counter


def parse_boxes(path):
    r = ET.parse(path).getroot()
    W = int(r.findtext("size/width"))
    H = int(r.findtext("size/height"))
    boxes = []
    for o in r.findall("object"):
        b = o.find("bndbox")
        boxes.append((
            float(b.findtext("xmin")), float(b.findtext("ymin")),
            float(b.findtext("xmax")), float(b.findtext("ymax")),
        ))
    return W, H, boxes


def pct(xs, q):
    if not xs:
        return 0.0
    xs = sorted(xs)
    i = min(len(xs) - 1, max(0, int(round(q * (len(xs) - 1)))))
    return xs[i]


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: analyze_voc_crop.py <dataset-root>\n")
        return 2
    root = argv[1]
    ann = os.path.join(root, "annotations")
    if not os.path.isdir(ann):
        sys.stderr.write("ERROR: no annotations/ under %s\n" % root)
        return 2

    rows = []
    for name in sorted(os.listdir(ann)):
        if name.lower().endswith(".xml"):
            rows.append((name,) + parse_boxes(os.path.join(ann, name)))
    if not rows:
        sys.stderr.write("ERROR: no .xml under %s\n" % ann)
        return 2

    # ---- 尺寸直方图
    sizes = Counter("%dx%d" % (W, H) for _, W, H, _ in rows)
    print("[sizes] %d xml files" % len(rows))
    for k, v in sizes.most_common():
        print("   %-10s x %d" % (k, v))

    # ---- 框宽/高
    bw = [x2 - x1 for _, _, _, bs in rows for (x1, _, x2, _) in bs]
    bh = [y2 - y1 for _, _, _, bs in rows for (_, y1, _, y2) in bs]
    print("[boxes] n=%d" % len(bw))
    for tag, xs in (("box_w", bw), ("box_h", bh)):
        print("   %-6s min=%.0f  p50=%.0f  p90=%.0f  max=%.0f"
              % (tag, min(xs), pct(xs, 0.5), pct(xs, 0.9), max(xs)))

    # ---- 按 crop_center_240.py 的口径实算
    keep = clip = drop = 0
    img_empty = 0
    area_before = area_after = 0.0
    box_wider_than_S = 0
    for _, W, H, bs in rows:
        S = min(W, H)
        x0, y0 = (W - S) // 2, (H - S) // 2
        n_keep = 0
        for (x1, y1, x2, y2) in bs:
            X, Y = (x1 + x2) / 2.0, (y1 + y2) / 2.0
            if not (x0 <= X < x0 + S and y0 <= Y < y0 + S):
                drop += 1
                continue
            n_keep += 1
            if (x2 - x1) > S:
                box_wider_than_S += 1
            l, r = max(x1 - x0, 0.0), min(x2 - x0, float(S))
            t, b = max(y1 - y0, 0.0), min(y2 - y0, float(S))
            touched = (x1 - x0 < 0) or (y1 - y0 < 0) or (x2 - x0 > S) or (y2 - y0 > S)
            keep += 0 if touched else 1
            clip += 1 if touched else 0
            area_before += (x2 - x1) * (y2 - y1)
            area_after += max(0.0, r - l) * max(0.0, b - t)
        if bs and n_keep == 0:
            img_empty += 1

    print("[crop S=min(W,H) center]  policy = box-center-inside, edge=clip")
    print("   boxes  keep=%d  clipped=%d  dropped=%d" % (keep, clip, drop))
    print("   boxes  wider than the crop window = %d  <-- target cannot fit"
          % box_wider_than_S)
    print("   images that would end up with ZERO boxes = %d" % img_empty)
    if area_before:
        print("   target AREA retained = %.1f%%" % (100.0 * area_after / area_before))
    print("   total boxes = %d" % (keep + clip + drop))

    # ---- 结论（判据写在这里，避免"看心情解读"）
    print("[verdict]")
    if keep + clip + drop == 0:
        print("   no boxes at all -> nothing to measure")
    elif keep + clip == 0:
        print("   ALL boxes dropped -> cropping destroys the dataset")
    else:
        intact = 100.0 * keep / (keep + clip + drop)
        print("   intact (untouched) boxes = %.1f%%" % intact)
        if box_wider_than_S:
            print("   WARN: %d box(es) are WIDER than the crop window -> "
                  "their left/right ends get cut off" % box_wider_than_S)
        if intact < 50.0:
            print("   => most boxes get sliced: center-crop is the WRONG fix here")
        else:
            print("   => most boxes survive intact: center-crop is viable")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
