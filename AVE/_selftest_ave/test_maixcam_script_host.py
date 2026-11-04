# -*- coding: utf-8 -*-
u"""用**假 MaixPy** 把 `_maixcam/maixcam_track_gate.py` 的 `main()` 在桌面上**整个跑一遍**。

为什么值得做：
  这台机器上**没有 MaixCAM、也没有它的 Python 包** ⇒ 脚本里只要有一处笔误，
  就要你**上机跑到第 30 帧**才发现（一次往返 = 你的时间）。用假模块把它跑完，
  能覆盖：导入、构造、检测过滤、闸门、串口字节流、**以及那条刚改的打印语句**。

假模块必须**忠实**的地方（不忠实的假模块 = 自欺）：
  * `detect()` 要**真的按 `conf_th` 过滤** —— 否则「低分被滤掉」这条路径根本没被测到；
  * `app.need_exit()` 要**有限次** —— 否则死循环；
  * `uart.write()` 要**记下字节** —— 这样能直接断言串口上发了什么。

输出**纯 ASCII**（GBK 控制台会炸中文）。
"""
import io
import importlib.util
import os
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 允许用环境变量 `GATE_SRC` 换一份脚本 —— 这样能跑**负对照**：
# 「换一个 `CONF_TH`，测试还过不过？」过 = 测试真的跟着脚本走，
# 不是写死 0.55 之后**看着绿**的假绿（本文件第一版正是这种假绿）。
SRC = os.environ.get(u"GATE_SRC") or os.path.join(ROOT, u"_maixcam", u"maixcam_track_gate.py")


class Obj(object):
    def __init__(self, x, y, w, h, s, cid=0):
        self.x, self.y, self.w, self.h, self.score, self.class_id = x, y, w, h, s, cid


class FakeImg(object):
    def draw_rect(self, *a, **k):
        pass

    def draw_string(self, *a, **k):
        pass


class FakeDetector(object):
    SCEN = []       # 由 install() 填：每帧一个「obj 列表」

    def __init__(self, model=None, dual_buff=False, **kw):
        self.labels = [u"1"]
        self.n = 0

    def input_width(self):
        return 224

    def input_height(self):
        return 224

    def input_format(self):
        return u"rgb888"

    def detect(self, img, conf_th=0.5, iou_th=0.45):
        objs = self.SCEN[self.n] if self.n < len(self.SCEN) else []
        self.n += 1
        # ★ 诚实地按 conf_th 过滤 —— 假模块一旦「客气」，这条路径就等于没测
        return [o for o in objs if o.score >= conf_th]


def install(max_frames):
    u"""装一份假 MaixPy 到 sys.modules，返回记录串口字节的列表。

    场景**先留空**：要等脚本加载完、读到它真正的 `CONF_TH`，才能按那个阈值去配分数。
    ⚠️ 不这么做的话，脚本把 `CONF_TH` 从 0.35 抬到 0.60，测试里那颗 0.55 的「假阳性」
       会**连 `detect()` 都过不了** ⇒ 那几段测的就成了空场景、**闸门等于没测**，
       而且断言会以「pass 不为 0」的形式**假红**，看着像闸门坏了 —— 是**测试自己的错**。
    """
    written = []
    FakeDetector.SCEN = []      # 真正的场景在读到 script 的 CONF_TH 之后再填
    state = {u"i": 0}

    def need_exit():
        state[u"i"] += 1
        return state[u"i"] > max_frames

    class FakeCamera(object):
        def __init__(self, *a, **k):
            pass

        def read(self):
            return FakeImg()

    class FakeDisplay(object):
        def __init__(self, *a, **k):
            pass

        def show(self, img):
            pass

    class FakeUART(object):
        def __init__(self, *a, **k):
            pass

        def write(self, b):
            written.append(b)

    image = types.ModuleType(u"maix.image")
    image.COLOR_RED = 1
    image.COLOR_WHITE = 2
    cam_m = types.ModuleType(u"maix.camera")
    cam_m.Camera = FakeCamera
    disp_m = types.ModuleType(u"maix.display")
    disp_m.Display = FakeDisplay
    nn_m = types.ModuleType(u"maix.nn")
    nn_m.YOLOv5 = FakeDetector
    app_m = types.ModuleType(u"maix.app")
    app_m.need_exit = need_exit
    uart_m = types.ModuleType(u"maix.uart")
    uart_m.UART = FakeUART

    maix = types.ModuleType(u"maix")
    for name, m in ((u"camera", cam_m), (u"display", disp_m), (u"image", image),
                    (u"nn", nn_m), (u"app", app_m), (u"uart", uart_m)):
        setattr(maix, name, m)
        sys.modules[u"maix." + name] = m
    sys.modules[u"maix"] = maix
    return written


def load_script():
    spec = importlib.util.spec_from_file_location(u"gatecam", SRC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_scenario(ct):
    u"""4 个 30 帧的阶段，每段一个 30 帧的窗口（脚本每 30 帧打一行）。

    ⚠️⚠️ 假阳性的分数取 `ct + 0.05` —— 必须**刚刚高过脚本的 `CONF_TH`**。
       低于它就过不了 `detect()`，那几段测的就成了空场景、**闸门等于没测**。
       （本文件第一次跟着阈值改动时踩到的就是这个。）
    """
    fp = round(ct + 0.05, 2)    # 刚好高过 CONF_TH 的假阳性 —— 必须能走到闸门面前
    assert 0.30 < ct < 0.78, \
        "CONF_TH=%.2f 时本场景的前提（0.30 该被滤掉、0.82 该通过）不成立" % ct
    P_FP = (100, 80)          # 稳定假阳性：与你说「原地闪」那一类对应
    J = [(50, 50), (180, 160)]  # 跨位置跳的假阳性
    P_T = (112, 88)           # 真靶子
    scen = []
    # 段1：跨位置跳的假阳性 —— 闸门**应该**全挡
    for i in range(30):
        cx, cy = J[i % 2]
        scen.append([Obj(cx - 17, cy - 15, 34, 31, fp)])
    # 段2：稳定假阳性 —— 闸门**挡不住**，这就是你观测到的那个现象
    for i in range(30):
        scen.append([Obj(P_FP[0] - 17, P_FP[1] - 15, 34, 31, fp)])
    # 段3：真靶子（0.82）—— 应该锁上
    for i in range(30):
        scen.append([Obj(P_T[0] - 17, P_T[1] - 15, 34, 31, 0.82)])
    # 段4：空场景 + 一个 0.30 的低分框 —— 必须被 conf_th 滤掉（raw 应为 0）
    for i in range(30):
        scen.append([Obj(60, 60, 34, 31, 0.30)])
    return scen


def main():
    written = install(120)
    mod = load_script()
    # ★ 先加载脚本、读到它**真正的** `CONF_TH`，再按那个阈值造场景 ——
    #   否则脚本一改阈值，测试里的「假阳性」就被 conf_th 滤掉了，闸门等于没测（假红）。
    ct = mod.CONF_TH
    # ⚠️ 这一行必须**纯 ASCII** —— GBK 控制台会把 UTF-8 的中文解成乱码（本项目的老毛病）
    print("[test] script CONF_TH = %.2f -> fake-positive scenario uses %.2f"
          % (ct, round(ct + 0.05, 2)))
    scen = build_scenario(ct)
    FakeDetector.SCEN = scen

    buf = io.StringIO()
    old = sys.stdout
    sys.stdout = buf
    try:
        mod.main()
    finally:
        sys.stdout = old
    out = buf.getvalue()

    lines = [l for l in out.split(u"\n") if l.strip()]
    print("[script] printed %d non-empty lines" % len(lines))
    for l in lines:
        print("   | " + l if len(l) < 200 else "   | " + l[:200] + u"...")

    fails = []

    def check(name, cond, got):
        print("  %-4s %s   got=%s" % ("PASS" if cond else "FAIL", name, got))
        if not cond:
            fails.append(name)

    win = [l for l in lines if l.startswith(u"fps=")]
    check("4 windows printed", len(win) == 4, len(win))
    if len(win) != 4:
        return 1

    # ---- 段1：跨位置跳 ⇒ 闸门全挡（pass=0）----
    check("seg1 pass=0", u"pass=0" in win[0], win[0])
    # ---- 段2：稳定假阳性 ⇒ 闸门【挡不住】（pass 接近满）----
    p2 = int(win[1].split(u"pass=")[1].split()[0])
    check("seg2 pass>=25 (gate CANNOT reject it)", p2 >= 25, p2)
    check("seg2 sent=100,80,1", win[1].endswith(u"sent=100,80,1"), win[1][-20:])
    # ---- 段3：真靶子 ⇒ 锁上 ----
    check("seg3 sent=112,88,1", win[2].endswith(u"sent=112,88,1"), win[2][-20:])
    # ---- 段4：0.30 的框必须被 conf_th 滤掉 ----
    check("seg4 raw=0/30", u"raw=0/30" in win[3], win[3])

    # ---- 串口字节流：段1 里**不许出现**任何 ",1\n" ----
    txt = b"".join(written).decode("ascii")
    seg1 = txt.split("\n")[:30]
    check("uart seg1 none flagged", all(l.endswith(",0") for l in seg1), seg1[:4])
    check("uart seg2 locks", "100,80,1" in "\n".join(txt.split("\n")[30:60]),
          txt.split("\n")[30:34])
    check("uart frame count", len(txt.strip().split("\n")) == len(scen),
          len(txt.strip().split("\n")))

    # ---- 新打印语句的字段必须在（本文件就是为它写的）----
    detail = [l for l in lines if l.strip().startswith(u"sc ")]
    check("detail lines printed", len(detail) == 4, len(detail))
    if detail:
        for f in (u">=.50:", u">=.65:", u">=.80:", u"| box ", u"| cx ", u"| d(1f) "):
            check("detail has %r" % f, f in detail[0], detail[0][:80])
        # 段2/3 全是 0.55 / 0.82 ⇒ >=.80 计数应分别是 0 / 30
        g80_2 = int(detail[1].split(u">=.80:")[1].split()[0])
        g80_3 = int(detail[2].split(u">=.80:")[1].split()[0])
        check("seg2 >=.80 is 0", g80_2 == 0, g80_2)
        check("seg3 >=.80 is 30", g80_3 == 30, g80_3)

    print("\n%s  (%d fails)" % ("ALL PASS" if not fails else "FAILED: " + ",".join(fails),
                                len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
