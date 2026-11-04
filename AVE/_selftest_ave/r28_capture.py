#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 28 次抓包脚本 —— 读 COM4，抓开机 [RST] / [TIM] 与随后的 [HB] / [VS]

用途：把「板子到底打出了什么」原样存盘，供 verify.md 引用。
不属于产品代码，不进 Keil，不进工程根目录。

🔴 本脚本存在的唯一理由是【把两种失败分开报】：
     「端口打不开」     —— Windows 的 COM 是独占的，被串口助手占着就报 PermissionError。
                           这跟"板子没在发"是两件事，混起来就会去查错方向。
     「打开了但 0 字节」 —— 这才是"板子没发"，往板子没上电 / 停在 halt 查。
   本工程已经为这条判据误诊过一次（见 risk 表 #11），所以在这里写死。

用法：
    python r28_capture.py                        # 默认 COM4 / 115200 / 10 秒
    python r28_capture.py --port COM99           # 负向测试：验"端口打不开"这条路
    python r28_capture.py --seconds 20 --out x.log
"""

import argparse
import sys
import time

# Windows 上 Python 默认按 cp936 编码 stdout，而终端（mintty/VSCode）按 UTF-8 解释
# ⇒ 中文全成乱码、`⇒` 退化成字面量 `⇒`。抓包脚本的产出就是「给人看的失败信息」，
# 读不出来等于没有，所以这里强制 UTF-8。errors="replace" 保证任何字节都不会让脚本自己崩。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):   # 被重定向到不支持 reconfigure 的对象时忽略
        pass

TAGS = ("[RST]", "[TIM]", "[HB]", "[VS]")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--out", default="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave/r28_capture_raw.txt")
    a = ap.parse_args()

    try:
        import serial  # 延迟导入：这样连 pyserial 都没装时，报错也清楚
    except ImportError:
        print("FAIL: 没有 pyserial", file=sys.stderr)
        return 3

    # ---- 第一步：能不能打开。打不开 ≠ 板子没在发 ----
    try:
        sp = serial.Serial(a.port, a.baud, timeout=0.2)
    except serial.SerialException as e:
        print(f"[端口打不开] {a.port}: {e}", file=sys.stderr)
        print("  ⇒ 先查是不是被串口助手/别的程序占着（PermissionError 类）。", file=sys.stderr)
        print("  ⇒ 这一条【不能】当作『板子没在发』。", file=sys.stderr)
        return 1

    print(f"已打开 {a.port} @ {a.baud} 8N1，抓 {a.seconds} 秒 ...")

    lines, raw = [], bytearray()
    t0 = time.time()
    try:
        while time.time() - t0 < a.seconds:
            chunk = sp.read(256)
            if not chunk:
                continue
            raw.extend(chunk)
            # 按行攒：串口是字节流，一次 read 未必正好一行
            for ln in chunk.split(b"\n"):
                if ln:
                    lines.append((time.time() - t0, ln.rstrip(b"\r").decode("utf-8", "replace")))
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    finally:
        sp.close()

    print(f"\n--- 共收到 {len(raw)} 字节 / {len(lines)} 行 ---")

    with open(a.out, "wb") as f:          # 原样存盘，不做任何加工
        f.write(bytes(raw))
    print(f"原始字节已存：{a.out}")

    # ---- 第二步：打开成功但没字节，这才是"板子没发" ----
    if not raw:
        print("FAIL: 端口打开成功、但 0 字节。", file=sys.stderr)
        print("  ⇒ 这【不是】占用问题。往：板子没上电 / 停在 halt（Keil 的 Reset and Run 没勾）", file=sys.stderr)
        print("  ⇒ 判据：按一次复位后，第一行应当立刻是 t=0。", file=sys.stderr)
        return 2

    # ---- 第三步：把本轮关心的两类行摘出来 ----
    print("\n--- 分类统计 ---")
    for tag in TAGS:
        hit = [l for _, l in lines if l.startswith(tag)]
        print(f"  {tag:6s} {len(hit):3d} 行")

    rst = [l for _, l in lines if l.startswith("[RST]")]
    tim = [l for _, l in lines if l.startswith("[TIM]")]
    print("\n--- 关键行原文 ---")
    for l in rst:
        print("  RST:", l)
    for l in tim:
        print("  TIM:", l)

    # 顺序判据（V4）：[RST] 与 [TIM] 的先后，以及它们是否都在第一条 [HB] 之前
    order = [i for i, (_, l) in enumerate(lines) if l.startswith(TAGS)]
    if rst and tim and len(order) >= 2:
        print(f"\n  [RST] 首次出现在第 {order[0] + 1} 行，[TIM] 在第 {order[1] + 1} 行")
    hb = [l for _, l in lines if l.startswith("[HB]")]
    if hb:
        print(f"  第一条 [HB]: {hb[0]}")
    else:
        print("  ⚠️ 一条 [HB] 都没有 —— 抓的时间太短，或调度器没起来")
    return 0


if __name__ == "__main__":
    sys.exit(main())
