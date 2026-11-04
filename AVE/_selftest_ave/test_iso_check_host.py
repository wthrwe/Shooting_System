# -*- coding: utf-8 -*-
u"""用**假 MaixPy** 把 `_maixcam/_iso_check.py` 跑一遍。

为什么值得跑：
  这台机器上**没有 MaixCAM** ⇒ 一个拼错的 API 名（`draw_circle` 写成 `draw_cicle`）
  要你**上机跑一遍**才发现。更要命的是**尺子画错** —— 半径超出画面、圆心算错、画到角上，
  判据本身就废了，**而且看着像有尺子**（比没有尺子更坏：它会给出一个"看起来对"的错答案）。
  所以这里断言的不是"跑没跑通"，是**尺子的几何**。

输出**纯 ASCII**（GBK 控制台会炸中文）。
"""
import importlib.util
import os
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, u"_maixcam", u"_iso_check.py")
MAXF = 20               # 假 need_exit 跑这么多帧就退出（不能让 30 秒的真实计时去管测试）


FRAMES = []             # 每 read() 一帧就存一个 FakeImg，测试断的是**最后一帧**画了什么


class FakeImg(object):
    u"""把 `img.draw_xxx(...)` 全部**记下来**（参数原样存），供测试断言。"""

    def __init__(self):
        self.calls = []

    def __getattr__(self, name):
        # 只放行 draw_* —— 别的名字照常报 AttributeError，免得把笔误吞掉
        if name.startswith(u"draw_"):
            def rec(*a, **k):
                self.calls.append((name, a, k))
            return rec
        raise AttributeError(name)

    def find(self, name):
        return [c for c in self.calls if c[0] == name]


def install():
    state = {u"i": 0}

    def need_exit():
        state[u"i"] += 1
        return state[u"i"] > MAXF

    class FakeCamera(object):
        def __init__(self, *a, **k):
            pass

        def read(self):
            im = FakeImg()
            FRAMES.append(im)
            return im

    class FakeDisplay(object):
        def __init__(self, *a, **k):
            pass

        def show(self, img):
            pass

    image = types.ModuleType(u"maix.image")
    image.COLOR_RED = 1
    image.COLOR_WHITE = 2
    cam_m = types.ModuleType(u"maix.camera")
    cam_m.Camera = FakeCamera
    disp_m = types.ModuleType(u"maix.display")
    disp_m.Display = FakeDisplay
    app_m = types.ModuleType(u"maix.app")
    app_m.need_exit = need_exit

    maix = types.ModuleType(u"maix")
    for name, m in ((u"camera", cam_m), (u"display", disp_m),
                    (u"image", image), (u"app", app_m)):
        setattr(maix, name, m)
        sys.modules[u"maix." + name] = m
    sys.modules[u"maix"] = maix


def load_script():
    spec = importlib.util.spec_from_file_location(u"isocheck", SRC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    install()
    mod = load_script()
    w, h = mod.W, mod.H

    rc = mod.main()

    fails = []

    def check(name, cond, got):
        print("  %-4s %s   got=%s" % ("PASS" if cond else "FAIL", name, got))
        if not cond:
            fails.append(name)

    print("[test] script mode = %dx%d, frames read = %d" % (w, h, len(FRAMES)))
    check("script returned 0", rc == 0, rc)
    check("frames read >= 5", len(FRAMES) >= 5, len(FRAMES))
    if not FRAMES:
        print("\nFAILED: no frames")
        return 1

    last = FRAMES[-1]
    circ = last.find(u"draw_circle")
    rect = last.find(u"draw_rect")
    line = last.find(u"draw_line")

    check("draw_circle called once per frame", len(circ) == 1, len(circ))
    check("draw_rect called", len(rect) >= 1, len(rect))
    check("draw_line called (crosshair)", len(line) >= 2, len(line))

    if circ:
        cx, cy, r = circ[0][1][:3]
        check("circle is centred", (cx, cy) == (w // 2, h // 2), (cx, cy))
        check("radius is sane (w/4)", r == min(w, h) // 4 and r > 10, r)
        # ★ 这条是**本文件存在的理由**：半径超出画面 ⇒ 屏幕上只剩半把尺子 ⇒ 判据悄悄变错
        check("circle fully inside frame",
              cx - r >= 0 and cy - r >= 0 and cx + r <= w - 1 and cy + r <= h - 1,
              "x:[%d,%d] y:[%d,%d] vs %dx%d" % (cx - r, cx + r, cy - r, cy + r, w, h))

    if rect:
        x, y, rw, rh = rect[0][1][:4]
        check("square is 2r x 2r", rw == rh == 2 * (min(w, h) // 4), (rw, rh))
        check("square fully inside frame",
              x >= 0 and y >= 0 and x + rw <= w and y + rh <= h,
              (x, y, rw, rh))

    print("\n%s  (%d fails)" % ("ALL PASS" if not fails else "FAILED: " + ",".join(fails),
                                len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
