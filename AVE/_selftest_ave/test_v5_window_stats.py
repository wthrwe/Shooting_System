# -*- coding: utf-8 -*-
"""
test_v5_window_stats.py -- v5_send.py 的【桌面端到端自测】（规则 2）
==================================================================
被测物：AVE/maixpy/v5_send.py（2026-10-13 加了 [WIN] 观测窗口统计与 CONF_TH 常量）

为什么要在桌面上"假装"跑一遍？
  `check_v5_script.py` 只做**静态**检查（AST / 正则），它证明不了：
    · `CONF_TH` 是不是真的传进了 `detect()`（可能是拼错名字、或者没接上）
    · `[WIN]` 那行的算术对不对（det / lost / 分数带 / cx·cy·w·h 跨度）
    · 加了统计之后，串口那一行**有没有被改坏**（协议一个字都不许动）
  ⇒ 本脚本注入一套**假 maix 模块**，把被测脚本原样 exec 一遍，喂 60 帧剧本，
    再逐条断言。它是"能证伪"的：变异表里的每一处改动都**必须**让它红。

本脚本能证明什么 / 不能证明什么（诚实边界）：
  ✅ 能证明：脚本语法可跑；主循环真的转 60 圈；`ser.write` 恰好 60 次且内容合规；
             `CONF_TH=0.70` 真的到了 `detect()`；`[WIN]` 的 det/lost/分数带/坐标跨度/框宽高跨度
             在**已知剧本**下与手算值一致；统计不写串口。
  ⬜ 不能证明：`from maix import ...` 在真 MaixCAM 上可用；真相机真模型的行为；
             串口真的到了 PB7；STM32 侧 `[VS]` 的读数。那几条要上硬件。

用法：
  python test_v5_window_stats.py              # 基线（必须 全绿）
  python test_v5_window_stats.py --mutate MX1 # 变异（判据【必须】红）
"""
import contextlib
import io
import os
import re
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.abspath(os.path.join(HERE, "..", "maixpy", "v5_send.py"))

# 2026-10-19 起 cx/cy 是**靶心**，不是框中心：
#   cx = 框中心 + 0.17·w，cy = 框中心 + 0.07·h（视作浮点后 int 截断）。
#   因此本剧本的期望值全部带上这两个偏置；窗口 1 的框中心仍是 100..109 / 80..84，
#   但发出去的是 125..135 / 89..93。别再按旧值改回来。
# [WIN] det=30/30 lost=0 sc=0.72/0.79/0.86 cx=125..135(span 10) cy=89..93(span 4)
#   w=150..156(span 6) h=130..134(span 4)
# 组序号：0-2 det/lost、3-5 分数带、6-8 cx、9-11 cy、12-14 w、15-17 h
WIN_RE = re.compile(
    r"\[WIN\] det=(\d+)/(\d+) lost=(\d+) "
    r"sc=([\d.]+)/([\d.]+)/([\d.]+) "
    r"cx=(-?\d+)\.\.(-?\d+)\(span (-?\d+)\) "
    r"cy=(-?\d+)\.\.(-?\d+)\(span (-?\d+)\) "
    r"w=(-?\d+)\.\.(-?\d+)\(span (-?\d+)\) "
    r"h=(-?\d+)\.\.(-?\d+)\(span (-?\d+)\)")


# ---------------------------------------------------------------------------
# 假 maix：只实现被测脚本真正用到的那几个 API，其余一概没有
#   用例图：cam.read() -> detector.detect() -> draw_rect/draw_string -> disp.show
# ---------------------------------------------------------------------------
class Obj:
    """一个检测框。x/y 是【左上角】（与真 MaixPy 的 obj.x/.y 同义）。"""
    def __init__(self, x, y, w, h, score, cid=0):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.score, self.class_id = score, cid


class Img:
    def draw_rect(self, *a, **k):
        pass

    def draw_string(self, *a, **k):
        pass


class Cam:
    def __init__(self, w, h, fmt):
        self.w, self.h, self.fmt = w, h, fmt

    def read(self):
        return Img()


class Det:
    """假的 nn.YOLOv5：按剧本逐帧吐框。"""
    labels = ["target"]
    plan = []          # 每帧一个 list[Obj]；由 make_frames() 填，exec 前设好

    def __init__(self, model=None, dual_buff=False):
        self.i = -1
        self.conf_ths = []      # 收到过的 conf_th，用来验 CONF_TH 真接上了

    def input_width(self):
        return 224

    def input_height(self):
        return 224

    def input_format(self):
        return "RGB888"

    def detect(self, img, conf_th=0.5, iou_th=0.45):
        self.i += 1
        self.conf_ths.append(conf_th)
        return Det.plan[self.i] if self.i < len(Det.plan) else []


class Disp:
    def show(self, img):
        pass


class Uart:
    def __init__(self, dev, baud):
        self.dev, self.baud = dev, baud
        self.sent = []

    def write(self, b):
        self.sent.append(b)


class App:
    def __init__(self, nframes):
        self.n, self.max = 0, nframes

    def need_exit(self):
        r = self.n >= self.max
        self.n += 1
        return r


def fake_time_module():
    """假的 time：每次取时间都往前走 0.05 s。

    真 time 会让 `now - t0` 等于 0 ⇒ `n / (now - t0)` 抛 ZeroDivisionError
    （桌面跑 60 帧用不到 1 ms）⇒ 那是**测试环境的假故障**，不是被测脚本的问题。
    """
    m = types.ModuleType("time")
    st = {"t": 1000.0}

    def _t():
        st["t"] += 0.05
        return st["t"]

    m.time = _t
    return m


# ---------------------------------------------------------------------------
# 剧本：60 帧 = 两个 30 帧观测窗口
# ---------------------------------------------------------------------------
def make_frames():
    frames = []
    # 窗口 1（帧 0..29）：30 帧全检出
    #   **框中心** cx = 100..109、cy = 80..84、sc = 0.72..0.86（均值 0.79）
    #   w = 150..156（跨度 6）、h = 130..134（跨度 4）
    #   ⚠️ 下面 place 的是框；发出去的靶心 = 框中心 + 0.17w / +0.07h ⇒ 125..135 / 89..93
    # 🔴 宽高**故意逐帧变化**。第一版把 w/h 写死成 156/138，跨度恒为 0 ——
    #    那样的剧本里"跨度算错"的变异也能过，"忘了复位"的变异更是完全抓不到。
    #    判据要有区分力，就得让被测的量在剧本里真的动起来。
    for i in range(30):
        cx, cy = 100 + i % 10, 80 + i % 5
        sc = 0.72 + 0.14 * (i / 29.0)
        w, h = 150 + i % 7, 130 + i % 5
        # 左上角 = 中心 − 边长//2；奇偶边长都行（int() 截断后中心仍精确等于 cx/cy）
        frames.append([Obj(cx - w // 2, cy - h // 2, w, h, sc)])
    # 窗口 2（帧 30..59）：帧 34 / 50 / 51 丢锁
    #   ⇒ det=27、lost=2（34 一次、50 一次；51 因为前帧也没检出，不重复计）
    #   ⇒ **框中心** cx = 120..124、cy = 90..92、sc = 0.70..0.80
    #   ⇒ w = 160..163（跨度 3）、h = 140..145（跨度 5）
    #   ⚠️ 发出去的靶心 = 框中心 + 0.17w / +0.07h ⇒ 147..152 / 99..102
    #      （奇数边长会让框中心多 0.5 px，所以最大值会比"中心+27"再高 1 px）
    # ⚠️ 窗口 2 的宽高区间与窗口 1 **故意不重叠**（150..156 vs 160..163）：
    #    这样"窗口结束时忘了复位 wlo/whi"的变异会让窗口 2 报出 w=150..163，
    #    判据立刻红。若两窗区间重叠，那个变异会静默存活。
    for f in range(30, 60):
        if f in (34, 50, 51):
            frames.append([])
            continue
        j = f - 30
        cx, cy = 120 + f % 5, 90 + f % 3
        sc = 0.70 + 0.10 * (j / 29.0)
        w, h = 160 + j % 4, 140 + j % 6
        frames.append([Obj(cx - w // 2, cy - h // 2, w, h, sc)])
    return frames


def run(src, nframes=60):
    """把被测源码 exec 一遍，返回 (stdout 文本, 脚本的全局名字空间)。

    🔴 用【假 time】覆盖 sys.modules["time"]：被测脚本里 `import time` 会直接拿到
       假模块（import 先查 sys.modules）。跑完必须还原，否则污染本进程。
    """
    Det.plan = make_frames()
    app_inst = App(nframes)

    fake_app = types.ModuleType("maix.app")
    fake_app.need_exit = app_inst.need_exit
    fake_camera = types.ModuleType("maix.camera")
    fake_camera.Camera = Cam
    fake_display = types.ModuleType("maix.display")
    fake_display.Display = Disp
    fake_image = types.ModuleType("maix.image")
    fake_image.COLOR_RED = 0xFF0000
    fake_nn = types.ModuleType("maix.nn")
    fake_nn.YOLOv5 = Det
    fake_uart = types.ModuleType("maix.uart")
    fake_uart.UART = Uart
    fake_maix = types.ModuleType("maix")
    fake_maix.camera, fake_maix.display = fake_camera, fake_display
    fake_maix.image, fake_maix.nn = fake_image, fake_nn
    fake_maix.app, fake_maix.uart = fake_app, fake_uart

    mods = {"maix": fake_maix, "maix.camera": fake_camera,
            "maix.display": fake_display, "maix.image": fake_image,
            "maix.nn": fake_nn, "maix.app": fake_app, "maix.uart": fake_uart,
            "time": fake_time_module()}
    saved = {k: sys.modules.get(k) for k in mods}
    sys.modules.update(mods)

    buf = io.StringIO()
    g = {"__name__": "__v5_under_test__", "__file__": TARGET}
    try:
        with contextlib.redirect_stdout(buf):
            exec(compile(src, TARGET, "exec"), g)
    finally:
        for k, v in saved.items():
            if v is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = v
    return buf.getvalue(), g


def checks(src):
    out, g = run(src)
    res = []

    def chk(name, ok, detail=""):
        res.append((name, bool(ok), detail))

    ser, det = g.get("ser"), g.get("detector")

    # ---- ① 串口那一行：协议一个字都没动 ----
    sent = ser.sent
    chk("serial: exactly 60 writes (one per frame)", len(sent) == 60,
        "got %d" % len(sent))
    bad_fmt = [b for b in sent
               if not re.fullmatch(rb"-?\d+,-?\d+,[01]\n", b)]
    chk("serial: every line matches cx,cy,flag\\n", not bad_fmt,
        "bad=%r" % bad_fmt[:3])
    over = [b for b in sent if len(b) > 32]
    chk("serial: every line <= 32 bytes", not over, "over=%r" % over[:3])
    # 框中心 (100,80)、w=150、h=130 ⇒ 靶心 = (100+25.5, 80+9.1) = (125,89)
    chk("serial: frame 0 == '125,89,1\\n' (box centre + 0.17w,0.07h)",
        sent[0] == b"125,89,1\n", "got %r" % sent[0])
    chk("serial: lost frame 34 == '-1,-1,0\\n'", sent[34] == b"-1,-1,0\n",
        "got %r" % sent[34])
    chk("serial: lost frame 50 == '-1,-1,0\\n'", sent[50] == b"-1,-1,0\n",
        "got %r" % sent[50])
    # 框中心 (124,92)、w=161（奇 ⇒ 中心多 0.5）、h=145 ⇒ 靶心 = (124+0.5+27.37, 92+0.5+10.15) ⇒ (151,102)
    chk("serial: frame 59 == '151,102,1\\n'", sent[59] == b"151,102,1\n",
        "got %r" % sent[59])

    # ---- ② CONF_TH 真的接到了 detect() ----
    ths = set(det.conf_ths)
    chk("detect() receives CONF_TH (== 0.70)", ths == {0.70},
        "seen=%r" % sorted(ths))

    # ---- ③ [WIN] 统计：窗口 1 ----
    wins = WIN_RE.findall(out)
    chk("[WIN] window count == 2", len(wins) == 2, "got %d" % len(wins))
    if len(wins) == 2:
        w1, w2 = wins
        chk("[WIN] w1 det/total", (int(w1[0]), int(w1[1])) == (30, 30),
            "got %s/%s" % (w1[0], w1[1]))
        chk("[WIN] w1 lost == 0 (no fragmented lock)", int(w1[2]) == 0,
            "got %s" % w1[2])
        chk("[WIN] w1 score band 0.72/0.79/0.86",
            abs(float(w1[3]) - 0.72) < 5e-3 and abs(float(w1[4]) - 0.79) < 5e-3
            and abs(float(w1[5]) - 0.86) < 5e-3,
            "got %s/%s/%s" % (w1[3], w1[4], w1[5]))
        chk("[WIN] w1 cx 125..135 span 10 (bullseye, not box centre)",
            (int(w1[6]), int(w1[7]), int(w1[8])) == (125, 135, 10),
            "got %s..%s(%s)" % (w1[6], w1[7], w1[8]))
        chk("[WIN] w1 cy 89..93 span 4 (bullseye, not box centre)",
            (int(w1[9]), int(w1[10]), int(w1[11])) == (89, 93, 4),
            "got %s..%s(%s)" % (w1[9], w1[10], w1[11]))
        chk("[WIN] w1 box w 150..156 span 6",
            (int(w1[12]), int(w1[13]), int(w1[14])) == (150, 156, 6),
            "got %s..%s(%s)" % (w1[12], w1[13], w1[14]))
        chk("[WIN] w1 box h 130..134 span 4",
            (int(w1[15]), int(w1[16]), int(w1[17])) == (130, 134, 4),
            "got %s..%s(%s)" % (w1[15], w1[16], w1[17]))

        # ---- ④ [WIN] 统计：窗口 2（含丢锁）----
        chk("[WIN] w2 det == 27/30", (int(w2[0]), int(w2[1])) == (27, 30),
            "got %s/%s" % (w2[0], w2[1]))
        chk("[WIN] w2 lost == 2 (two lock breaks)", int(w2[2]) == 2,
            "got %s" % w2[2])
        chk("[WIN] w2 score band 0.70/0.75/0.80",
            abs(float(w2[3]) - 0.70) < 5e-3 and abs(float(w2[4]) - 0.75) < 5e-3
            and abs(float(w2[5]) - 0.80) < 5e-3,
            "got %s/%s/%s" % (w2[3], w2[4], w2[5]))
        chk("[WIN] w2 cx 147..152 span 5 (bullseye, not box centre)",
            (int(w2[6]), int(w2[7]), int(w2[8])) == (147, 152, 5),
            "got %s..%s(%s)" % (w2[6], w2[7], w2[8]))
        chk("[WIN] w2 cy 99..102 span 3 (bullseye, not box centre)",
            (int(w2[9]), int(w2[10]), int(w2[11])) == (99, 102, 3),
            "got %s..%s(%s)" % (w2[9], w2[10], w2[11]))
        chk("[WIN] w2 box w 160..163 span 3 (accumulators were reset)",
            (int(w2[12]), int(w2[13]), int(w2[14])) == (160, 163, 3),
            "got %s..%s(%s)" % (w2[12], w2[13], w2[14]))
        chk("[WIN] w2 box h 140..145 span 5",
            (int(w2[15]), int(w2[16]), int(w2[17])) == (140, 145, 5),
            "got %s..%s(%s)" % (w2[15], w2[16], w2[17]))
    else:
        chk("[WIN] w1 det/total", False, "window count != 2")
        chk("[WIN] w1 lost == 0 (no fragmented lock)", False, "window count != 2")
        chk("[WIN] w1 score band 0.72/0.79/0.86", False, "window count != 2")
        chk("[WIN] w1 cx 125..135 span 10 (bullseye, not box centre)", False,
            "window count != 2")
        chk("[WIN] w1 cy 89..93 span 4 (bullseye, not box centre)", False,
            "window count != 2")
        chk("[WIN] w2 det == 27/30", False, "window count != 2")
        chk("[WIN] w2 lost == 2 (two lock breaks)", False, "window count != 2")
        chk("[WIN] w2 score band 0.70/0.75/0.80", False, "window count != 2")
        chk("[WIN] w2 cx 147..152 span 5 (bullseye, not box centre)", False,
            "window count != 2")
        chk("[WIN] w2 cy 99..102 span 3 (bullseye, not box centre)", False,
            "window count != 2")
        chk("[WIN] w1 box w 150..156 span 6", False, "window count != 2")
        chk("[WIN] w1 box h 130..134 span 4", False, "window count != 2")
        chk("[WIN] w2 box w 160..163 span 3 (accumulators were reset)", False,
            "window count != 2")
        chk("[WIN] w2 box h 140..145 span 5", False, "window count != 2")

    # ---- ⑤ 负数对照：统计绝不能出现在串口上 ----
    leaked = [b for b in sent if b"[WIN]" in b]
    chk("negative control: [WIN] never goes to UART", not leaked, "%r" % leaked)

    return res


# --- 变异表：改坏一处，上面某条判据【必须】红 ---
MUTATIONS = {
    # 丢锁计数口径错 ⇒ 窗口 2 的 lost 会差
    "MX1": ("        lost += 1", "        lost += 0", "[WIN] w2 lost == 2"),
    # 阈值没接上（回到历史值 0.4）⇒ 断言必须红
    "MX2": ("\nCONF_TH = 0.70\n", "\nCONF_TH = 0.40\n",
            "detect() receives CONF_TH (== 0.70)"),
    # 跨度算错（把 max 写成 min）⇒ cx 跨度判据必须红
    "MX3": ("        if cx > cxh: cxh = cx", "        if cx > cxh: cxh = cx - 1",
            "[WIN] w1 cx 125..135 span 10"),
    # 框宽的上界算错 ⇒ w 跨度判据必须红（这一条专门盯新加的 wh 字段）
    "MX4": ("        if o.w > whi: whi = o.w", "        if o.w > whi: whi = o.w - 1",
            "[WIN] w1 box w 150..156 span 6"),
    # 窗口结束时忘了复位框宽高累加器 ⇒ 窗口 2 会带上窗口 1 的下界 150
    "MX5": ("        wlo, whi, hlo, hhi = W, -1, H, -1\n", "",
            "[WIN] w2 box w 160..163 span 3"),
    # 靶心偏置被删掉（退回框中心）⇒ 发出去的坐标必须红。
    # 这条是 2026-10-19 新增的"判据能证伪"锚点：没有它，以后有人把偏置删了，
    # 其余断言（det/lost/w/h/分数带）全都照样绿。
    "MX6": ("min(int(o.x + o.w / 2 + 0.17 * o.w), W - 1)",
            "min(int(o.x + o.w / 2), W - 1)",
            "serial: frame 0 == '125,89,1"),
}


def main():
    with open(TARGET, "r", encoding="utf-8") as f:
        src = f.read()

    if len(sys.argv) >= 3 and sys.argv[1] == "--mutate":
        name = sys.argv[2]
        old, new, expect = MUTATIONS[name]
        if src.count(old) != 1:
            print("MUTATION-ANCHOR-NOT-UNIQUE %s (count=%d)" % (name, src.count(old)))
            return 3
        res = checks(src.replace(old, new, 1))
        failed = [n for (n, ok, d) in res if not ok]
        # 🔴 期望串按【子串】匹配，不按列表成员精确匹配：
        #    第一版写成 `expect in failed`（列表成员），而 MX1 的期望串写成了缩写
        #    "[WIN] w2 lost == 2"，真名是 "[WIN] w2 lost == 2 (two lock breaks)"
        #    ⇒ 变异其实【已被抓到】，却被报成 SURVIVED（假红）。
        #    现象：打印里 failures=[...] 明明就有那一条，只是名字更长。
        hit = [n for n in failed if expect in n]
        if hit:
            print("MUTANT %s CAUGHT by '%s'  (other failures: %s)"
                  % (name, hit[0], [x for x in failed if x not in hit]))
            return 0
        print("MUTANT %s SURVIVED -- expected '%s' to fail; failures=%s"
              % (name, expect, failed))
        return 1

    res = checks(src)
    nfail = 0
    for name, ok, detail in res:
        if not ok:
            nfail += 1
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  <- %s" % detail) if (detail and not ok) else ""))
    print("RESULT: %d passed / %d failed" % (len(res) - nfail, nfail))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
