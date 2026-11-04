# -*- coding: utf-8 -*-
u"""数据集中心裁方（方案 ⓑ）—— 一次性几何变换工具，**不进产品链路**

# ============================ 这个文件负责什么 ============================
负责：
  把一份数据集的**每张图中心裁成正方形**，并把标注**跟着搬**到新坐标系。
  **两种格式都认**（按 `<src>/annotations` 目录是否存在自动判，见 `dataset_mode()`）：
    A. VOC / MaixHub 检测集 —— `images/*.jpg` + `annotations/*.xml`（像素坐标 bndbox）
       裁完**重写 XML**（新 `<size>` + 平移/截断后的 `bndbox`）+ **重生成 `train.txt`**；
    B. YOLO —— `images/` + `labels/*.txt`（归一化框），裁完写新的 `.txt`。
  默认边长 = min(w,h)：416x234 的图裁成 234x234（左右各去 91 px），
  320x240 的图裁成 240x240。两者都是"整幅高度 x 整幅高度"的方窗 —— 视场一致。

  为什么偏偏要"正方形"（`verify.md` 第五十二次）：
    训练图 = 采集的 320x240（4:3），MaixHub 把它塞进 224x224 的模型输入；
    推理图 = 相机**直接**给 224x224，而相机是**中心裁**出来的（不是压扁，实测见
    `_maixcam/_iso_check.py`）⇒ 两边视场根本不是一个东西。
  把训练图也裁成 1:1 之后，"4:3 → 1:1"这一步的**宽高比歧义彻底消失**：
  方形喂进方形，**不管 MaixHub 走的是压扁还是 letterbox，结果都一样**。
  而"320x240 进 224x224 时 MaixHub 究竟压扁还是补灰边"我们**至今不知道**
  （`verify.md` 第十次收尾）—— 这正是本方案比"改推理端去迁就训练端"更稳的地方。

不负责：
  1. **不重训、不评估模型、不发串口、不碰 MaixCAM** —— 这里只做文件几何变换。
     混进来就分不清"不准"是几何的锅还是模型的锅（前几轮返工过的坑）。
  2. 不判断"裁得对不对" —— 相机到底裁多少，是 `_maixcam/_iso_check.py` 的判据。
  3. 不改任何产品 `.c` / `.h`。
  4. **不删源文件**（源目录只读）。

# ============================== 硬约束（不许破坏）==============================
  1. `--dst` 必须 ≠ `--src`；`--dst` 已存在且非空时必须显式 `--force`。
     **绝不覆盖源目录** —— 数据集重新采集一次要花掉一个人的半天。
  2. **绝不静默跳过**：标注行解析失败 / 图片打不开 / 数值越界 ⇒ 打印 `文件:行`
     并**非零退出**。"静默跳过"会把一个坏样本悄悄留在训练集里，而唯一的症状只是
     "训出来不太准" —— 本工程为这类错返工过不止一次。
  3. 裁剪是**全局系统误差**：差一个像素，所有框一起错，而且**不报任何错**。
     因此参数写死并原样回显进报告，报告随数据集一起走。
  4. 报告必须落盘：**没有报告 = 没跑过**（沿用 `AVE` 工程"丢弃必须可观测"的习惯）。
  5. 控制台输出**只允许 ASCII** —— GBK 控制台打不出特殊符号，会当场抛异常。

# ============================== 框的取舍（用户 2026-10-12 裁决）==================
  框中心落在裁剪区内 → 保留；落在区外 → **丢弃**并计数。
  （理由：推理时那块位置本来就看不见，留着等于教模型去认它永远看不到的东西。）
  保留但**越出裁剪边界**的框 → 按边界**截断**，单独计数。
  （不截断的话归一化坐标会 > 1，多数训练器要么报错要么悄悄夹 —— 两种都不好。）

# ============================== VOC XML 的四条硬规定 ==========================
  1. **绝不原样拷贝 XML**。XML 里是**像素坐标**：图裁了、坐标不重写 = **标注全错**，
     而且错得"看起来很对"（框仍在画面里，只是整体偏了，肉眼扫一遍根本发现不了）。
     旧版工具正是把 XML 当普通文件拷贝的（实测 `files_copied = 247`）——这是本工具
     最容易犯、后果最隐蔽的错，所以自测里专门有一条变异 M5 盯着它。
  2. **bndbox 写成整数**（先 round 再钳到 `[0,S]`）。本数据集原始 XML 就是整数；
     万一 MaixHub 的解析器用 `int()` 读，写小数会**当场抛异常**。整数对 `int()` /
     `float()` 两种读法都安全 —— 这是唯一一种"两边都不炸"的写法。
  3. `<name>` / `<truncated>` / `<difficult>` / `<occluded>` **一字不改**。
     被裁掉两端的框**不**改成 `truncated=1`：万一 MaixHub 拿它做过滤，改了就等于
     **悄悄丢样本**；不改最坏只是标注保守。风险不对称，所以不改 ——
     截断的**数量**写进报告，信息不会丢。
  4. `train.txt` **筛选后重生成**（不是拷贝）。被裁掉的图必须从清单里消失，否则
     清单里留着一个不存在的文件 —— 训练器要么报错、要么**静默少一张**（本工程最怕
     后者）。行文本**原样保留**（有的清单写 `a.jpg`，有的写 `images/a.jpg`），只筛选。

# ============================== 用法 ==============================
  # ① 先 --dry-run：只读 + 出报告，**一个字节都不写**。这一步回答"裁了会丢多少框"
  python _selftest_ave/crop_center_240.py --src <数据集根> --dst <输出根> --dry-run
  # ② 拿 20 张真写一遍，人工扫一眼产物的 XML（几乎零成本，能挡住"整份裁错"）
  python _selftest_ave/crop_center_240.py --src <数据集根> --dst <输出根> --limit 20
  # ③ 全量
  python _selftest_ave/crop_center_240.py --src <数据集根> --dst <输出根>
  # 自测（不需要真数据集，自己造）
  python _selftest_ave/test_crop_center_240.py
"""

from __future__ import print_function

import argparse
import datetime
import os
import sys
import xml.etree.ElementTree as ET

try:
    from PIL import Image
except ImportError:                                   # pragma: no cover
    sys.stderr.write(
        "ERROR: Pillow is required.\n"
        "  python -m pip install -i https://pypi.org/simple pillow\n"
        "  (note: this box's pip defaults to a Tsinghua mirror that is NOT reachable;\n"
        "   the explicit -i above is required)\n")
    raise SystemExit(3)

# 控制台是 GBK/CP936：任何非 ASCII 写不出去会直接抛 UnicodeEncodeError。
# 用 replace 兜底，别让"打印失败"看起来像"工具崩了"。
try:
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
except Exception:                                     # pragma: no cover
    pass

IMAGE_EXT = {".jpg", ".jpeg", ".png", ".bmp"}
JPEG_EXT = {".jpg", ".jpeg"}
EXIF_ORIENTATION_TAG = 274

DEFAULT_SIZE = 0        # 0 = auto = min(w,h)：416x234 的图 -> 234，320x240 的图 -> 240
DEFAULT_QUALITY = 95


# --------------------------------------------------------------------------- #
# 几何
# --------------------------------------------------------------------------- #
def plan_crop(w, h, size):
    u"""算出裁剪窗口。返回 (S, x0, y0)。

    参数：w/h = 原图宽高；size = 目标边长，0 表示 auto。
    返回：S = 边长，x0/y0 = 窗口左上角在原图中的坐标。
    坑：auto 取 min(w,h) 而不是写死 240 —— 这样对 224x224 的图是**恒等变换**
        （no-op），对 320x240 才等于"左右各去 40"。写死 240 会让非 320x240 的图
        直接撞上断言，而数据集中混进几张别的尺寸是常事。
    """
    S = int(size) if size and size > 0 else min(w, h)
    if S <= 0 or S > min(w, h):
        raise ValueError("bad crop size %d for image %dx%d" % (S, w, h))
    return S, (w - S) // 2, (h - S) // 2


def remap_box(cx, cy, bw, bh, W, H, S, x0, y0):
    u"""把**归一化**的框搬到裁剪后的坐标系。

    参数：cx/cy/bw/bh = 原图归一化框（YOLO 约定：中心 + 宽高，全部 0~1）；
          W/H = 原图像素宽高；S/x0/y0 = `plan_crop()` 的输出。
    返回：(新框 4 元组, 状态) —— 状态是 'keep' / 'clipped' / 'outside' / 'degenerate'；
          状态为后两者时新框是 None。
    坑：先判"中心在不在窗口里"再算截断。顺序反了的话，一个中心在窗外、但身子
        伸进窗内的框会被截成一小条留在训练集里 —— 那种样本学出来的是"半个目标
        也算目标"，推理时边上任何东西都会报警。
    """
    X, Y = cx * W, cy * H
    BW, BH = bw * W, bh * H

    if not (x0 <= X < x0 + S and y0 <= Y < y0 + S):
        return None, "outside"

    left = X - x0 - BW / 2.0
    right = X - x0 + BW / 2.0
    top = Y - y0 - BH / 2.0
    bottom = Y - y0 + BH / 2.0

    clipped = (left < 0.0) or (top < 0.0) or (right > S) or (bottom > S)
    left, right = max(left, 0.0), min(right, float(S))
    top, bottom = max(top, 0.0), min(bottom, float(S))

    if right - left <= 0.0 or bottom - top <= 0.0:
        return None, "degenerate"

    return ((left + right) / 2.0 / S,
            (top + bottom) / 2.0 / S,
            (right - left) / S,
            (bottom - top) / S), ("clipped" if clipped else "keep")


# --------------------------------------------------------------------------- #
# 数据集布局
# --------------------------------------------------------------------------- #
def label_path_for(img_path, src_root):
    u"""找一张图对应的标注文件；找不到返回 None（= 负样本）。

    两种布局都认：
      A. `root/images/**.jpg` + `root/labels/**.txt`   （MaixHub / YOLO 常规）
      B. 图与 .txt **同目录同名**
    返回：路径 或 None。
    """
    d, name = os.path.split(img_path)
    stem = os.path.splitext(name)[0]
    rel = os.path.relpath(d, src_root)
    parts = [] if rel == "." else rel.split(os.sep)

    if parts and parts[0] == "images":                       # 布局 A
        cand = os.path.join(src_root, "labels", *parts[1:], stem + ".txt")
        if os.path.isfile(cand):
            return cand

    cand = os.path.join(d, stem + ".txt")                    # 布局 B
    return cand if os.path.isfile(cand) else None


def parse_label(path):
    u"""读 YOLO 标注。返回 [(cls, cx, cy, bw, bh), ...]；文件不存在 = 空列表。

    坑：解析失败**必须抛异常**并带上 `文件:行`（硬约束 2）。早期版本这里写了
        `continue`，结果一个把 `0.5` 写成 `o.5` 的样本被静默丢掉，训练集少一个
        目标样本而现场毫无提示。
    """
    if path is None:
        return []
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line:
                continue
            cols = line.split()
            if len(cols) < 5:
                raise ValueError("%s:%d: expected >=5 columns, got %d: %r"
                                 % (path, lineno, len(cols), line))
            try:
                vals = [float(c) for c in cols[:5]]
            except ValueError:
                raise ValueError("%s:%d: not numeric: %r" % (path, lineno, line))
            cls = int(vals[0])
            box = vals[1:]
            for v in box:
                # 容 1e-6 的浮点毛刺，再大就是真越界了
                if not (-1e-6 <= v <= 1.0 + 1e-6):
                    raise ValueError("%s:%d: normalized coord out of [0,1]: %r"
                                     % (path, lineno, line))
            out.append((cls, box[0], box[1], box[2], box[3]))
    return out


def fmt_box(cls, box):
    return "%d %.6f %.6f %.6f %.6f" % (cls, box[0], box[1], box[2], box[3])


# --------------------------------------------------------------------------- #
# VOC(XML) 布局
# --------------------------------------------------------------------------- #
def voc_label_path_for(img_path, src_root):
    u"""找一张图对应的 VOC XML；找不到返回 None（= 负样本，本数据集有 50 张）。

    布局与 `label_path_for()` 一一对应，只是把 `.txt` 换成 `annotations/*.xml`。
    """
    d, name = os.path.split(img_path)
    stem = os.path.splitext(name)[0]
    rel = os.path.relpath(d, src_root)
    parts = [] if rel == "." else rel.split(os.sep)

    if parts and parts[0] == "images":                       # 布局 A
        cand = os.path.join(src_root, "annotations", *parts[1:], stem + ".xml")
        if os.path.isfile(cand):
            return cand

    cand = os.path.join(d, stem + ".xml")                    # 布局 B
    return cand if os.path.isfile(cand) else None


def voc_dst_path(rel_img):
    u"""裁后 XML 在输出根下的相对路径：`images/x.jpg` -> `annotations/x.xml`。

    必须镜像 `voc_label_path_for()` 的布局规则 —— 两处不一致的话，XML 会写散到
    别的地方，而 MaixHub 只认 `annotations/` ⇒ **等于整份数据集没有标注**，
    且没有任何报错。
    """
    rel_dir, name = os.path.split(rel_img)
    stem = os.path.splitext(name)[0]
    parts = [] if rel_dir == "." else rel_dir.split(os.sep)
    if parts and parts[0] == "images":
        parts = ["annotations"] + parts[1:]
    return os.path.join(*(parts + [stem + ".xml"]))


def load_voc(path):
    u"""读一张 VOC XML，返回 (tree, root, W, H, objs)。

    objs = [(<object> 元素, 类名, x1, y1, x2, y2), ...]，**像素**坐标（左上/右下）。
    坑：这里**必须**把 XML 的 `<size>` 与图片真实尺寸对一遍。不一致说明数据集被改过
        （或 XML 是从别的图复制来的），此时裁剪窗口、平移量全算错，而**框仍然落在画面
        内** —— 不校验的话，这种错在报告里、在图上、在训练日志里全都看不出来。
    """
    try:
        tree = ET.parse(path)
    except ET.ParseError as exc:
        raise ValueError("%s: XML parse error: %s" % (path, exc))
    root = tree.getroot()

    def num(elem, expr, label):
        txt = elem.findtext(expr)
        if txt is None:
            raise ValueError("%s: missing <%s>" % (path, label))
        try:
            return float(txt)
        except ValueError:
            raise ValueError("%s: <%s> is not numeric: %r" % (path, label, txt))

    W = int(round(num(root, "size/width", "size/width")))
    H = int(round(num(root, "size/height", "size/height")))
    if W <= 0 or H <= 0:
        raise ValueError("%s: bad <size> %dx%d" % (path, W, H))

    objs = []
    for i, o in enumerate(root.findall("object")):
        cls = (o.findtext("name") or "").strip()
        if not cls:
            raise ValueError("%s: object[%d] has no <name>" % (path, i))
        b = o.find("bndbox")
        if b is None:
            raise ValueError("%s: object[%d] has no <bndbox>" % (path, i))
        x1 = num(b, "xmin", "object[%d]/xmin" % i)
        y1 = num(b, "ymin", "object[%d]/ymin" % i)
        x2 = num(b, "xmax", "object[%d]/xmax" % i)
        y2 = num(b, "ymax", "object[%d]/ymax" % i)
        if x1 < -1e-6 or y1 < -1e-6 or x2 > W + 1e-6 or y2 > H + 1e-6:
            raise ValueError("%s: object[%d] bndbox (%.1f,%.1f,%.1f,%.1f) outside "
                             "image %dx%d" % (path, i, x1, y1, x2, y2, W, H))
        objs.append((o, cls, x1, y1, x2, y2))
    return tree, root, W, H, objs


def voc_set_size(root, S):
    u"""改 `<size>`：宽高都写 S（裁后是正方形），`depth` 不动。"""
    size_el = root.find("size")
    if size_el is None:                                  # 极少数 XML 缺 <size>
        size_el = ET.SubElement(root, "size")
    for tag in ("width", "height"):
        el = size_el.find(tag)
        if el is None:
            el = ET.SubElement(size_el, tag)
        el.text = str(int(S))


def voc_set_box(elem, box):
    u"""把整数 bndbox (x1,y1,x2,y2) 写回这个 `<object>`。数值口径见文件头硬规定 2。"""
    b = elem.find("bndbox")
    if b is None:
        raise ValueError("object element has no <bndbox>")
    for tag, val in zip(("xmin", "ymin", "xmax", "ymax"), box):
        el = b.find(tag)
        if el is None:
            el = ET.SubElement(b, tag)
        el.text = str(int(val))


def voc_int_box(nb, S):
    u"""归一化框 -> 输出图上的**整数** bndbox（左上/右下）。

    坑：**先 round 再保证宽高 >= 1 px**。不 round 会写出一堆小数（见硬规定 2）；
        不保证宽高 >= 1 的话，一个 0.4 px 宽的残框会变成 `xmin == xmax` 的零面积框。
    ⚠️ 诚实边界：那两行 `[0, S]` 钳制**目前是死代码** —— `remap_box()` 返回值已经钳在
        `[0,S]` 内（这是它的约定），所以 round 不可能把它顶出界。留着纯属保险：
        万一以后有人改了 remap_box 的约定，这里不至于把负坐标写进标签。自测里那条
        "去掉钳制"的变异**捕获不到**（正是因为它不可达），已换成别的变异，见测试文件。
    """
    l = int(round((nb[0] - nb[2] / 2.0) * S))
    t = int(round((nb[1] - nb[3] / 2.0) * S))
    r = int(round((nb[0] + nb[2] / 2.0) * S))
    btm = int(round((nb[1] + nb[3] / 2.0) * S))
    l = max(0, min(S - 1, l))
    t = max(0, min(S - 1, t))
    r = max(l + 1, min(S, r))
    btm = max(t + 1, min(S, btm))
    return (l, t, r, btm)


def write_voc(doc, out_path, S, rel_img):
    u"""把裁后的 XML 写到 `out_path`：新 `<size>`、只剩留住的 `<object>`、`<path>` 指向新位置。

    坑 1：被丢弃的 `<object>` 必须**删掉**（`root.remove`），不能改成 `(0,0,0,0)` ——
        零面积框在不同训练器上表现不一（有的丢、有的当负样本、有的直接崩），而
        "这个目标的中心已经不在裁剪窗内"是**确定**的事，不该靠下游猜。
    坑 2：写完**回读一遍**再验 `<size>` 和 `<object>` 个数 —— 验内存里那棵树是同义
        反复（改的就是它），验不出"写盘这一步没生效"（本工程吃过"改了没生效"的亏）。
    """
    root = doc["root"]
    kept = []
    for (el, box) in doc["boxes_new"]:
        voc_set_box(el, box)
        kept.append(el)
    kept_ids = set(id(el) for el in kept)
    for o in list(root.findall("object")):
        if id(o) not in kept_ids:
            root.remove(o)
    voc_set_size(root, S)
    for tag, val in (("filename", os.path.basename(rel_img)), ("path", out_path)):
        el = root.find(tag)
        if el is not None:
            el.text = val                 # 缺字段就不补：MaixHub 只按 filename 配对
    if hasattr(ET, "indent"):
        ET.indent(doc["tree"], space="  ")          # 人能直接看，别挤成一行
    # 声明写 "UTF-8"（大写）是**照抄源文件的写法**：源 XML 就是 `encoding="UTF-8"`，
    # 而 XML 规范里编码名大小写不敏感、任何正规解析器都认。跟随源写法纯属保险 ——
    # 万一 MaixHub 那边的解析是手搓的正则，少一处差异就少一次踩雷的机会。
    doc["tree"].write(out_path, encoding="UTF-8", xml_declaration=True)

    back = ET.parse(out_path).getroot()
    bw = back.findtext("size/width")
    bh = back.findtext("size/height")
    if (bw, bh) != (str(int(S)), str(int(S))):
        raise ValueError("%s: read-back <size> is %sx%s, expected %dx%d"
                         % (out_path, bw, bh, S, S))
    if len(back.findall("object")) != len(kept):
        raise ValueError("%s: read-back has %d <object>, expected %d"
                         % (out_path, len(back.findall("object")), len(kept)))


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #
def image_size(path):
    u"""只读头部拿宽高（不解码像素）—— 省内存，也省时间。"""
    with Image.open(path) as im:
        if im.mode == "P" and "transparency" in im.info:
            pass                                   # 与尺寸无关，只是提醒自己别乱转模式
        orient = im.getexif().get(EXIF_ORIENTATION_TAG, 1)
        if orient not in (0, 1, None):
            raise ValueError("%s: EXIF orientation=%s; this tool does NOT rotate "
                             "pixels, labels would land in the wrong place"
                             % (path, orient))
        return im.size


def crop_and_save(src, dst, S, x0, y0, quality):
    with Image.open(src) as im:
        im = im.crop((x0, y0, x0 + S, y0 + S))
        # JPEG 存不下 alpha，就地转 RGB（不转的话 PIL 会抛 OSError）
        if os.path.splitext(dst)[1].lower() in JPEG_EXT and im.mode not in ("RGB", "L", "CMYK"):
            im = im.convert("RGB")
        kw = {"quality": quality} if os.path.splitext(dst)[1].lower() in JPEG_EXT else {}
        im.save(dst, **kw)
    # **回读**盘上那个文件再验尺寸 —— 验内存里的 im.size 是同义反复（crop 完必然是 SxS），
    # 查不出"写盘这一步出了岔子"（本工程吃过"改了但没生效"的亏，判据必须落在产物上）。
    with Image.open(dst) as chk:
        if chk.size != (S, S):
            raise ValueError("wrote %s with size %s, expected %dx%d"
                             % (dst, chk.size, S, S))


def dataset_mode(src_root):
    u"""判数据集格式：有 `annotations/` 目录 = VOC(XML)，否则按 YOLO 处理。

    为什么按**目录名**判，不按"扫一遍看有没有 .xml"：目录名是数据集结构的一部分，
    稳定、零成本；扫文件在 300 张上要多读几千个目录项，还得替"两种标注混在一起"
    这种歧义选一个答案 —— 那种歧义该由人裁决，不该由工具偷偷选。
    """
    return "voc" if os.path.isdir(os.path.join(src_root, "annotations")) else "yolo"


def build_plan(src_root, size, limit=0):
    u"""第一遍：只读 + 算，**不写任何东西**。返回一个 dict（键见函数末尾 return）。

    坑：`limit` 必须在这里生效，不能在 `main()` 里对结果切片 —— 那样报告里的合计是
        全量的、逐图行却是截断的（"合计 300 张、逐图只有 20 行"），而报告正是要拿去
        当证据的，自相矛盾的数字比没有数字更坏。
    坑：`train.txt` 里"这行的图不存在"的校验**只在 `limit == 0` 时做** —— 限量跑只走
        了一部分图，拿"没走到"当"不存在"会凭空报错。
    """
    mode = dataset_mode(src_root)
    # 任务元组：(图片绝对路径, 相对路径, S, x0, y0, YOLO 新行, doc)
    #   doc = None(YOLO)，或 {"tree","root","boxes_new","has_xml"}（VOC 写 XML 用）
    tasks = []
    copy_files = []     # 原样拷贝的非图片、非标注文件
    consumed_labels = set()
    stats = {k: 0 for k in (
        "images_total", "images_written", "images_dropped", "images_no_label",
        "boxes_total", "boxes_kept", "boxes_clipped", "boxes_dropped",
        "boxes_degenerate", "files_copied", "xml_written",
        "train_lines_in", "train_lines_out", "train_lines_excluded")}
    sizes = {}
    per_image = []
    problems = []
    src_names = set()   # 源里出现过的图片文件名（校验 train.txt 用）

    for dirpath, dirnames, filenames in os.walk(src_root):
        if limit and stats["images_total"] >= limit:
            break
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, src_root)
        parts = [] if rel_dir == "." else rel_dir.split(os.sep)

        for name in sorted(filenames):
            if limit and stats["images_total"] >= limit:
                break
            abspath = os.path.join(dirpath, name)
            ext = os.path.splitext(name)[1].lower()

            if ext not in IMAGE_EXT:
                # 标注目录（YOLO 的 `labels/`、VOC 的 `annotations/`）里的文件由
                # "图片分支"负责**重写**。原样拷贝 = 把没改过的坐标当标注用 ——
                # 旧版正是栽在这里（XML 被当普通文件拷走，报告里 files_copied=247）。
                if parts and parts[0] in ("labels", "annotations"):
                    continue
                # train.txt 由下面统一重生成：原样拷贝会把"已被裁掉"的图留在清单里
                if not parts and name.lower() == "train.txt":
                    continue
                copy_files.append(abspath)
                continue

            stats["images_total"] += 1
            src_names.add(name)
            label = (voc_label_path_for(abspath, src_root) if mode == "voc"
                     else label_path_for(abspath, src_root))
            if label:
                consumed_labels.add(os.path.normcase(os.path.abspath(label)))
            try:
                W, H = image_size(abspath)
                S, x0, y0 = plan_crop(W, H, size)
                if mode == "voc":
                    if label:
                        tree, root_el, xw, xh, objs = load_voc(label)
                        if (xw, xh) != (W, H):
                            raise ValueError("%s: <size> says %dx%d, image is %dx%d"
                                             % (label, xw, xh, W, H))
                    else:
                        tree, root_el, objs = None, None, []   # 负样本：本来就没有框
                    # 两种格式统一成**归一化中心框** ⇒ 下面共用同一套取舍策略，
                    # 策略只有 `remap_box()` 一份实现（两份实现一定会漂移）
                    boxes = [((o[2] + o[4]) / 2.0 / W, (o[3] + o[5]) / 2.0 / H,
                              (o[4] - o[2]) / W, (o[5] - o[3]) / H) for o in objs]
                    classes = [None] * len(boxes)
                else:
                    tree, root_el, objs = None, None, []
                    rows = parse_label(label)
                    boxes = [(r[1], r[2], r[3], r[4]) for r in rows]
                    classes = [int(r[0]) for r in rows]
            except Exception as exc:
                problems.append(str(exc))
                continue

            sizes["%dx%d" % (W, H)] = sizes.get("%dx%d" % (W, H), 0) + 1
            if label is None:
                stats["images_no_label"] += 1

            new_lines = []          # YOLO：输出 .txt 的行
            boxes_new = []          # VOC：(<object> 元素, 新 bndbox 整数四元组)
            n_keep = n_clip = n_drop = n_degen = 0
            for i, (cx, cy, bw, bh) in enumerate(boxes):
                stats["boxes_total"] += 1
                nb, status = remap_box(cx, cy, bw, bh, W, H, S, x0, y0)
                if status == "outside":
                    n_drop += 1
                elif status == "degenerate":
                    n_degen += 1
                else:
                    if mode == "voc":
                        boxes_new.append((objs[i][0], voc_int_box(nb, S)))
                    else:
                        new_lines.append(fmt_box(classes[i], nb))
                    if status == "clipped":
                        n_clip += 1
                    else:
                        n_keep += 1

            stats["boxes_kept"] += n_keep
            stats["boxes_clipped"] += n_clip
            stats["boxes_dropped"] += n_drop
            stats["boxes_degenerate"] += n_degen

            rel = os.path.relpath(abspath, src_root)
            if boxes and not (new_lines or boxes_new):
                # 原本有框、裁完全没了 ⇒ 整张图丢掉（留着就是一张没有正样本的"负样本"，
                # 而它其实拍的是有目标的画面 —— 那种标签是错的，比没有更坏）
                stats["images_dropped"] += 1
                per_image.append("DROP  %-9s -> %dpx | boxes keep=0 clip=%d drop=%d degen=%d | %s"
                                 % ("%dx%d" % (W, H), S, n_clip, n_drop, n_degen, rel))
                continue

            stats["images_written"] += 1
            per_image.append("OK    %-9s -> %dpx +%d+%d | boxes keep=%d clip=%d drop=%d degen=%d | %s"
                             % ("%dx%d" % (W, H), S, x0, y0,
                                n_keep, n_clip, n_drop, n_degen, rel))
            doc = None
            if mode == "voc":
                doc = {"tree": tree, "root": root_el, "boxes_new": boxes_new,
                       "has_xml": label is not None}
            tasks.append((abspath, rel, S, x0, y0, new_lines, doc))

    # 同目录布局（B）下，图的标注文件已被"图片分支"消费，别再原样拷一遍
    copy_files = [p for p in copy_files
                  if os.path.normcase(os.path.abspath(p)) not in consumed_labels]
    stats["files_copied"] = len(copy_files)
    # XML 到底写没写，只有"写"那一步知道；这里统计的是**计划要写**的数量。
    # 敢这么写的前提是：写盘任何一步出错都会抛异常、整轮以非零码结束，
    # 而报告只在全部写完之后才落盘 ⇒ 不会出现"报告说写了、盘上没有"。
    stats["xml_written"] = sum(1 for t in tasks if t[6] and t[6]["has_xml"])

    # ---- train.txt：筛选后**重生成**，绝不原样拷贝（文件头硬规定 4）
    written = set(os.path.basename(t[1]) for t in tasks)
    train_in, train_out = [], []
    train_path = os.path.join(src_root, "train.txt")
    has_train = os.path.isfile(train_path)
    if has_train:
        with open(train_path, "r", encoding="utf-8", errors="replace") as fh:
            for lineno, raw in enumerate(fh, 1):
                line = raw.strip()
                if not line:
                    continue
                base = os.path.basename(line.replace("\\", "/"))
                if base in written:
                    train_in.append(line)
                    train_out.append(line)              # 这行的图还在 -> 留下
                elif limit:
                    train_in.append(line)               # 限量跑：判不了，就不丢
                elif base in src_names:
                    train_in.append(line)               # 图被裁掉了 -> 从清单里去掉
                else:
                    problems.append("%s:%d: %r matches no image in the dataset"
                                    % (train_path, lineno, line))
                    train_in.append(line)
    stats["train_lines_in"] = len(train_in)
    stats["train_lines_out"] = len(train_out)
    stats["train_lines_excluded"] = len(train_in) - len(train_out)

    return {"tasks": tasks, "copy_files": copy_files, "stats": stats, "sizes": sizes,
            "per_image": per_image, "problems": problems, "mode": mode,
            "train_in": train_in, "train_out": train_out, "has_train": has_train}


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Center-crop a dataset to squares (plan B). "
                    "Handles VOC (annotations/*.xml) and YOLO (labels/*.txt).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="source dataset root: images/ + annotations/(xml) or images/ + labels/(txt)")
    ap.add_argument("--dst", required=True, help="output root (must differ from --src)")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE,
                    help="crop side in px; 0 = auto = min(w,h)  [default: 0]")
    ap.add_argument("--quality", type=int, default=DEFAULT_QUALITY,
                    help="JPEG quality for re-encode  [default: %d]" % DEFAULT_QUALITY)
    ap.add_argument("--limit", type=int, default=0,
                    help="only process the first N images (smoke test)")
    ap.add_argument("--dry-run", action="store_true", help="plan only, write nothing")
    ap.add_argument("--force", action="store_true", help="allow a non-empty --dst")
    args = ap.parse_args(argv)

    src = os.path.abspath(args.src)
    dst = os.path.abspath(args.dst)

    if not os.path.isdir(src):
        sys.stderr.write("ERROR: --src is not a directory: %s\n" % src)
        return 2
    if os.path.normcase(src) == os.path.normcase(dst):
        sys.stderr.write("ERROR: --dst must differ from --src (refusing to overwrite "
                         "the source dataset)\n")
        return 2
    if src.startswith(dst + os.sep) or dst.startswith(src + os.sep):
        sys.stderr.write("ERROR: --dst and --src must not be nested (src=%s dst=%s)\n"
                         % (src, dst))
        return 2
    if os.path.isdir(dst) and os.listdir(dst) and not args.force and not args.dry_run:
        sys.stderr.write("ERROR: --dst is not empty: %s (pass --force to allow)\n" % dst)
        return 2

    plan = build_plan(src, args.size, args.limit)
    stats = plan["stats"]
    problems = plan["problems"]

    if problems:
        sys.stderr.write("ERROR: %d problem(s) found; nothing was written.\n"
                         % len(problems))
        for p in problems[:20]:
            sys.stderr.write("  " + p + "\n")
        if len(problems) > 20:
            sys.stderr.write("  ... and %d more\n" % (len(problems) - 20))
        return 3

    lines = []
    add = lines.append
    add("# crop_center_240.py report")
    add("# generated %s" % datetime.datetime.now().isoformat(timespec="seconds"))
    add("src    = %s" % src)
    add("dst    = %s" % dst)
    add("mode   = %s (%s)" % (plan["mode"],
                              "annotations/*.xml" if plan["mode"] == "voc" else "labels/*.txt"))
    add("params = size=%s  quality=%d  policy=box-center-inside  edge=clip"
        % ("auto(min(w,h))" if args.size == 0 else str(args.size), args.quality))
    add("limit  = %s" % (args.limit if args.limit else "none"))
    add("dry_run= %s" % bool(args.dry_run))
    if plan["has_train"]:
        add("train.txt = regenerated: in=%d out=%d excluded=%d (rows whose image was dropped)"
            % (stats["train_lines_in"], stats["train_lines_out"],
               stats["train_lines_excluded"]))
    if plan["mode"] == "voc":
        add("xml     = rewritten: integer bndbox clamped to [0,S]; "
            "name/truncated/difficult/occluded kept verbatim")
    add("")
    add("[input size histogram]")
    for k in sorted(plan["sizes"]):
        add("  %-10s x %d" % (k, plan["sizes"][k]))
    add("")
    add("[summary]")
    for k in sorted(stats):
        add("  %-18s = %d" % (k, stats[k]))
    add("")
    add("[per-image]  status  src_size -> crop_px  +x0+y0 | box accounting | path")
    for ln in plan["per_image"]:
        add("  " + ln)
    report = "\n".join(lines) + "\n"

    if args.dry_run:
        sys.stdout.write(report)
        sys.stdout.write("[dry-run] nothing written\n")
        return 0

    for (abspath, rel, S, x0, y0, new_lines, doc) in plan["tasks"]:
        out_img = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(out_img), exist_ok=True)
        crop_and_save(abspath, out_img, S, x0, y0, args.quality)
        if doc is None:                                   # YOLO：写 .txt（负样本写空文件）
            out_lbl = os.path.splitext(out_img)[0] + ".txt"
            with open(out_lbl, "w", encoding="utf-8", newline="\n") as fh:
                if new_lines:
                    fh.write("\n".join(new_lines) + "\n")
        elif doc["has_xml"]:                              # VOC：重写 XML
            out_xml = os.path.join(dst, voc_dst_path(rel))
            os.makedirs(os.path.dirname(out_xml), exist_ok=True)
            write_voc(doc, out_xml, S, rel)
        # 负样本（源里没有 XML）**不造空 XML** —— MaixHub 的"没有目标"就是没有那个文件

    for p in plan["copy_files"]:
        rel = os.path.relpath(p, src)
        out = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(p, "rb") as fh:
            data = fh.read()
        with open(out, "wb") as fh:
            fh.write(data)

    os.makedirs(dst, exist_ok=True)
    if plan["has_train"]:
        with open(os.path.join(dst, "train.txt"), "w", encoding="utf-8", newline="\n") as fh:
            if plan["train_out"]:
                fh.write("\n".join(plan["train_out"]) + "\n")
    with open(os.path.join(dst, "crop_report.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(report)

    sys.stdout.write("[crop] src=%s\n" % src)
    sys.stdout.write("[crop] dst=%s\n" % dst)
    sys.stdout.write("[crop] mode=%s  images written=%d dropped=%d  xml rewritten=%d\n"
                     % (plan["mode"], stats["images_written"], stats["images_dropped"],
                        stats["xml_written"]))
    sys.stdout.write("[crop] boxes kept=%d clipped=%d dropped=%d degenerate=%d\n"
                     % (stats["boxes_kept"], stats["boxes_clipped"],
                        stats["boxes_dropped"], stats["boxes_degenerate"]))
    sys.stdout.write("[crop] report -> %s\n" % os.path.join(dst, "crop_report.txt"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
