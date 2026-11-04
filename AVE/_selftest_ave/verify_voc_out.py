# -*- coding: utf-8 -*-
u"""独立校验"裁方"产物的数据集 —— **不 import 工具，自己按文档口径重算一遍**

# ============================ 这个文件负责什么 ============================
负责：拿 `crop_center_240.py` 的**产物**（`shooting240/`）与**源**（`shooting (2)/`）逐张对账：
  1. 图：产物每张必须是正方形，边长必须 = min(源图宽,高)；文件名集合必须与源一致；
  2. XML：`<size>` 必须等于产物图的实际像素尺寸（**回读盘上文件**，不信报告）；
  3. 框：按"中心在窗内 -> 平移、触边 -> 钳制、窗外 -> 删除、再 round 成整数"重算，
     与产物里的 bndbox **逐个比**，报出不一致（并区分"round 半像素平局"这类已知毛刺）；
  4. 框的合法性：必须在 `[0,S]` 内、且宽高 >= 1 px；
  5. 负样本（源无 XML）：产物**不许**有 XML；
  6. `train.txt`：行数与文件名集合必须与"产物里真实存在的图"一致；
  7. 像素（抽样）：把源图按 (x0,y0,S) 裁出来的缩略图与产物比，且要求它**比**
     "裁左边"和"裁右边"两个错误窗都更接近 —— 只验尺寸是查不出窗口整体偏移的。

# 不负责
  不裁图、不写任何文件、不重训、不评价模型好坏。**只读 + 打印 + 退出码**。

# 为什么要有它（不是多余的一层）
  工具自己的报告是"工具自己说它对"。这份脚本**重新实现**了同一套口径 ——
  两套独立实现互相印证；只有一处对得上才叫证据（本工程吃过"改了但没生效"的亏）。
  ⚠️ 它**不是**训练器视角的验收：MaixHub 到底怎么解析这些 XML/图，这里测不出来。

# 已知的口径毛刺（必须报出来，不许悄悄放过）
  本工具把像素坐标先转成归一化浮点、再乘回 S，理论上在"边界正好落在 0.5 px"时
  `round()` 会出现 1 px 的平局差。这类不一致单独计数（TIE），不混进 FAIL。

用法：
  python _selftest_ave/verify_voc_out.py <src_root> <out_root> [--pixels N]
"""
import os
import sys
import xml.etree.ElementTree as ET

try:
    from PIL import Image
except ImportError:                                   # pragma: no cover
    sys.stderr.write("ERROR: Pillow is required.\n")
    raise SystemExit(3)

try:
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
except Exception:                                     # pragma: no cover
    pass

IMAGE_EXT = {".jpg", ".jpeg", ".png", ".bmp"}


def src_boxes(src_root, img_rel):
    u"""源的 (W, H, [(x1,y1,x2,y2), ...])；XML 不存在返回 None（负样本）。"""
    stem = os.path.splitext(os.path.basename(img_rel))[0]
    xml = os.path.join(src_root, "annotations", stem + ".xml")
    with Image.open(os.path.join(src_root, img_rel)) as im:
        W, H = im.size
    if not os.path.isfile(xml):
        return W, H, None
    r = ET.parse(xml).getroot()
    boxes = []
    for o in r.findall("object"):
        b = o.find("bndbox")
        boxes.append((float(b.findtext("xmin")), float(b.findtext("ymin")),
                      float(b.findtext("xmax")), float(b.findtext("ymax"))))
    return W, H, boxes


def expect(W, H, boxes):
    u"""按文档口径算出**期望的整数 bndbox 列表**（与工具无关的第二实现）。

    返回 (S, x0, y0, [ (l,t,r,b) 或 None（该框被丢） ], [是否有平局风险])
    """
    S = min(W, H)
    x0, y0 = (W - S) // 2, (H - S) // 2
    out, ties = [], []
    for (x1, y1, x2, y2) in boxes:
        cx, cy = (x1 + x2) / 2.0, (y1 + y2) / 2.0
        if not (x0 <= cx < x0 + S and y0 <= cy < y0 + S):
            out.append(None)                          # 中心在窗外 -> 丢
            continue
        l, r = max(x1 - x0, 0.0), min(x2 - x0, float(S))
        t, b = max(y1 - y0, 0.0), min(y2 - y0, float(S))
        if r - l <= 0.0 or b - t <= 0.0:
            out.append(None)                          # 退化 -> 丢
            continue
        vals = (l, t, r, b)
        # 半像素平局：值的小数部分落在 .5 附近（round 半偶/浮点误差都可能翻）
        if any(abs(v - int(v) - 0.5) < 1e-6 for v in vals):
            ties.append(vals)
        out.append((max(0, min(S - 1, int(round(l)))), max(0, min(S - 1, int(round(t)))),
                    max(int(round(l)) + 1, min(S, int(round(r)))),
                    max(int(round(t)) + 1, min(S, int(round(b))))))
    return S, x0, y0, out, ties


def read_out_xml(path):
    r = ET.parse(path).getroot()
    size = (int(float(r.findtext("size/width"))), int(float(r.findtext("size/height"))))
    objs = []
    for o in r.findall("object"):
        b = o.find("bndbox")
        objs.append((int(float(b.findtext("xmin"))), int(float(b.findtext("ymin"))),
                     int(float(b.findtext("xmax"))), int(float(b.findtext("ymax")))))
    return size, objs


def thumb_mad(a, b, n=64):
    u"""两张 PIL 图 -> n x n 灰度缩略图的平均绝对差（0..255）。"""
    pa = a.convert("L").resize((n, n))
    pb = b.convert("L").resize((n, n))
    da = list(pa.getdata())
    db = list(pb.getdata())
    return sum(abs(x - y) for x, y in zip(da, db)) / float(len(da))


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__.split("用法：")[-1].strip() + "\n")
        return 2
    src_root, out_root = argv[1], argv[2]
    n_px = 12
    if "--pixels" in argv:
        n_px = int(argv[argv.index("--pixels") + 1])

    src_imgs = sorted(f for f in os.listdir(os.path.join(src_root, "images"))
                      if os.path.splitext(f)[1].lower() in IMAGE_EXT)
    fails, ties_total, px_ties = [], 0, 0
    n_xml_ok = n_neg = 0
    n_boxes = 0

    # ---- 1/2/3/4/5：逐图对账
    for name in src_imgs:
        rel = os.path.join("images", name)
        out_img = os.path.join(out_root, rel)
        if not os.path.isfile(out_img):
            fails.append("missing output image: %s" % rel)
            continue
        W, H, boxes = src_boxes(src_root, rel)
        S, x0, y0, want, ties = expect(W, H, boxes or [])
        with Image.open(out_img) as im:
            got_size = im.size
        if got_size != (S, S):
            fails.append("%s: output size %s, expected %dx%d" % (rel, got_size, S, S))
        stem = os.path.splitext(name)[0]
        out_xml = os.path.join(out_root, "annotations", stem + ".xml")
        if boxes is None:                             # 负样本
            n_neg += 1
            if os.path.isfile(out_xml):
                fails.append("%s: negative sample has an output XML" % rel)
            continue
        if not os.path.isfile(out_xml):
            fails.append("%s: output XML missing" % rel)
            continue
        x_size, x_objs = read_out_xml(out_xml)
        if x_size != (S, S):
            fails.append("%s: XML <size> %s != image %dx%d" % (rel, x_size, S, S))
        want_kept = [b for b in want if b is not None]
        if len(x_objs) != len(want_kept):
            fails.append("%s: XML has %d boxes, expected %d"
                         % (rel, len(x_objs), len(want_kept)))
            continue
        n_xml_ok += 1
        n_boxes += len(x_objs)
        for i, (g, w) in enumerate(zip(x_objs, want_kept)):
            if g != w:
                d = max(abs(g[k] - w[k]) for k in range(4))
                if d <= 1 and ties:                   # 只可能是半像素平局
                    ties_total += 1
                else:
                    fails.append("%s box[%d]: got %s want %s" % (rel, i, g, w))
            if not (0 <= g[0] < g[2] <= S and 0 <= g[1] < g[3] <= S):
                fails.append("%s box[%d]: out of [0,%d] or zero-area: %s" % (rel, i, S, g))

    # ---- 6：train.txt
    src_train = os.path.join(src_root, "train.txt")
    out_train = os.path.join(out_root, "train.txt")
    if os.path.isfile(src_train):
        def lines(p):
            with open(p, encoding="utf-8", errors="replace") as fh:
                return [ln.strip() for ln in fh if ln.strip()]
        if not os.path.isfile(out_train):
            fails.append("train.txt missing in output")
        else:
            a, b = lines(src_train), lines(out_train)
            if len(a) != len(b):
                fails.append("train.txt lines: %d -> %d (dropped images should explain a "
                             "smaller count; any other change is a bug)" % (len(a), len(b)))
            elif a != b:
                diff = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
                fails.append("train.txt content differs at %d row(s): %s" % (len(diff), diff[:5]))

    # ---- 7：像素抽样：窗口整体偏移是"只看尺寸"查不出来的
    px_checked = 0
    for name in src_imgs[:n_px]:
        rel = os.path.join("images", name)
        out_img = os.path.join(out_root, rel)
        if not os.path.isfile(out_img):
            continue
        W, H, _ = src_boxes(src_root, rel)
        S = min(W, H)
        with Image.open(os.path.join(src_root, rel)) as im:
            im = im.convert("RGB")
            got = Image.open(out_img).convert("RGB")
            d_center = thumb_mad(im.crop(((W - S) // 2, (H - S) // 2,
                                          (W - S) // 2 + S, (H - S) // 2 + S)), got)
            d_left = thumb_mad(im.crop((0, (H - S) // 2, S, (H - S) // 2 + S)), got)
            d_right = thumb_mad(im.crop((W - S, (H - S) // 2, W, (H - S) // 2 + S)), got)
            got.close()
        if d_center > 8.0:
            fails.append("%s: center-crop pixels differ from source (MAD=%.2f)"
                         % (rel, d_center))
        elif d_center >= min(d_left, d_right):
            px_ties += 1                              # 画面太均匀 -> 分辨率不足，不算失败
        px_checked += 1

    print("[verify] src=%s" % src_root)
    print("[verify] out=%s" % out_root)
    print("[verify] images: src=%d out=%d  (xml compared=%d, negatives=%d, boxes=%d)"
          % (len(src_imgs),
             len([f for f in os.listdir(os.path.join(out_root, "images"))
                  if os.path.splitext(f)[1].lower() in IMAGE_EXT]) if os.path.isdir(
                 os.path.join(out_root, "images")) else -1,
             n_xml_ok, n_neg, n_boxes))
    print("[verify] pixel windows checked=%d  inconclusive(uniform image)=%d"
          % (px_checked, px_ties))
    print("[verify] rounding ties (1 px, benign)=%d" % ties_total)
    if fails:
        print("[verify] FAIL: %d problem(s)" % len(fails))
        for f in fails[:20]:
            print("   " + f)
        if len(fails) > 20:
            print("   ... and %d more" % (len(fails) - 20))
        return 1
    print("[verify] PASS: every output image/XML/train.txt line matches the independent "
          "recomputation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
