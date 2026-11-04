# -*- coding: utf-8 -*-
u"""相机几何自检（一次性工具，**不进生产链路**）

# ============================ 这个文件负责什么 ============================
负责：
  回答一个问题 —— `camera.Camera(224,224)` 拿到的那张图，是把 4:3 的画面**压扁**成方形，
  还是从 4:3 里**裁**出中间一块方形？这个答案决定「训练图」与「推理图」的几何一不一致：

    训练图 = 采集的 320x240（4:3）被 MaixHub 缩到 224x224（1:1）
    推理图 = 相机**直接**给 224x224（1:1）

  相机若**裁剪** ⇒ 两边的**视场**就不是同一个 ⇒ 模型在推理时看到的是它训练时没见过的取景
  ⇒ 「训练几何 ≠ 推理几何」坐实 —— 这是目前对「置信度低、框大、随姿态漂移」最像病因的一条。

不负责：
  1. **不跑模型、不检测、不发串口** —— 这里只回答「几何」，别的都别混进来；
     混进来就分不清「分数低」是几何的锅还是模型的锅（这正是前几轮返工过的坑）。
  2. 不改任何东西（只读相机 + 画图 + 显示）。
  3. **不是产品的一部分** —— 量完就可以从设备上删掉，别留在 `/root/shooting3/` 里。

# ============================== 怎么读这个判据（10 秒）==============================
屏幕上会画一把**尺子**：画面正中一个**正圆** ＋ 一个**正方形**框 ＋ 十字线。
拿一个**圆**的实物（硬币／杯口／瓶盖）正对镜头放进去，跟尺子比：

  ┌─ 硬币和画的圆 **一样圆** ⇒ 相机**不压扁** ⇒ 那就是**裁剪**（或方形直接读出）
  │                          ⇒ 视场是方形 ⇒ **与训练图的 4:3 视场不一致** ⇒ **实锤**
  ├─ 硬币比画的圆 **明显扁** ⇒ 相机**也压扁了** ⇒ 这条嫌疑减轻
  │                          （仍要看 MaixHub 那一侧压不压，见 `verify.md` 第十次收尾）
  └─ 画面**上下有灰边／黑边** ⇒ 相机做了 **letterbox**（把 4:3 保住、两边补边）
                             ⇒ 若 MaixHub 也是 letterbox，则**两边一致**，这条排除

# ⚠️ 为什么必须画这把尺子 —— 不能"直接看硬币圆不圆"
    板子屏幕自己不是 1:1（MaixCAM 的屏是 552x368），`display.show()` 到底**保比**还是
    **拉伸铺满**，**我没验过**。万一是拉伸铺满，屏幕上的「圆」就不是图里的圆
    ⇒ 判据会被**屏幕自己**污染，而且**看着还挺像结论**（这是最坏的一种错）。
    硬币和画的圆走的是**同一条** 图→屏 通路，屏幕怎么变形都会**同时**作用在两者上、
    在**比较**里抵消掉 ⇒ 这把尺子**自校准**，不依赖任何屏幕参数。

# ============================== 运行 ==============================
  W, H = 224, 224    # 默认看的就是**推理用的那个模式**（`detector.input_width()`）
  想再看一眼原生 4:3，把下面那行改成 320, 240 重跑 —— 两个模式各看一眼更稳妥。
"""

from maix import camera, display, image, app
import time

# ------------------------------------------------------------------ 配置
W, H = 320,240        # ← 改这里换模式：224,224 = 推理模式；320,240 = 原生 4:3
SECONDS = 30           # 跑多久（够看清就行，超时自动退出，不用管）
MAX_FRAMES = 3000      # 帧数保险 —— 万一 `time` 出问题也不至于死循环


def draw_ruler(img, w, h):
    u"""在**图像坐标**里画尺子。这里的圆一定是正圆（不经过相机），这就是它的全部价值。

    参数：img = 相机帧（就地画）；w/h = 图像宽高。
    返回：无。
    坑：半径必须让圆**完整落在画面内**（`x-r >= 0` 且 `x+r <= w-1`），否则屏幕上只有半把尺子，
        而**半把尺子照样能看**，于是判据会**悄悄**变成错的 —— 所以下面留了断言，
        桌面自测 `_selftest_ave/test_iso_check_host.py` 专门断这一条。
    """
    r = min(w, h) // 4
    cx, cy = w // 2, h // 2
    assert cx - r >= 0 and cy - r >= 0 and cx + r <= w - 1 and cy + r <= h - 1, \
        "ruler does not fit: %dx%d r=%d" % (w, h, r)
    img.draw_circle(cx, cy, r, color=image.COLOR_WHITE, thickness=2)
    img.draw_rect(cx - r, cy - r, r * 2, r * 2, color=image.COLOR_RED, thickness=2)
    img.draw_line(cx - r, cy, cx + r, cy, color=image.COLOR_WHITE)
    img.draw_line(cx, cy - r, cx, cy + r, color=image.COLOR_WHITE)


def main():
    cam = camera.Camera(W, H)
    disp = display.Display()
    # ASCII only —— 这条输出会走控制台，中文在这类环境下有乱码前科
    print("[iso] mode=%dx%d  put a ROUND object in view; compare it with the drawn circle"
          % (W, H))
    t0 = time.time()
    n = 0
    while not app.need_exit() and (time.time() - t0) < SECONDS and n < MAX_FRAMES:
        img = cam.read()
        draw_ruler(img, W, H)                       # ← 尺子画在**相机帧**上，先画后显示
        img.draw_string(2, 2, "ISO %dx%d" % (W, H), color=image.COLOR_WHITE)
        disp.show(img)
        n += 1
    print("[iso] frames=%d, done" % n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
