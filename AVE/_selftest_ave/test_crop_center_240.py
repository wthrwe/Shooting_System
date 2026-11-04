# -*- coding: utf-8 -*-
u"""`crop_center_240.py` 的宿主机自测 —— 造数据集、真跑工具、验产物、再变异反证

# 这个文件负责什么
  证明 `crop_center_240.py` 的四件事**真的成立**（而不是"看着像成立"）：
    1. **标注数学对**：中心在窗内的框按 `-(x0,y0)` 平移、按新边长归一化，数值精确；
    2. **取舍策略对**：中心在窗外的框被丢；框身越出边界的被截断并计数；
       框全丢光的图**整张不写**（不能变成一张假负样本）；
    3. **像素真的被裁了**：不是只改了 .txt —— 用一张带定位色块的 PNG 直接验像素；
    4. **VOC(XML) 路径**（T15 起）：XML 被**重写**而不是拷贝、`<size>` 跟着变、
       被丢的 `<object>` 真被删、`train.txt` 被筛过、源数据集**一个字节没动**。
  然后**变异反证**：故意把工具改坏 8 处（每条规则至少一处），要求测试**必须变红**。
  "用例全绿"从来不是证据（`CLAUDE.md` 规则 3.5 / 本工程第二十九次那次的教训）。

# 不负责
  真数据集（这套自测自己造图，不碰用户的 250 张）；不重训；不评估模型。

# 坑（写的时候踩过 / 容易踩的）
  - 控制台输出**只允许 ASCII**：GBK 控制台打不出特殊符号会直接抛，把"测试失败"
    伪装成"脚本崩了"。
  - `subprocess(text=True)` 在本机会炸（父进程按 GBK 解子进程的字节）⇒ 一律
    `capture_output=True` 自己 decode。
  - 变异体的锚点**必须断言只命中 1 次**：锚点命中 0 次 = 变异没生效，那种"变异被捕获"
    是假的（本工程第二十九次吃过这个亏）。
"""

import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "crop_center_240.py")
SANDBOX = os.path.join(HERE, "_tmp_crop_test")

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:                                     # pragma: no cover
    pass

TOL = 1e-5


# --------------------------------------------------------------------------- #
# 造数据集的小工具
# --------------------------------------------------------------------------- #
def new_case(name, fmt="yolo"):
    u"""造一个空数据集骨架。fmt="voc" 时建 `annotations/`（不建 `labels/`）——
    工具正是按"有没有 annotations/ 目录"判格式的，两个都建会测不出这条判据。"""
    work = os.path.join(SANDBOX, name)
    shutil.rmtree(work, ignore_errors=True)
    src = os.path.join(work, "src")
    os.makedirs(os.path.join(src, "images"))
    os.makedirs(os.path.join(src, "annotations" if fmt == "voc" else "labels"))
    return work, src, os.path.join(work, "dst")


def mk(src, img_rel, size, marks=(), label=None, fmt=None):
    u"""写一张图 + 可选标注。marks = [(x,y,w,h,(r,g,b)), ...]。"""
    path = os.path.join(src, img_rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im = Image.new("RGB", size, (0, 0, 255))
    d = ImageDraw.Draw(im)
    for (x, y, w, h, col) in marks:
        d.rectangle([x, y, x + w - 1, y + h - 1], fill=col)
    if fmt is None:
        fmt = "PNG" if path.lower().endswith(".png") else "JPEG"
    im.save(path, fmt)
    if label is not None:
        lp = os.path.join(src, "labels", os.path.splitext(os.path.basename(img_rel))[0] + ".txt")
        with open(lp, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(label) + ("\n" if label else ""))
    return path


def mk_xml(src, img_rel, size, objs):
    u"""写一张 VOC XML。objs = [(name, x1, y1, x2, y2), ...]，整数像素坐标。

    字段刻意按 MaixHub 导出的样式写全（folder/filename/path/source/size/segmented/
    object.name.pose.truncated.difficult.occluded.bndbox）—— 少写字段的话，工具
    "保留原字段"这条就测不出来了。
    """
    w, h = size
    base = os.path.basename(img_rel)
    stem = os.path.splitext(base)[0]
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             "<annotation>",
             "  <folder>images</folder>",
             "  <filename>%s</filename>" % base,
             "  <path>E:/src/images/%s</path>" % base,
             "  <source><database>Unknown</database></source>",
             "  <size>",
             "    <width>%d</width>" % w,
             "    <height>%d</height>" % h,
             "    <depth>3</depth>",
             "  </size>",
             "  <segmented>0</segmented>"]
    for (cls, x1, y1, x2, y2) in objs:
        lines += ["  <object>",
                  "    <name>%s</name>" % cls,
                  "    <pose>Unspecified</pose>",
                  "    <truncated>0</truncated>",
                  "    <difficult>0</difficult>",
                  "    <occluded>0</occluded>",
                  "    <bndbox>",
                  "      <xmin>%d</xmin>" % x1,
                  "      <ymin>%d</ymin>" % y1,
                  "      <xmax>%d</xmax>" % x2,
                  "      <ymax>%d</ymax>" % y2,
                  "    </bndbox>",
                  "  </object>"]
    lines.append("</annotation>")
    p = os.path.join(src, "annotations", stem + ".xml")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    return p


def mk_voc(src, img_rel, size, objs, train=None, xml_size=None, marks=()):
    u"""一张 VOC 图 + 它的 XML。objs=None 表示**没有 XML**（负样本）。"""
    mk(src, img_rel, size, marks=marks)                 # 图；不写 .txt
    if objs is not None:
        mk_xml(src, img_rel, xml_size or size, objs)
    if train is not None:
        with open(os.path.join(src, "train.txt"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(train) + "\n")


def read_xml(path):
    u"""读输出 XML -> (size, [(name, xmin, ymin, xmax, ymax), ...])；不存在返回 None。"""
    if not os.path.isfile(path):
        return None
    r = ET.parse(path).getroot()
    size = (int(float(r.findtext("size/width"))), int(float(r.findtext("size/height"))))
    objs = []
    for o in r.findall("object"):
        b = o.find("bndbox")
        objs.append((o.findtext("name").strip(),
                     int(float(b.findtext("xmin"))), int(float(b.findtext("ymin"))),
                     int(float(b.findtext("xmax"))), int(float(b.findtext("ymax")))))
    return size, objs


def read_bytes(path):
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as fh:
        return fh.read()


def run_tool(tool, src, dst, extra=()):
    cmd = [sys.executable, tool, "--src", src, "--dst", dst] + list(extra)
    p = subprocess.run(cmd, capture_output=True)
    return (p.returncode,
            p.stdout.decode("utf-8", "replace"),
            p.stderr.decode("utf-8", "replace"))


def read_label(path):
    u"""读输出标注；文件不存在返回 None，空文件返回 []。"""
    if not os.path.isfile(path):
        return None
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            c = line.split()
            out.append((int(float(c[0])),) + tuple(float(x) for x in c[1:5]))
    return out


def read_text(path):
    if not os.path.isfile(path):
        return ""
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def report_has(text, key, val):
    u"""在报告里找 `key = val`。

    不能直接 `"images_total        = 1" in rep` —— 那个空格数是我自己数的，
    报告里是 `%-18s` 补出来的，**数错一次就变成"永远失败"或更糟的"永远通过"**。
    这里按"空白任意个"匹配。
    """
    return re.search(r"\b%s\s*=\s*%s\b" % (re.escape(key), re.escape(str(val))),
                     text) is not None


def size_of(path):
    with Image.open(path) as im:
        return im.size


def size_or_none(path):
    u"""产物不存在就返回 None —— 让"工具崩了/没写"变成**断言失败**，
    而不是把测试自己抛成一团 traceback（那样看日志的人分不清是工具坏了还是测试坏了）。"""
    return size_of(path) if os.path.isfile(path) else None


def px(path, x, y):
    with Image.open(path) as im:
        return im.convert("RGB").getpixel((x, y))


def near(a, b, tol=TOL):
    return abs(a - b) <= tol


def box_is(got, exp, tol=TOL):
    if got is None or len(got) != len(exp):
        return False
    return all(near(g, e, tol) for g, e in zip(got, exp))


# --------------------------------------------------------------------------- #
# 用例
# --------------------------------------------------------------------------- #
def run_suite(tool, root):
    u"""跑全套，返回 {用例名: (是否通过, 说明)}。"""
    res = {}

    def rec(name, ok, msg=""):
        res[name] = (bool(ok), str(msg))

    # ---- T1 基本几何：320x240 中心框 -> -x0 平移 + 按 240 归一化 ----
    work, src, dst = new_case("t1")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 0.25 0.25"])
    rc, out, err = run_tool(tool, src, dst)
    img = os.path.join(dst, "images", "a.jpg")
    lab = read_label(os.path.join(dst, "images", "a.txt"))
    exp = (0, 0.5, 0.5, 80.0 / 240, 0.25)
    rec("T1_geometry", rc == 0 and size_or_none(img) == (240, 240)
        and lab is not None and len(lab) == 1 and box_is(lab[0], exp),
        "rc=%d size=%s label=%s exp=%s" % (rc, size_or_none(img), lab, exp))

    # ---- T2 中心在窗外 -> 丢框、且整张图不写 ----
    #     框特意给**宽**的（bw=0.5）：窄框被"保留"时会退化成零宽而被另一条规则挡住，
    #     那样就测不出"中心判据"本身，变异 M3 会假装被捕获。
    work, src, dst = new_case("t2")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.05 0.5 0.5 0.25"])
    rc, out, err = run_tool(tool, src, dst)
    rep2 = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T2_outside", rc == 0
        and not os.path.isfile(os.path.join(dst, "images", "a.jpg"))
        and not os.path.isfile(os.path.join(dst, "images", "a.txt"))
        and report_has(rep2, "images_dropped", 1)
        and report_has(rep2, "boxes_dropped", 1),
        "rc=%d img_exists=%s rep_drop=%s"
        % (rc, os.path.isfile(os.path.join(dst, "images", "a.jpg")),
           report_has(rep2, "images_dropped", 1)))

    # ---- T3 中心在窗内但越出左边界 -> 保留 + 截断（数值要精确） ----
    work, src, dst = new_case("t3")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.140625 0.5 0.09375 0.041667"])
    rc, out, err = run_tool(tool, src, dst)
    lab = read_label(os.path.join(dst, "images", "a.txt"))
    # X=45, BW=30 -> 跨度 30..60, 裁剪窗从 40 起 -> 截成 0..20
    exp = (0, 10.0 / 240, 0.5, 20.0 / 240, 10.0 / 240)
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T3_clipped", rc == 0 and lab is not None and len(lab) == 1 and box_is(lab[0], exp)
        and report_has(rep, "boxes_clipped", 1),
        "label=%s exp=%s" % (lab, exp))

    # ---- T4 一图两框：一留一丢 ----
    work, src, dst = new_case("t4")
    mk(src, "images/a.jpg", (320, 240),
       label=["0 0.5 0.5 0.25 0.25", "0 0.95 0.5 0.25 0.25"])
    rc, out, err = run_tool(tool, src, dst)
    lab = read_label(os.path.join(dst, "images", "a.txt"))
    rec("T4_mixed", rc == 0 and lab is not None and len(lab) == 1
        and box_is(lab[0], (0, 0.5, 0.5, 80.0 / 240, 0.25)),
        "label=%s" % (lab,))

    # ---- T5 零宽框 -> 退化，丢弃（覆盖 degenerate 这条防御分支） ----
    work, src, dst = new_case("t5")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 0.0 0.2"])
    rc, out, err = run_tool(tool, src, dst)
    rec("T5_degenerate", rc == 0
        and not os.path.isfile(os.path.join(dst, "images", "a.jpg")),
        "rc=%d img_exists=%s" % (rc, os.path.isfile(os.path.join(dst, "images", "a.jpg"))))

    # ---- T6 已经是方的（224x224）-> 恒等变换，标注一字不变 ----
    work, src, dst = new_case("t6")
    mk(src, "images/a.jpg", (224, 224), label=["0 0.5 0.5 0.2 0.2"])
    rc, out, err = run_tool(tool, src, dst)
    lab = read_label(os.path.join(dst, "images", "a.txt"))
    rec("T6_identity", rc == 0 and size_or_none(os.path.join(dst, "images", "a.jpg")) == (224, 224)
        and lab is not None and box_is(lab[0], (0, 0.5, 0.5, 0.2, 0.2)),
        "label=%s" % (lab,))

    # ---- T7 竖向图 240x320 -> auto 取 min=240, 纵向偏 40；框跟着上移 ----
    work, src, dst = new_case("t7")
    mk(src, "images/a.jpg", (240, 320),
       label=["0 0.5 0.5 0.25 0.25", "0 0.5 0.1 0.25 0.25"])
    rc, out, err = run_tool(tool, src, dst)
    lab = read_label(os.path.join(dst, "images", "a.txt"))
    rec("T7_yoffset", rc == 0 and size_or_none(os.path.join(dst, "images", "a.jpg")) == (240, 240)
        and lab is not None and len(lab) == 1
        and box_is(lab[0], (0, 0.5, 0.5, 0.25, 80.0 / 240)),
        "label=%s" % (lab,))

    # ---- T8 负样本（无 .txt）-> 图照裁，生成**空** .txt ----
    work, src, dst = new_case("t8")
    mk(src, "images/a.jpg", (320, 240), label=None)
    rc, out, err = run_tool(tool, src, dst)
    lp = os.path.join(dst, "images", "a.txt")
    rec("T8_negative", rc == 0 and os.path.isfile(os.path.join(dst, "images", "a.jpg"))
        and os.path.isfile(lp) and os.path.getsize(lp) == 0,
        "rc=%d label_exists=%s" % (rc, os.path.isfile(lp)))

    # ---- T9 像素级：真的裁了，且裁的位置对 ----
    work, src, dst = new_case("t9")
    # (40,130) 与 (279,130) 是裁剪窗的**左右边缘那一列**：让它们显形，等于把
    # "x0 到底等于 40 还是 41"这条判死 —— 只看中间那块红方块的话，窗口整体平移
    # 一个像素是看不出来的（变异 M1 第一版就是这么溜过去的）。
    mk(src, "images/a.png", (320, 240),
       marks=[(50, 100, 20, 20, (255, 0, 0)),        # 裁后应落在 (10..29,100..119)
              (0, 100, 15, 20, (0, 255, 0)),         # 整个被裁掉
              (300, 100, 20, 20, (255, 255, 0)),     # 整个被裁掉
              (40, 130, 1, 1, (255, 0, 255)),        # 窗口左边界 -> 输出 x=0
              (279, 130, 1, 1, (0, 255, 255))],      # 窗口右边界 -> 输出 x=239
       label=["0 0.5 0.5 0.2 0.2"])
    rc, out, err = run_tool(tool, src, dst)
    op = os.path.join(dst, "images", "a.png")
    have = os.path.isfile(op)
    rec("T9_pixels", rc == 0 and size_or_none(op) == (240, 240)
        and have and px(op, 15, 105) == (255, 0, 0)
        and px(op, 5, 105) == (0, 0, 255)
        and px(op, 235, 105) == (0, 0, 255)
        and px(op, 0, 130) == (255, 0, 255)
        and px(op, 239, 130) == (0, 255, 255),
        "size=%s red=%s cut_l=%s cut_r=%s edge_l=%s edge_r=%s"
        % (size_or_none(op),
           px(op, 15, 105) if have else None,
           px(op, 5, 105) if have else None,
           px(op, 235, 105) if have else None,
           px(op, 0, 130) if have else None,
           px(op, 239, 130) if have else None))

    # ---- T10 坏标注 -> 非零退出 + 点名 file:line（硬约束 2） ----
    work, src, dst = new_case("t10")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 0.2"])
    rc, out, err = run_tool(tool, src, dst)
    rec("T10_badcol", rc == 3 and "a.txt:1" in err and not os.path.isdir(dst),
        "rc=%d err=%s" % (rc, err.strip().splitlines()[:1]))
    work, src, dst = new_case("t10b")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 o.2 0.2"])
    rc, out, err = run_tool(tool, src, dst)
    rec("T10_badnum", rc == 3 and "a.txt:1" in err, "rc=%d" % rc)
    work, src, dst = new_case("t10c")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 1.7 0.2"])
    rc, out, err = run_tool(tool, src, dst)
    rec("T10_range", rc == 3 and "out of [0,1]" in err, "rc=%d" % rc)

    # ---- T11 显式 --size 240 与 auto 等价 ----
    work, src, dst = new_case("t11")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 0.25 0.25"])
    rc, out, err = run_tool(tool, src, dst, ["--size", "240", "--limit", "1"])
    lab = read_label(os.path.join(dst, "images", "a.txt"))
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T11_explicit_size", rc == 0 and lab is not None
        and box_is(lab[0], (0, 0.5, 0.5, 80.0 / 240, 0.25))
        and report_has(rep, "images_total", 1), "label=%s" % (lab,))

    # ---- T12 拒绝危险参数 ----
    work, src, dst = new_case("t12")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 0.25 0.25"])
    rc_same, _, err_same = run_tool(tool, src, src)
    rc_nest, _, _ = run_tool(tool, src, os.path.join(src, "out"))
    rc0, _, _ = run_tool(tool, src, dst)
    rc1, _, err1 = run_tool(tool, src, dst)                 # dst 非空、无 --force
    rc2, _, _ = run_tool(tool, src, dst, ["--force"])
    rec("T12_refuse", rc_same == 2 and rc_nest == 2 and rc0 == 0 and rc1 == 2 and rc2 == 0
        and "differ" in err_same and "not empty" in err1,
        "same=%d nest=%d ok=%d nonempty=%d forced=%d" % (rc_same, rc_nest, rc0, rc1, rc2))

    # ---- T13 非图片文件原样拷贝（data.yaml） ----
    work, src, dst = new_case("t13")
    mk(src, "images/a.jpg", (320, 240), label=["0 0.5 0.5 0.25 0.25"])
    with open(os.path.join(src, "data.yaml"), "w", encoding="utf-8") as fh:
        fh.write("names: [target]\n")
    rc, out, err = run_tool(tool, src, dst)
    rec("T13_copy", rc == 0 and os.path.isfile(os.path.join(dst, "data.yaml"))
        and open(os.path.join(dst, "data.yaml"), encoding="utf-8").read() == "names: [target]\n",
        "rc=%d" % rc)

    # ---- T14 --limit 把统计口径一起截断（不许出现"合计全量、逐图截断"） ----
    work, src, dst = new_case("t14")
    for i in range(3):
        mk(src, "images/a%d.jpg" % i, (320, 240), label=["0 0.5 0.5 0.25 0.25"])
    rc, out, err = run_tool(tool, src, dst, ["--limit", "2"])
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T14_limit", rc == 0 and report_has(rep, "images_total", 2)
        and report_has(rep, "images_written", 2) and rep.count("OK    ") == 2,
        "rc=%d ok_lines=%d" % (rc, rep.count("OK    ")))

    # ================= VOC(XML) 路径 =================
    # ---- T15 XML 被重写：新 <size> + bndbox 按 -(x0,y0) 平移；源 XML 不许动 ----
    #      (100,80,180,160) -> 窗口 x0=40 -> (60,80,140,160)，边长 234/240 都不变宽高
    work, src, dst = new_case("t15", fmt="voc")
    src_xml = mk_xml(src, "images/a.jpg", (320, 240), [("1", 100, 80, 180, 160)])
    mk(src, "images/a.jpg", (320, 240))
    before = read_bytes(src_xml)
    rc, out, err = run_tool(tool, src, dst)
    got = read_xml(os.path.join(dst, "annotations", "a.xml"))
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T15_voc_rewrite", rc == 0
        and size_or_none(os.path.join(dst, "images", "a.jpg")) == (240, 240)
        and got == ((240, 240), [("1", 60, 80, 140, 160)])
        and report_has(rep, "xml_written", 1)
        and report_has(rep, "mode", "voc")
        and read_bytes(src_xml) == before,
        "rc=%d got=%s src_changed=%s" % (rc, got, read_bytes(src_xml) != before))

    # ---- T16 越界的框被钳到 [0,S]，中心在窗外的框**从 XML 里删掉** ----
    work, src, dst = new_case("t16", fmt="voc")
    # 框1: 中心 X=55, 宽 90 -> 裁后 -30..60 -> 钳成 0..60（截断）
    # 框2: 中心 X=5 在窗(40..280)外 -> 丢
    mk_voc(src, "images/a.jpg", (320, 240),
           [("1", 10, 100, 100, 180), ("1", 0, 20, 10, 40)])
    rc, out, err = run_tool(tool, src, dst)
    got = read_xml(os.path.join(dst, "annotations", "a.xml"))
    raw = read_text(os.path.join(dst, "annotations", "a.xml"))
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T16_voc_clip_drop", rc == 0
        and got == ((240, 240), [("1", 0, 100, 60, 180)])
        and report_has(rep, "boxes_clipped", 1)
        and report_has(rep, "boxes_dropped", 1)
        and "<truncated>0</truncated>" in raw,      # 原字段不许被改（硬规定 3）
        "rc=%d got=%s" % (rc, got))

    # ---- T17 唯一的框被丢 -> 整张图和它的 XML 都不写 ----
    work, src, dst = new_case("t17", fmt="voc")
    mk_voc(src, "images/a.jpg", (320, 240), [("1", 0, 20, 10, 40)])
    rc, out, err = run_tool(tool, src, dst)
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T17_voc_drop_image", rc == 0
        and not os.path.isfile(os.path.join(dst, "images", "a.jpg"))
        and not os.path.isfile(os.path.join(dst, "annotations", "a.xml"))
        and report_has(rep, "images_dropped", 1),
        "rc=%d img=%s xml=%s" % (rc, os.path.isfile(os.path.join(dst, "images", "a.jpg")),
                                 os.path.isfile(os.path.join(dst, "annotations", "a.xml"))))

    # ---- T18 负样本（没有 XML）：图照裁，但**不造**空 XML ----
    work, src, dst = new_case("t18", fmt="voc")
    mk_voc(src, "images/n.jpg", (320, 240), None)
    rc, out, err = run_tool(tool, src, dst)
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T18_voc_negative", rc == 0
        and size_or_none(os.path.join(dst, "images", "n.jpg")) == (240, 240)
        and not os.path.exists(os.path.join(dst, "annotations", "n.xml"))
        and report_has(rep, "images_no_label", 1)
        and report_has(rep, "xml_written", 0),
        "rc=%d xml_exists=%s" % (rc, os.path.exists(os.path.join(dst, "annotations", "n.xml"))))

    # ---- T19 train.txt 被筛选重生成（被裁掉的图从清单里消失） ----
    work, src, dst = new_case("t19", fmt="voc")
    mk_voc(src, "images/a.jpg", (320, 240), [("1", 100, 80, 180, 160)])
    mk_voc(src, "images/b.jpg", (320, 240), [("1", 0, 20, 10, 40)], train=["a.jpg", "b.jpg"])
    rc, out, err = run_tool(tool, src, dst)
    rep = read_text(os.path.join(dst, "crop_report.txt"))
    rec("T19_voc_train", rc == 0
        and read_text(os.path.join(dst, "train.txt")) == "a.jpg\n"
        and report_has(rep, "train_lines_in", 2)
        and report_has(rep, "train_lines_out", 1)
        and report_has(rep, "train_lines_excluded", 1),
        "rc=%d train=%r" % (rc, read_text(os.path.join(dst, "train.txt"))))

    # ---- T20 XML 的 <size> 与图片真实尺寸不符 -> 非零退出（不校验就全错还看不出） ----
    work, src, dst = new_case("t20", fmt="voc")
    mk_voc(src, "images/a.jpg", (320, 240), [("1", 10, 20, 100, 120)], xml_size=(640, 480))
    rc, out, err = run_tool(tool, src, dst)
    rec("T20_voc_size_mismatch", rc == 3 and "image is 320x240" in err,
        "rc=%d err=%s" % (rc, err.strip().splitlines()[:1]))

    # ---- T21 bndbox 越出图片 -> 非零退出 ----
    work, src, dst = new_case("t21", fmt="voc")
    mk_voc(src, "images/a.jpg", (320, 240), [("1", 10, 20, 400, 300)])
    rc, out, err = run_tool(tool, src, dst)
    rec("T21_voc_out_of_range", rc == 3 and "outside image" in err,
        "rc=%d err=%s" % (rc, err.strip().splitlines()[:1]))

    # ---- T22 train.txt 里有一行根本不存在 -> 非零退出（不许静默训练少一张） ----
    work, src, dst = new_case("t22", fmt="voc")
    mk_voc(src, "images/a.jpg", (320, 240), [("1", 100, 80, 180, 160)],
           train=["a.jpg", "ghost.jpg"])
    rc, out, err = run_tool(tool, src, dst)
    rec("T22_voc_train_dangling", rc == 3 and "matches no image" in err,
        "rc=%d err=%s" % (rc, err.strip().splitlines()[:1]))

    # ---- T23 源数据集一个字节都不许变（硬约束 1）：图 + XML + train.txt ----
    work, src, dst = new_case("t23", fmt="voc")
    mk_voc(src, "images/a.jpg", (320, 240), [("1", 100, 80, 180, 160)], train=["a.jpg"])
    snap = {p: read_bytes(os.path.join(src, p))
            for p in ("images/a.jpg", "annotations/a.xml", "train.txt")}
    rc, out, err = run_tool(tool, src, dst)
    same = all(read_bytes(os.path.join(src, p)) == b for p, b in snap.items())
    rec("T23_voc_src_untouched", rc == 0 and same,
        "rc=%d src_same=%s" % (rc, same))

    return res


# --------------------------------------------------------------------------- #
# 变异反证
# --------------------------------------------------------------------------- #
CLAMP_OLD = ("    left, right = max(left, 0.0), min(right, float(S))\n"
             "    top, bottom = max(top, 0.0), min(bottom, float(S))")
CLAMP_NEW = "    left, right = left, right\n    top, bottom = top, bottom"

MUTATIONS = [
    ("M1_crop_offset_off_by_one",
     "    return S, (w - S) // 2, (h - S) // 2",
     "    return S, (w - S) // 2 + 1, (h - S) // 2",
     "T1_geometry"),
    ("M2_no_clamp_at_edges",
     CLAMP_OLD, CLAMP_NEW,
     "T3_clipped"),
    ("M3_ignore_center_policy",
     "    if not (x0 <= X < x0 + S and y0 <= Y < y0 + S):",
     "    if False:",
     "T2_outside"),
    ("M4_write_fully_dropped_images",
     "            if boxes and not (new_lines or boxes_new):",
     "            if False:",
     "T2_outside"),
    # ---- VOC 路径的四条 ----
    ("M5_voc_keep_old_bndbox",
     "        voc_set_box(el, box)",
     "        pass",
     "T15_voc_rewrite"),
    # 注：曾试过"去掉 voc_int_box 里的钳制"当 M6，**捕获不到** —— 因为 remap_box()
    # 先把框钳进 [0,S] 了，那 4 行钳制是**死代码**（防御性保留，注明见工具内注释）。
    # 变异必须能红，所以换成下面这条**真能触发**的：不更新 <size>。
    ("M6_voc_no_size_update",
     "    voc_set_size(root, S)",
     "    pass",
     "T15_voc_rewrite"),
    ("M7_voc_train_keeps_all",
     "                if base in written:",
     "                if True:",
     "T19_voc_train"),
    ("M8_voc_keep_dropped_objects",
     "        if id(o) not in kept_ids:",
     "        if False:",
     "T16_voc_clip_drop"),
]


def mutate(src_text, old, new):
    u"""做一处文字替换，并断言锚点**恰好命中 1 次**。

    命中 0 次 = 变异根本没生效 ⇒ 那种"变异被捕获"是假的（本工程第二十九次的坑：
    改了用例名忘了改调用处，脚本照样报满屏绿字）。
    """
    n = src_text.count(old)
    if n != 1:
        raise AssertionError("anchor hit %d times (expected exactly 1): %r" % (n, old[:60]))
    return src_text.replace(old, new)


def main():
    if not os.path.isfile(TOOL):
        sys.stdout.write("FAIL: tool not found: %s\n" % TOOL)
        return 1

    shutil.rmtree(SANDBOX, ignore_errors=True)
    os.makedirs(SANDBOX, exist_ok=True)

    sys.stdout.write("[test] self-test for %s\n" % os.path.basename(TOOL))

    base = run_suite(TOOL, os.path.join(SANDBOX, "base"))
    names = sorted(base)
    n_pass = sum(1 for n in names if base[n][0])
    sys.stdout.write("\n---- baseline (%d/%d) ----\n" % (n_pass, len(names)))
    for n in names:
        ok, msg = base[n]
        sys.stdout.write("  %-4s %-22s %s\n" % ("PASS" if ok else "FAIL", n,
                                                "" if ok else msg))
    baseline_ok = (n_pass == len(names))

    sys.stdout.write("\n---- mutation controls ----\n")
    with open(TOOL, "r", encoding="utf-8") as fh:
        src_text = fh.read()
    caught = 0
    mut_detail = []
    for (mname, old, new, expect_fail) in MUTATIONS:
        try:
            text = mutate(src_text, old, new)
        except AssertionError as exc:
            mut_detail.append("  BROKEN %-30s anchor problem: %s" % (mname, exc))
            continue
        mpath = os.path.join(SANDBOX, mname + ".py")
        with open(mpath, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        res = run_suite(mpath, os.path.join(SANDBOX, mname))
        failed = sorted(n for n in res if not res[n][0])
        hit = expect_fail in failed
        if hit:
            caught += 1
        mut_detail.append("  %-7s %-30s failed=%s (expected %s) %s"
                          % ("CAUGHT" if hit else "MISSED", mname,
                             ",".join(failed) if failed else "(none)",
                             expect_fail, "" if hit else "  <-- mutation NOT detected"))
    for line in mut_detail:
        sys.stdout.write(line + "\n")

    all_good = baseline_ok and caught == len(MUTATIONS)
    sys.stdout.write("\n[test] baseline %d/%d, mutations %d/%d caught -> %s\n"
                     % (n_pass, len(names), caught, len(MUTATIONS),
                        "OK" if all_good else "FAILED"))

    if all_good:
        shutil.rmtree(SANDBOX, ignore_errors=True)
        sys.stdout.write("[test] sandbox removed\n")
    else:
        sys.stdout.write("[test] sandbox kept for inspection: %s\n" % SANDBOX)
    return 0 if all_good else 1


if __name__ == "__main__":
    raise SystemExit(main())
