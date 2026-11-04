# -*- coding: utf-8 -*-
u"""时序闸门的**桌面自测** —— 从 `_maixcam/maixcam_track_gate.py` **原样抽出**
`GATE_BEGIN..GATE_END` 那一段来跑，测的就是**将要烧进相机的那几个字节**，不是它的副本。

为什么必须这么抽（而不是在本文件里重写一遍逻辑）：
  重写 = 测的是**副本**，副本与正本一漂移，测试全绿而机器上照错 ——
  本工程栽过这个坑（`mutate_selftest.sh` 那轮的过期标签）。抽段 exec 消除了这条漂移。

输出**纯 ASCII**（GBK 控制台会炸中文，见 memory `gbk-console-encoding-traps`）。
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, u"_maixcam", u"maixcam_track_gate.py")


def load_gate():
    u"""抽出纯逻辑段并 exec，返回命名空间字典。"""
    src = io.open(SRC, encoding="utf-8").read()
    lines = src.split(u"\n")
    ib = ie = -1
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith(u"#") and u"GATE_BEGIN" in s:
            ib = i
        if s.startswith(u"#") and u"GATE_END" in s:
            ie = i
    assert ib >= 0 and ie > ib, "GATE markers not found (ib=%d ie=%d)" % (ib, ie)
    seg = u"\n".join(lines[ib + 1:ie])
    # 约束 C：这段必须**不含**任何 MaixPy 依赖，否则桌面根本跑不起来。
    #   ⚠️ 第一版写成「u'import' not in seg」⇒ 被**注释里**那句「不 import 任何东西」
    #   误判成泄漏（判据比它要量的事实宽）—— 改成只认真正的 import 语句。
    for i, l in enumerate(seg.split(u"\n")):
        s = l.strip()
        assert not (s.startswith(u"import ") or s.startswith(u"from ")), \
            "pure-logic segment imports at +%d: %s" % (i, s)
    for bad in (u"maix", u"camera.Camera", u"nn.YOLOv5"):
        assert bad not in seg, "pure-logic segment leaks %r" % bad
    assert u"def gate_step" in seg and u"def gate_new" in seg, "gate fns missing"
    ns = {}
    exec(compile(seg, "<gate>", "exec"), ns)
    return ns


def run(ns, raws, gate=None):
    u"""跑一段逐帧输入，返回每帧的 (cx,cy,flag) 列表；gate 传入则临时覆盖参数。"""
    G = ns[u"GATE"]
    saved = dict(G)
    if gate:
        G.update(gate)
    try:
        st = ns[u"gate_new"]()
        return [ns[u"gate_step"](st, r) for r in raws], st
    finally:
        G.clear()
        G.update(saved)


def main():
    ns = load_gate()
    G = ns[u"GATE"]
    print("[gate] HOLD_N=%d MATCH_R=%d LOST_N=%d" % (G["HOLD_N"], G["MATCH_R"], G["LOST_N"]))
    fails = []

    def check(name, cond, got):
        print("  %-4s %s   got=%s" % ("PASS" if cond else "FAIL", name, got))
        if not cond:
            fails.append(name)

    # ---- S1 静止真靶子：第 HOLD_N 帧起必须放行 ----
    P = (112, 88)
    outs, st = run(ns, [P] * 10)
    flags = [o[2] for o in outs]
    print("\nS1 steady real target at %s -> flags %s" % (P, flags))
    check("S1.flags", flags == [0, 0, 1, 1, 1, 1, 1, 1, 1, 1], flags)
    check("S1.pos", outs[2] == (P[0], P[1], 1), outs[2])
    check("S1.lockframe", flags.index(1) == G["HOLD_N"] - 1, flags.index(1))

    # ---- S2 跳变假阳性：永远不许放行（这就是本轮要挡的东西） ----
    alt = [(50, 50), (180, 160)] * 10
    outs, st = run(ns, alt)
    flags = [o[2] for o in outs]
    print("\nS2 jumping false positive (alternates far apart) -> flags %s" % flags)
    check("S2.allreject", set(flags) == {0}, sorted(set(flags)))
    check("S2.notlocked", st["locked"] is False, st["locked"])

    # ---- S3 【已知局限】抖动假阳性：在 MATCH_R 之内 ⇒ 闸门挡不住，如实断言 ----
    jit = [(112 + (i % 3) - 1, 88 + (i % 2) * 3) for i in range(10)]
    outs, st = run(ns, jit)
    flags = [o[2] for o in outs]
    print("\nS3 jittering FP within MATCH_R (KNOWN LIMIT) -> flags %s" % flags)
    check("S3.LIMIT_locks", flags[-1] == 1, flags[-1])
    print("       ^ 这不是 bug：MATCH_R 之内的抖动=「没动」。")
    print("         若实机直方图显示假阳性全落在 <20 档，则闸门无效，必须靠 conf_th。")

    # ---- S4/S5 已锁定后短暂丢帧：容忍 LOST_N 帧，超出才解锁 ----
    outs, st = run(ns, [P] * 5 + [None] * 5)
    flags = [o[2] for o in outs]
    print("\nS4/S5 locked then dropout x5 -> flags %s" % flags)
    check("S4.tol2", flags[5] == 1 and flags[6] == 1, flags[5:7])
    check("S4.holdpos", outs[6][:2] == P, outs[6])
    check("S5.dropat3", flags[7] == 0, flags[7])

    # ---- S6 已锁定后目标真的换地方并稳住：先守旧位置，再重新锁定到新位置 ----
    Q = (60, 60)
    outs, st = run(ns, [P] * 5 + [Q] * 6)
    seq = [o[:3] for o in outs[5:]]
    print("\nS6 locked, target truly relocates %s -> %s" % (Q, seq))
    check("S6.hold_old", outs[5][:2] == P and outs[5][2] == 1, outs[5])
    check("S6.unlock", outs[7][2] == 0, outs[7])
    check("S6.relock", outs[10][:3] == (Q[0], Q[1], 1), outs[10])

    # ---- 负对照：把闸门参数故意放宽 ⇒ S2 必须**不再**被挡住 ----
    #   （否则 S2 的「全 0」可能只是「反正谁也过不了」这种空判据）
    for tag, ov in ((u"MATCH_R huge", {u"MATCH_R": 10 ** 6}), (u"HOLD_N=1", {u"HOLD_N": 1})):
        outs, _ = run(ns, alt, gate={k.encode("ascii").decode("ascii"): v for k, v in ov.items()})
        flags = [o[2] for o in outs]
        print("\nNEGCTRL %-14s -> flags %s" % (tag, flags))
        check("NEGCTRL." + tag, 1 in flags, "has_pass=%s" % (1 in flags))

    print("\n%s  (%d/%d)" % ("ALL PASS" if not fails else "FAILED: " + ",".join(fails),
                             12 - len(fails), 12))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
