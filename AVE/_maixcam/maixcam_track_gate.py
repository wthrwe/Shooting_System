# -*- coding: utf-8 -*-
u"""MaixCAM 侧：YOLOv5 检测 + 【时序一致性闸门】+ 【跳变距离实测】

# ============================ 这个文件负责什么 ============================
负责：
  1. 跑 YOLOv5 检测（`model_323612.mud`，2026-10-11 重训版）；
  2. **第二道闸**：连续 HOLD_N 帧落在「同一处」才认目标（假阳性在【跳】、真靶子【不动】）；
  3. 用**文本协议** `cx,cy,flag\\n` 发给 STM32 —— 与固件 `AVE_vision.c` 的解析一致；
  4. 实测「相邻两帧两个框的距离 d」的直方图，**这个数决定 MATCH_R 该取多少**。

不负责：
  1. 不接电机、不调 PID（那是固件侧 ② 的事）；
  2. 不改模型、不做在线学习；
  3. 不存图（负样本采集是**枚举竞赛**，本轮结论是**不追**）。

# ======================= 不许破坏的硬约束（改了必出事）=======================
  A. 相机必须开在 `detector.input_width()/input_height()` 上 —— 坐标就此锁死在 224×224。
     固件的 `AVE_VISION_SCREEN_W/H` 必须等于这两个数，否则 `err_x` 整体缩放错。
  B. 发出的每一行**必须是** `"%d,%d,%d\\n"` 三个整数、逗号分隔、换行结尾。
     固件第 3 个字段 `flag`：`1` = 有目标（`KIND_OK`）、其它 = 无目标（`KIND_NO_TARGET`）。
  C. `GATE_BEGIN/GATE_END` 之间那段**必须保持纯逻辑、不许引用任何 MaixPy 符号** ——
     桌面自测 `_selftest_ave/test_gate_host.py` 会把这段**原样抽出来跑**，
     测的就是**将要烧进相机的那几个字节**，不是它的副本。
"""

from maix import camera, display, image, nn, app, uart
import time

# ------------------------------------------------------------------ 配置
MODEL = "/root/shooting3/model_323612.mud"   # ⚠️ 改成你机上 model 的实际路径
IOU_TH = 0.45

# ⚠️⚠️ `CONF_TH` **不是模型的属性，是「姿态」的属性** —— 0.60 这个操作点已被实测否掉。
#   同一台相机、同一个模型，只换了个姿态，「每 30 帧里有多少帧 ≥0.60」这个指标：
#     姿态 A（相机偏上，靶子框 161~171x111~125）：**10 / 29 / 30 / 30 / 30 / 30** ⇒ 0.60 几乎全留；
#     姿态 B（相机偏下，靶心落在 cy≈125）：      **7 / 2 / 13 / 27 / 16 / 11** ⇒ 0.60 把大半切掉，
#                                                 六个窗口里三个 `sent=-1,-1,0`（**闸门一半时间丢了目标**）。
#   ⇒ **任何一个固定阈值换个机位就废**。详见 `verify.md` 第五十一次·第九/第十次收尾。
# ⚠️ 所以下面这个 0.55 只是**折中停靠点，不是量出来的** ——
#   靶子在 0.55 那一档的帧数**没测到**（姿态 B 那趟的过滤器卡在 0.60，0.60 以下的分布被切掉了）
#   ⇒ 这个值标 ⬜，**必须在实际工作姿态下、看着屏幕重调**。
# 📌 想回到「量分数」模式：改成 `0.35`（或 0.45），分档计数会重新给出全部帧的分布。
CONF_TH = 0.55

# ============================ GATE_BEGIN ============================
# 下面这一段是**纯逻辑**：不 import 任何东西、不碰相机/串口。
# 桌面自测把它逐字抽出来 exec，所以这里改什么，测的就是什么。
GATE = {
    "HOLD_N": 3,     # 连续几帧落在「同一处」才放行（3 帧 ≈ 100 ms @30fps）
    "MATCH_R": 20,   # 「同一处」的判定半径（像素，224 坐标系）
    "LOST_N": 2,     # 已锁定后，允许连续几帧丢失/跳走仍保持锁定
}


def gate_new():
    u"""闸门状态。`kx/ky` = 上一次被接受的框中心（**不是**上一帧的框）。"""
    return {"kept": 0, "lost": 0, "locked": False, "kx": -1, "ky": -1}


def gate_step(st, raw):
    u"""喂一帧的原始检测结果，返回本帧该发给 MCU 的三元组。

    raw = (cx, cy) 或 None（本帧没检出）。
    就地更新 st；返回 (cx, cy, flag)，flag=1 表示放行。

    设计要点（每条都有理由，别随手改）：
      * 只有「落在同一处」才 **刷新 kx/ky**。跳走的那一帧**不刷新** ——
        否则假阳性一帧就能把锁定位置拽走（这是第一版最容易写错的地方）。
      * 已锁定时，跳走/丢失不立刻解锁，先容忍 LOST_N 帧；超了才放弃旧位置。
        没有这条 ⇒ 真靶子偶掉一帧就闪成 NO_TARGET ⇒ 看着像「闸门把真靶子挡了」（假诊断）。
      * 未锁定时，任何新位置都从 kept=1 重新数 —— 这就是抗跳变的全部机理。
    """
    if raw is not None:
        cx, cy = raw
        if st["kx"] >= 0 and (cx - st["kx"]) ** 2 + (cy - st["ky"]) ** 2 <= GATE["MATCH_R"] ** 2:
            st["kept"] += 1
            st["lost"] = 0
            st["kx"], st["ky"] = cx, cy          # 只有「同一处」才刷新
        else:
            st["kept"] = 1                        # 跳了 → 从头数
            if st["locked"]:
                st["lost"] += 1
                if st["lost"] > GATE["LOST_N"]:
                    st["locked"] = False          # 认了：目标真换地方了
                    st["kx"], st["ky"] = cx, cy
            else:
                st["kx"], st["ky"] = cx, cy
        # ⚠️ 锁定判定必须对**两条分支都生效**。第一版只在「同一处」分支里判
        #    ⇒ `HOLD_N=1` 退化成「要连续两帧」，**参数与语义不符**；
        #    桌面自测的负对照（`NEGCTRL HOLD_N=1`）当场抓到 ⇒ 判据能失败，才有用。
        if st["kept"] >= GATE["HOLD_N"]:
            st["locked"] = True
    else:
        st["kept"] = 0
        if st["locked"]:
            st["lost"] += 1
            if st["lost"] > GATE["LOST_N"]:
                st["locked"] = False
                st["kx"] = st["ky"] = -1

    if st["locked"]:
        return (st["kx"], st["ky"], 1)
    return (-1, -1, 0)


_JB = (10, 20, 40, 80)          # 跳变距离分档边界（像素）


def jump_bucket(d):
    u"""把「相邻两帧框心距离」丢进档位；返回下标 0..4。"""
    for i, b in enumerate(_JB):
        if d < b:
            return i
    return len(_JB)
# ============================= GATE_END =============================

# ---- 分数分档：整数「百分之一」，65 表示 0.65 ----
# ⚠️ 档位必须**跨过候选阈值**，否则量了等于没量：
#   真机两趟（同一模型、同一 `CONF_TH=0.35`、同一台仪器）实测 ——
#     假阳性场景 7 个窗口 / 210 帧：分数上限只到 **0.55**，`>=.65` **恒为 0**；
#     真靶子     6 个窗口 / 180 帧：分数 **0.55~0.78**，`>=.65` 是 8~29/30。
#   ⇒ **决策区间就是 0.55~0.70**，而原来的档位只有 `.50/.65/.80` ⇒ 中间是空的。
#   `.50`/`.80` 留着是为了跟前几趟**对齐**（`.80` 至今 13 个窗口全 0）。
_BUCKETS = (50, 55, 60, 65, 70, 80)


def score_bins(sc, bins):
    u"""把一帧的分数落进各档（就地加一）。`bins` 与 `_BUCKETS` 等长。"""
    h = sc * 100.0
    for i, b in enumerate(_BUCKETS):
        if h >= b:
            bins[i] += 1


def main():
    detector = nn.YOLOv5(model=MODEL, dual_buff=True)
    W, H = detector.input_width(), detector.input_height()
    cam = camera.Camera(W, H, detector.input_format())
    disp = display.Display()
    ser = uart.UART("/dev/ttyS0", 115200)

    # 打印生效参数 —— 让日志**自证**跑的是哪一套参数（判据只在跑的那一刻有效）
    print("[gate] HOLD_N=%d MATCH_R=%d LOST_N=%d | CONF_TH=%.2f | space=%dx%d | model=%s"
          % (GATE["HOLD_N"], GATE["MATCH_R"], GATE["LOST_N"], CONF_TH, W, H, MODEL))

    st = gate_new()
    n = 0
    t0 = time.time()
    # ---- 窗口统计（每 30 帧清零）----
    n_raw = n_pass = 0
    sc_bins = [0] * len(_BUCKETS)    # 本窗口检出中「分数 ≥ 每档」的帧数 —— 定 `CONF_TH` 的直接依据
    win_score, win_cx, win_cy = [], [], []
    win_top = None                   # 本窗口**最高分**那个框 (sc, w, h, cx, cy)
    hist = [0] * (len(_JB) + 1)
    last_out = (-1, -1, 0)
    ser_err = 0
    prev_raw = None          # ⚠️ 只用来算「**一帧**间隔」的位移，见下面的坑

    while not app.need_exit():
        img = cam.read()
        objs = detector.detect(img, conf_th=CONF_TH, iou_th=IOU_TH)

        raw, sc = None, -1.0
        if len(objs) > 0:
            o = max(objs, key=lambda x: x.score)          # 只留最高分那个
            raw = (min(int(o.x + o.w / 2), W - 1), min(int(o.y + o.h / 2), H - 1))
            sc = o.score
            # ★ 记「本窗口最高分」那个框 —— 原来打的是**最后一帧**的框，随帧乱变、看不出规律。
            #   最高分那个才代表「模型认为最像目标的东西，有多大、多少分」，
            #   将来若要做「框尺寸闸门」，尺寸阈值只能从这一项定。
            if win_top is None or sc > win_top[0]:
                win_top = (sc, int(o.w), int(o.h), raw[0], raw[1])
            win_score.append(sc)
            win_cx.append(raw[0])
            win_cy.append(raw[1])
            # **************** 分数分档计数：★这一次跑的主要目的 ★****************
            # 假阳性落在哪一档、真靶子落在哪一档 —— 两趟一对比，`CONF_TH` 就定了。
            score_bins(sc, sc_bins)

        # ---- 位移实测：**只在「上一帧也检出」时**才算 ----
        #   ⚠️⚠️ 第一版拿 `st["kx"]` 当参照物 —— 而它在「未锁定 + 当前帧空」时**不会被清**，
        #       于是跨几十帧空档也记成一次「跳」⇒ 直方图量的是「**场景换没换**」，
        #       不是「**一帧内跳多远**」。这是**判据比它要量的事实宽**，
        #       跟「把 0/1 标志当计数」同源。⇒ 参照物必须是 prev_raw（上一帧原始框）。
        if raw is not None and prev_raw is not None:
            d = ((raw[0] - prev_raw[0]) ** 2 + (raw[1] - prev_raw[1]) ** 2) ** 0.5
            hist[jump_bucket(d)] += 1
        prev_raw = raw

        out = gate_step(st, raw)
        last_out = out
        if raw is not None:
            n_raw += 1
        if out[2] == 1:
            n_pass += 1

        try:
            ser.write(("%d,%d,%d\n" % out).encode())
        except Exception as e:
            if ser_err == 0:
                print("[uart] 写失败: %s  —— 屏幕仍然有效，继续跑" % e)
            ser_err += 1

        # ---- 画面：**原始框**红框 + 标签里带 kept，一眼看出闸门在干什么 ----
        for o in objs:
            img.draw_rect(o.x, o.y, o.w, o.h, color=image.COLOR_RED, thickness=2)
            img.draw_string(o.x, max(0, o.y - 12),
                            "%s:%.2f k%d%s" % (detector.labels[o.class_id], o.score,
                                               st["kept"], " LOCK" if st["locked"] else ""),
                            color=image.COLOR_RED)
        disp.show(img)

        n += 1
        if n >= 30:
            now = time.time()
            # 本窗口检出的**分数区间**与**位置散布** —— 这两项自己就能认出
            # 「这是真靶子还是假阳性」，不必靠回忆当时对着什么（第一版缺的正是这一项）。
            if win_score and win_top is not None:
                sc_s = "%.2f..%.2f" % (min(win_score), max(win_score))
                top_s = "%.2f @(%d,%d)" % (win_top[0], win_top[3], win_top[4])
                box_s = "%dx%d" % (win_top[1], win_top[2])
                cx_s = "%d..%d" % (min(win_cx), max(win_cx))
                cy_s = "%d..%d" % (min(win_cy), max(win_cy))
            else:
                sc_s = top_s = box_s = cx_s = cy_s = "-"
            # 分两行打：一行是**闸门在做什么**，一行是**模型看到了什么**。
            print("fps=%.1f raw=%d/30 pass=%d lockend=%d | sent=%d,%d,%d"
                  % (n / (now - t0), n_raw, n_pass, 1 if st["locked"] else 0,
                     last_out[0], last_out[1], last_out[2]))
            bin_s = " ".join(">=.%d:%d" % (_BUCKETS[i], sc_bins[i])
                             for i in range(len(_BUCKETS)))
            print("   sc %s %s | top %s | box %s | cx %s cy %s"
                  " | d(1f) <10:%d 10-20:%d 20-40:%d 40-80:%d >=80:%d"
                  % (sc_s, bin_s, top_s, box_s, cx_s, cy_s,
                     hist[0], hist[1], hist[2], hist[3], hist[4]))
            n, t0 = 0, now
            n_raw = n_pass = 0
            sc_bins = [0] * len(_BUCKETS)
            win_score, win_cx, win_cy, win_top = [], [], [], None
            hist = [0] * (len(_JB) + 1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
