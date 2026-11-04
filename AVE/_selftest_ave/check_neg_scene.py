# -*- coding: utf-8 -*-
"""负样本实机验收：解析串口日志里的 [VS] 行，判定「无靶画面」是否真的被判为无目标。

为什么单独写一个、不复用 board_check.py：
  board_check.py 的判据 C3（"无目标帧 → f=-1,-1,1 / st=0 / se 不涨"）**已经覆盖了固件解析**，
  但它走的是**注入**路径 —— 由 PC 往串口写 `-1,-1,0`。
  本工具验的是**另一条独立的链**：真实 MaixCAM 对着真实空场景，由 v5 脚本真的发出无目标帧。
  ⚠️ **注入过 != 实机跑过**（第二十九次的原话：「全部通过」不是证据）。

判据落点：`AVE_vision.h:42-44` 的 kind（NONE=0 / NO_TARGET=1 / OK=2），
它被打在 `[VS]` 行 `f=` 的**第三个字段**上（`AVE_vision.c:461`）。

🔴🔴 最大的坑：**「屏幕上没框」在 kind=0 和 kind=1 下长得一模一样。**
    kind=0 = 一条合法帧都没收到（脚本没在发 / 线断了 / st=1）→ 是**链路故障**
    kind=1 = 合法的无目标帧收到了                              → 才是**我们想要的**
    ⇒ 只看 MaixCAM 屏幕 = 只对了一条链 ⇒ 正是本工程反复吃过的「假绿」。

输出全 ASCII（避开 GBK 控制台），`--selftest` 自带合成用例。
"""
import argparse
import io
import re
import sys

# ⚠️ 与 _selftest_ave/board_check.py:116 是**同一个正则**。改一处必须改两处，
#    否则两个工具会对同一份日志给出不同结论（本工程吃过"两处定义漂移"的亏）。
VS_RE = re.compile(
    r"^\[VS\] t=(\d+) st=(\d+) se=(\d+) ok=(\d+) dr=(\d+) sp=(\d+) "
    r"f=(-?\d+),(-?\d+),(\d+)")
VS_FIELDS = ("t", "st", "se", "ok", "dr", "sp", "fx", "fy", "kind")

KIND_NAME = {0: "NONE", 1: "NO_TARGET", 2: "OK"}


def parse(text):
    """从整份日志里抽出所有 [VS] 行，返回 dict 列表（顺序保持）。"""
    rows = []
    for ln in text.split("\n"):
        m = VS_RE.match(ln.strip())
        if m:
            rows.append(dict(zip(VS_FIELDS, (int(g) for g in m.groups()))))
    return rows


def analyse(rows, expect):
    """返回 [(code, name, verdict, detail)]；verdict ∈ PASS/FAIL/WARN/INV。

    INV = 证据不足，**不是通过**（窗口里一条 [VS] 都没有时，什么都证明不了）。
    """
    out = []
    if not rows:
        return [("N1", "log has >=1 [VS] line", "INV",
                 "no [VS] line at all -- nothing can be judged")]
    out.append(("N1", "log has >=1 [VS] line", "PASS", "%d lines" % len(rows)))

    hist = {}
    for r in rows:
        hist[r["kind"]] = hist.get(r["kind"], 0) + 1
    hist_s = ", ".join("%s=%d" % (KIND_NAME.get(k, "?"), v)
                       for k, v in sorted(hist.items()))

    # N2 —— 本次最关键的一条：链路必须活着。
    # kind=0 只会在"从未收到任何帧"时出现，所以它一出现就说明这些行不是实机视觉流。
    n0 = hist.get(0, 0)
    out.append(("N2", "no kind=0 (link/script alive)", "FAIL" if n0 else "PASS",
                "kind=0 x%d -- script not sending / wire broken" % n0 if n0
                else "kind=0 absent"))

    # N5 —— st 是"流健康"位：C4 已证 st=1 表示停发。
    bad_st = [r["t"] for r in rows if r["st"] != 0]
    out.append(("N5", "st == 0 throughout", "FAIL" if bad_st else "PASS",
                "st!=0 at t=%s" % bad_st[:5] if bad_st else "st=0 x%d" % len(rows)))

    if expect == "none":
        n2 = hist.get(2, 0)
        n1 = hist.get(1, 0)
        out.append(("N3", "no kind=2 (no false positive)", "FAIL" if n2 else "PASS",
                    "kind=2 x%d -- boxes drawn on an empty scene" % n2 if n2
                    else "kind=2 absent"))
        out.append(("N4", "kind=1 present (real no-target frames)",
                    "FAIL" if n1 == 0 else "PASS",
                    "kind=1 x%d" % n1 if n1 else
                    "no NO_TARGET frame -- firmware never classified one"))
        # 报文合法但自相矛盾：flag 说无目标、却带了范围内的坐标
        # （AVE_vision.c:184-190 会置 flag0_bad_coords）。
        odd = [r for r in rows if r["kind"] == 1 and (r["fx"] != -1 or r["fy"] != -1)]
        out.append(("N4b", "kind=1 coords normalized to -1,-1",
                    "WARN" if odd else "PASS",
                    "kind=1 with real coords x%d, e.g. f=%d,%d (flag0_bad_coords)"
                    % (len(odd), odd[0]["fx"], odd[0]["fy"]) if odd
                    else "all kind=1 are -1,-1"))
    elif expect == "target":
        good = [r for r in rows if r["kind"] == 2 and r["fx"] != -1]
        out.append(("N6", "kind=2 with real coords", "FAIL" if not good else "PASS",
                    "kind=2 count=%d, e.g. f=%d,%d"
                    % (len(good), good[0]["fx"], good[0]["fy"]) if good
                    else "no OK frame with real coords -- regression!"))

    out.append(("N7", "kind histogram", "PASS", hist_s))
    return out


def run(rows, expect):
    res = analyse(rows, expect)
    nf = sum(1 for r in res if r[2] == "FAIL")
    nw = sum(1 for r in res if r[2] == "WARN")
    ni = sum(1 for r in res if r[2] == "INV")
    for c, n, v, d in res:
        print("  [%-3s] %-6s %-38s %s" % (c, v, n, d))
    print("  ==> %s (FAIL=%d WARN=%d INV=%d)"
          % ("FAIL" if (nf or ni) else "PASS", nf, nw, ni))
    return nf, ni


def _vs(t, kind, fx=-1, fy=-1, st=0, se=0):
    return "[VS] t=%d st=%d se=%d ok=7 dr=0 sp=12345 f=%d,%d,%d" % (t, st, se, fx, fy, kind)


def selftest():
    """合成用例：每种 verdict 都要能被触发出**不同的**结论（判据必须能失败）。"""
    cases = [
        ("S1 clean empty scene -> PASS", "none",
         "\n".join(_vs(2000 + 2000 * i, 1) for i in range(4)), (0, 0)),
        ("S2 false positive -> FAIL N3", "none",
         "\n".join(_vs(2000 + 2000 * i, 1) for i in range(3)) + "\n" + _vs(10000, 2, 126, 122),
         (1, 0)),
        # 链路死会**同时**踩 N2（出现了 kind=0）与 N4（一条 kind=1 都没有）——
        # 两条 FAIL 都对。第一次写这个用例时我把 want 写成 (1,0)，自测当场逮住。
        ("S3 link dead (kind=0) -> FAIL N2+N4", "none",
         "\n".join(_vs(2000 + 2000 * i, 0) for i in range(3)), (2, 0)),
        ("S4 empty log -> INV", "none", "", (0, 1)),
        ("S5 stream stopped (st=1) -> FAIL N5", "none",
         "\n".join(_vs(2000 + 2000 * i, 1, st=1) for i in range(3)), (1, 0)),
        ("S6 target regression OK -> PASS", "target",
         "\n".join(_vs(2000 + 2000 * i, 2, 126, 122) for i in range(3)), (0, 0)),
        ("S7 expect target but none -> FAIL N6", "target",
         "\n".join(_vs(2000 + 2000 * i, 1) for i in range(3)), (1, 0)),
        ("S8 kind=1 with real coords -> WARN", "none",
         _vs(2000, 1, 100, 90) + "\n" + _vs(4000, 1), (0, 0)),
    ]
    bad = 0
    for name, expect, text, want in cases:
        print("--- %s" % name)
        got = run(parse(text), expect)
        if got != want:
            print("  SELFTEST MISMATCH: got %s want %s" % (got, want))
            bad += 1
    print("")
    print("[selftest] %d/%d cases behaved as expected" % (len(cases) - bad, len(cases)))
    # 反向验活：故意把正则改坏，必须看出"全部 INV"（证明 N1 不是恒真的摆设）
    global VS_RE
    old = VS_RE
    VS_RE = re.compile(r"^\[NOPE\]")
    degenerate = run(parse("\n".join(_vs(2000 + 2000 * i, 1) for i in range(3))), "none")
    VS_RE = old
    ok_degenerate = (degenerate == (0, 1))
    print("[selftest] regex-broken control: %s (expect INV, got %s)"
          % ("OK" if ok_degenerate else "BAD", degenerate))
    if not ok_degenerate:
        bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", help="serial capture file")
    ap.add_argument("--expect", choices=("none", "target"), default="none",
                    help="scene should have no target / a target")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        return 1 if selftest() else 0
    if not a.log:
        print("need --log <file> or --selftest")
        return 2

    # errors="replace"：串口日志里可能混着半个多字节字符，别让它把分析整个噎死
    text = io.open(a.log, "r", encoding="utf-8", errors="replace").read()
    rows = parse(text)
    print("[neg-scene] %s  expect=%s  [VS] lines=%d" % (a.log, a.expect, len(rows)))
    nf, ni = run(rows, a.expect)
    return 1 if (nf or ni) else 0


if __name__ == "__main__":
    sys.exit(main())
