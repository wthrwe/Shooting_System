# -*- coding: utf-8 -*-
"""
跨端契约测试 —— 视觉端真正发出去的字节 × 固件端真正的解析器

── 它要回答的问题 ──────────────────────────────────────────────────────────
   `verify.md` 里长期挂着一条 ⬜：「视觉链路**没用真实数据验证过**」——
   视觉侧测过（`test_v5_window_stats.py`），固件侧也测过（`ave_m13_runtime.c` 的 V 组），
   但两侧各自用的都是**自己手写的样例**：固件侧喂的是我编的 `112,112,1`，
   视觉侧只检查自己写的字节长什么样。**从来没有人把一侧的真实产物喂进另一侧。**
   本文件补的就是这一刀。

── 两侧各自独立推导，互不抄 ────────────────────────────────────────────────
   视觉侧（本文件 Phase A）按 plan.md §6.3 的**文字条款**逐条查 `v5_send.py`
   真正 write 出去的字节。

   固件侧（`ave_vision_replay.c` + Phase B/C）把**同一串字节**喂给真产品解析器，
   只看它报出来的计数器。

   两侧的"什么是合法的"是**分别**从契约推出来的。若两侧共用一个实现
   （比如我在 Python 里再写一遍解析），那测的就只是"我抄得像不像"。

── 三阶段 ──────────────────────────────────────────────────────────────────
   Phase A  视觉端字节是否符合 §6.3（长度 / 字段数 / flag 取值 / 值域 / 行尾）
   Phase B  固件端收下这串真实字节：**零投诉**，且帧数对得上
   Phase C  负样本：往真实字节流中间**插一帧越界**（flag=1），
            必须被丢掉、且**不许失去同步**（其后真实帧照常解析）

── 硬约束（改这个文件之前先读）─────────────────────────────────────────────
   · **不写死 224**：画面尺寸从固件自己的 `AVE_tuning.h` 里读，
     依据是 `#define AVE_VISION_SCREEN_W`。单点真相在固件那头。
   · **不写死"60 帧"以外的假设**：帧数取自视觉端实际发出的条数，不靠猜。
   · 控制台输出**一律 ASCII**：本工程控制台是 GBK，输出「⬜」这类字符会直接抛异常
     （见 memory: gbk-console-encoding-traps）。中文只出现在本文件的注释里。
   · `subprocess` **不用 `text=True`**：那会在 GBK 控制台上炸（同上）。一律 `capture_output=True`
     再自己 `decode("utf-8", "replace")`。

临时验证脚手架，不进版本库、不进 Keil 工程。
"""

import os
import re
import subprocess
import sys

HERE   = os.path.dirname(os.path.abspath(__file__))
ROOT   = os.path.dirname(HERE)
PROJ   = os.path.join(ROOT, "AVE")
V5_SRC = os.path.join(ROOT, "maixpy", "v5_send.py")
TUNING = os.path.join(PROJ, "AVE_tuning.h")
BUILD  = os.path.join(HERE, "build")
REPLAY = os.path.join(BUILD, "replay.exe")
ARTIFACT = os.path.join(BUILD, "v5_real_lines.bin")

# 控制台可能不是 UTF-8。先把它掰成"绝不抛异常"，再保证自己只打印 ASCII。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

_g_pass = 0
_g_fail = 0


def chk(what, ok, detail=""):
    global _g_pass, _g_fail
    if ok:
        _g_pass += 1
        print("  ok   [%s]" % what)
    else:
        _g_fail += 1
        print("  FAIL [%s]  %s" % (what, detail))
    return bool(ok)


def screen_size():
    """画面尺寸从**固件自己的宏**读，不写死。"""
    txt = open(TUNING, "r", encoding="utf-8", errors="replace").read()
    w = re.search(r"#define\s+AVE_VISION_SCREEN_W\s+(\d+)", txt)
    h = re.search(r"#define\s+AVE_VISION_SCREEN_H\s+(\d+)", txt)
    if not (w and h):
        raise RuntimeError("在 AVE_tuning.h 里找不到 AVE_VISION_SCREEN_W/H")
    return int(w.group(1)), int(h.group(1))


def emit_real_lines():
    """
    跑 `maixpy/v5_send.py` **本体**（在假 maix 环境下），拿它真正 `ser.write()`
    出去的字节。复用 `test_v5_window_stats.py` 里那套假模块 —— 两边用同一套假环境，
    才能保证这里看到的字节与那一轮测过的字节是同一个东西。

    ⚠️ 这里**不改** v5_send.py 的任何一行。若哪天 v5_send.py 的 API 用法变了，
       这个函数会以 ImportError/AttributeError 的形式炸出来，而不是静默给出空结果。
    """
    sys.path.insert(0, HERE)
    import test_v5_window_stats as vh

    # ⚠️ `vh.run()` 收的是**源码文本**，不是路径（它内部是
    #    `exec(compile(src, TARGET, "exec"), g)`）。传路径进去会得到一句
    #    莫名其妙的 SyntaxError —— 本轮真踩了一次，留在这里省下一个人十分钟。
    src_text = open(V5_SRC, "r", encoding="utf-8").read()
    _out, g = vh.run(src_text, 60)
    ser = g.get("ser")
    if ser is None or not hasattr(ser, "sent"):
        raise RuntimeError("假环境里没拿到 Uart 替身（v5_send.py 的 API 用法可能变了）")
    return list(ser.sent)


def run_replay(path):
    """跑固件侧回放程序，返回 (退出码, stdout 文本)。"""
    p = subprocess.run([REPLAY, path], capture_output=True)
    return p.returncode, p.stdout.decode("utf-8", "replace")


CT_RE = re.compile(r"^\[CT\] (.+)$", re.M)

# ⚠️ 末尾那个 `\r?` 是**必须**的，不是随手加的：
#    回放程序是 MinGW 编的，Windows 文本模式会把 `printf` 的 '\n' 翻成 CRLF，
#    所以 stdout 里每行都以 '\r\n' 结尾。而**产品渲染出来的 [VS] 正文本身不带行尾**
#    —— 这个 '\r' 是宿主产物，板子上不一定有。判据必须容忍它，否则一条都匹配不上。
VS_RE = re.compile(
    r"^\[VS\] t=(\d+) st=(\d+) se=(\d+) ok=(\d+) dr=(\d+) sp=(\d+)"
    r" f=(-?\d+),(-?\d+),(\d+)\r?$", re.M)


def parse_ct(out):
    m = CT_RE.search(out)
    if not m:
        raise RuntimeError("回放程序没有输出 [CT] 汇总行")
    d = {}
    for kv in m.group(1).split():
        k, _sep, v = kv.partition("=")
        d[k] = v.strip()      # 同上的 CRLF 原因：值后面可能挂着一个 '\r'
    return d


def main():
    print("=== cross-side contract test: v5_send.py bytes  x  AVE_vision.c parser ===")

    if not os.path.exists(REPLAY):
        print("  !! replay exe not built: %s" % REPLAY)
        print("     run _selftest_ave/run_contract.sh instead of this file directly")
        return 3
    if not os.path.exists(V5_SRC):
        print("  !! not found: %s" % V5_SRC)
        return 3

    W, H = screen_size()
    print("firmware screen size (from AVE_tuning.h): %d x %d" % (W, H))

    # ---------------------------------------------------------------- Phase A
    print("\n-- Phase A: vision side -- the bytes v5_send.py really writes --")
    sent = emit_real_lines()
    raw = b"".join(sent)

    # 落一份原始字节，让"真字节"这件事可复核（hexdump 就能看）
    with open(ARTIFACT, "wb") as f:
        f.write(raw)
    print("  (raw bytes saved: %s, %d bytes)" % (os.path.relpath(ARTIFACT, ROOT), len(raw)))

    nlines = len(sent)
    chk("A1 one line per frame (60 frames)", nlines == 60, "got %d" % nlines)

    # 语法：^-?\d+,-?\d+,[01]\n$  —— plan.md §6.3 第一条与第五条
    bad_syntax = []
    for b in sent:
        if not b.endswith(b"\n"):
            bad_syntax.append(b)
            continue
        if not re.match(rb"^-?\d+,-?\d+,[01]$", b[:-1]):
            bad_syntax.append(b)
    chk("A2 every line is `cx,cy,flag\\n` with flag in {0,1}",
        not bad_syntax, "offending=%r" % bad_syntax[:3])

    # 行宽：协议契约 ≤32 字节（AVE_VISION_LINE_MAX）
    too_long = [b for b in sent if len(b) > 32]
    chk("A3 no line exceeds 32 bytes", not too_long,
        "offending=%r" % [(b, len(b)) for b in too_long[:3]])

    chk("A4 no CR anywhere (pure LF line ends)", b"\r" not in raw,
        "found CR at %d" % raw.find(b"\r"))

    # 值域：flag==1 的坐标必须落在固件画面内；flag==0 必须恰好是 -1,-1
    oob = []
    bad_zero = []
    for b in sent:
        cx, cy, fl = [int(x) for x in b[:-1].split(b",")]
        if fl == 1:
            if not (0 <= cx < W and 0 <= cy < H):
                oob.append((cx, cy))
        else:
            if (cx, cy) != (-1, -1):
                bad_zero.append((cx, cy))
    chk("A5 flag=1 coords all inside [0,%d)x[0,%d)" % (W, H), not oob,
        "out of range=%r" % oob[:5])
    chk("A6 flag=0 coords are exactly -1,-1", not bad_zero,
        "offending=%r" % bad_zero[:5])

    # ---------------------------------------------------------------- Phase B
    print("\n-- Phase B: firmware side -- replay those exact bytes --")
    rc, out = run_replay(ARTIFACT)
    chk("B1 replay program exited 0 (no ring-buffer drop)", rc == 0,
        "rc=%d" % rc)

    ct = parse_ct(out)
    lines = int(ct["lines"])
    ok = int(ct["ok"])
    nt = int(ct["no_target"])

    chk("B2 firmware saw all %d lines" % nlines, lines == nlines,
        "ct lines=%d" % lines)
    chk("B3 every line became a frame: ok + no_target == lines", ok + nt == lines,
        "ok=%d no_target=%d lines=%d" % (ok, nt, lines))
    chk("B4 proto_err == 0", int(ct["proto_err"]) == 0, "proto_err=%s" % ct["proto_err"])
    chk("B5 range_err == 0", int(ct["range_err"]) == 0, "range_err=%s" % ct["range_err"])
    chk("B6 line_spilled == 0", int(ct["line_spilled"]) == 0,
        "line_spilled=%s" % ct["line_spilled"])
    chk("B7 ring_flushed == 0", int(ct["ring_flushed"]) == 0,
        "ring_flushed=%s" % ct["ring_flushed"])
    chk("B8 blank == 0", int(ct["blank"]) == 0, "blank=%s" % ct["blank"])
    chk("B9 backlog == 0 (drain never hit the 16-line cap)", int(ct["backlog"]) == 0,
        "backlog=%s" % ct["backlog"])
    chk("B10 uart layer dropped nothing (drop_bytes/drop_lines == 0)",
        int(ct["drop_bytes"]) == 0 and int(ct["drop_lines"]) == 0,
        "drop_bytes=%s drop_lines=%s" % (ct["drop_bytes"], ct["drop_lines"]))
    chk("B11 flag0_coords == 0 (script never sends flag=0 with coords)",
        int(ct["flag0"]) == 0, "flag0=%s" % ct["flag0"])

    # [VS] 行本身也是契约的一部分：板子上就长这样
    vs = VS_RE.findall(out)
    chk("B12 at least one [VS] line was rendered", len(vs) >= 1,
        "found %d" % len(vs))
    if vs:
        last = vs[-1]
        # ⚠️ 这里**不能**写 `ok == lines`：`[VS]` 那行只印 ok 一个帧计数，
        #    **根本不印 no_target**（见 AVE_vision.c 的渲染格式）。真实数据里有
        #    几帧是 `-1,-1,0`（脚本那一帧没检出目标），它们**成功解析**成了
        #    kind=NO_TARGET，只是不计进 ok。本轮实测 ok=57 / no_target=3 / lines=60。
        #    把"无目标帧"当成"解析失败"，就会得到一个看起来很像产品 bug 的假红。
        #    真正的判据是：**[VS] 的 ok 与 [CT] 的 no_target 合起来必须铺满每一行** ——
        #    这两条是固件两个不同出口（渲染字符串 vs 直接读结构体）给出的数字，
        #    它们对得上，"全部收下"才算数。
        chk("B13 last [VS] ok + [CT] no_target == %d lines" % lines,
            int(last[3]) + nt == lines,
            "ok=%s no_target=%d lines=%d" % (last[3], nt, lines))
        chk("B13b last [VS] ok agrees with the final [CT] ok",
            int(last[3]) == ok, "[VS] ok=%s [CT] ok=%d" % (last[3], ok))
        chk("B14 last [VS] reports dr == 0 and sp == 0",
            int(last[4]) == 0 and int(last[5]) == 0,
            "dr=%s sp=%s" % (last[4], last[5]))
        chk("B15 the link never went stale during the run (st/se == 0)",
            int(last[1]) == 0 and int(last[2]) == 0,
            "st=%s se=%s" % (last[1], last[2]))
    chk("B16 published frame seq == number of frames (%d)" % lines,
        int(ct["seq"]) == lines, "seq=%s" % ct["seq"])

    # ---------------------------------------------------------------- Phase C
    print("\n-- Phase C: negative control -- inject one out-of-range frame mid-stream --")
    # 越界坐标由画面尺寸推出来（不写死 999），插在**中间**而不是末尾：
    # 插末尾只能证明"最后那帧被丢了"，插中间才能证明**丢帧之后没失去同步**。
    oor = b"%d,%d,1\n" % (W + 100, H + 100)
    half = nlines // 2
    raw2 = b"".join(sent[:half]) + oor + b"".join(sent[half:])
    path2 = os.path.join(BUILD, "v5_real_lines_plus_oor.bin")
    with open(path2, "wb") as f:
        f.write(raw2)

    rc2, out2 = run_replay(path2)
    chk("C1 replay exited 0", rc2 == 0, "rc=%d" % rc2)
    ct2 = parse_ct(out2)

    chk("C2 the injected frame was rejected: range_err == 1",
        int(ct2["range_err"]) == 1, "range_err=%s" % ct2["range_err"])
    chk("C3 it also counted as a protocol error: proto_err == 1",
        int(ct2["proto_err"]) == 1, "proto_err=%s" % ct2["proto_err"])
    chk("C4 it produced no frame: ok + no_target == lines - 1",
        int(ct2["ok"]) + int(ct2["no_target"]) == int(ct2["lines"]) - 1,
        "ok=%s no_target=%s lines=%s" % (ct2["ok"], ct2["no_target"], ct2["lines"]))
    # 这一条是关键：丢了一帧之后，**后面的真实帧一条都没少**。
    # 若解析器因此失去同步（例如把两行并成一行读），seq 就会小于 60。
    chk("C5 no loss of sync: published frame seq still == %d" % nlines,
        int(ct2["seq"]) == nlines, "seq=%s" % ct2["seq"])
    chk("C6 nothing else was disturbed (line_spilled/blank/drop_bytes == 0)",
        int(ct2["line_spilled"]) == 0 and int(ct2["blank"]) == 0
        and int(ct2["drop_bytes"]) == 0,
        "sp=%s blank=%s drop=%s" % (ct2["line_spilled"], ct2["blank"], ct2["drop_bytes"]))

    print("\n=== %d passed / %d failed ===" % (_g_pass, _g_fail))
    return 0 if _g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
