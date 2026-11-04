#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
  ******************************************************************************
  * @file    board_check.py
  * @brief   第十四次上板验证 —— 往 PB7 灌数据、抓 PB10 的日志、按 6 条判据自动判定
  *
  *  ── 这个文件负责什么 ──────────────────────────────────────────────────────
  *    ① 按固定时序往【视觉口 PB7】灌视觉协议行；
  *    ② 全程抓【调试口 PB10】吐出来的每一行，并给每行打上到达时刻；
  *    ③ 收工后按 verify.md 第十四次那 6 条判据逐条判 PASS / FAIL，并把原始日志落盘。
  *
  *  ── 它【不】负责什么 ──────────────────────────────────────────────────────
  *    · 它不编译、不烧录、不碰 STM32 上的任何代码 —— 纯 PC 侧的测试脚手架；
  *    · 它不改判定标准。判据的文字出自 `AVE/verify.md` 第十四次第五节，
  *      本文件只是把那段文字翻译成可执行的断言。**要改判据，先改 verify.md。**
  *
  *  ── 有哪几条"不许破坏"的硬约束 ────────────────────────────────────────────
  *    🔴 1. 判据窗口必须【比 [VS] 的日志节拍宽】。`[VS]` 每 2000 毫秒才有一行，
  *          所以任何短于 2 秒的窗口都可能一整个落在两行之间 —— 那条判据就不是
  *          "失败"，而是【无从观测】。本文件的窗口都 ≥ 3 秒，别调窄。
  *    🔴 2. 每个窗口的起点都要比阶段切换点【晚 300 毫秒】。原因是断流阈值是
  *          200 毫秒（`AVE_VISION_RX_TIMEOUT_MS`，`AVE_tuning.h:138`）：刚切换完的瞬间，片上状态还没翻转，
  *          这时抓到的行反映的是【上一个阶段】。不晚这 300 毫秒，
  *          `st` 会以"对的结论、错的原因"通过 —— 这正是本项目反复栽的那个坑。
  *    🔴 3. 【判据 5 必须先回灌好帧】。若 19 秒时 `st` 本来就是 1（停发之后一直是 1），
  *          那"发坏帧 → st 转 1"什么都没验到。所以 16~19 秒先灌好帧把 `st` 压回 0，
  *          并且本文件把这个前提取成【独立断言】—— 前提没成立时判 INV（不可判定），
  *          【绝不】判 PASS。
  *
  *  ── 接线（只需要一块 USB-TTL）─────────────────────────────────────────────
  *      USB-TTL TXD ──→ PB7   (USART1_RX，灌数据)
  *      USB-TTL RXD ←── PB10  (USART3_TX，读日志)
  *      USB-TTL GND ──── GND
  *    TXD / RXD 是两根独立的线，同时收发互不干扰，【不需要第二块】。
  *    ⚠️ PB11 不用接。灌到 PB11 是调试口的 RX，什么都不会发生。
  *    ⚠️ PC13 是激光。本脚本不驱动它，但别拿眼睛对着镜头。
  *
  *  ── 用法 ──────────────────────────────────────────────────────────────────
  *      python board_check.py                     # 真上板跑一遍（默认 COM4）
  *      python board_check.py --port COM7
  *      python board_check.py --selftest           # ★ 离线自测判定逻辑（不接板子）
  *
  *  `--selftest` 是硬要求，不是可选项：它拿 9 份【伪造的回显文本】喂给同一套断言，
  *  其中 8 份是【故意坏的】。一个永远报 PASS 的检查脚本比没有脚本更糟 ——
  *  自测就是证明"它判得对，也判得错"。
  ******************************************************************************
"""

import argparse
import re
import sys
import time

# Windows 的控制台默认按 GBK 编码 stdout，本文件里有中文和少量符号（↳ ✅ 🔴 ⚠️），
# 撞上 GBK 编不出来的字符会直接 UnicodeEncodeError 崩掉 ——
# 而崩的位置恰好是"打印失败详情"那一行，于是【真正要看的报错被吞掉】。
# 本工程已经吃过一次"诊断通道自己先坏掉"的亏（第十三次的 Error_Handler 静默），
# 所以这里在导入期就把 stdout 固定成 UTF-8。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ============================================================================
# 一、判据窗口 —— 这里的数字全部对应第四节的注入时序
#
# 为什么把窗口写成常量而不是散在代码里：它们是【判据的一部分】，
# 调它们等于调判据。集中在这里，改的时候一眼能看到改了哪几条。
# ============================================================================

GUARD = 0.3          # 阶段切换后的静默期（见文件头硬约束 2）

# 注入时序：(阶段名, 起, 止, 每次发什么, 多少毫秒发一次)
PHASES = [
    ("baseline", 0.0,  5.0,  None,              0),    # 什么都不发，拿基线
    ("good",     5.0,  9.0,  b"320,240,1\n",    100),  # 判据 2
    ("notarget", 9.0,  12.0, b"-1,-1,0\n",      100),  # 判据 3
    ("silent",   12.0, 16.0, None,              0),    # 判据 4：停发
    ("good2",    16.0, 19.0, b"320,240,1\n",    100),  # 判据 5 的前置：把 st 压回 0
    ("bad",      19.0, 22.0, b"900,900,1\n",    100),  # 判据 5
    ("tail",     22.0, 25.0, None,              0),    # 收尾
]

TOTAL_S = PHASES[-1][2]

# 各阶段的切换点。基线的截取【必须】用这些值，不能用窗口起点 —— 理由见下。
S_GOOD, S_NOTARGET, S_SILENT, S_GOOD2, S_BAD = 5.0, 9.0, 12.0, 16.0, 19.0

# 判定窗口：一律 = 阶段起点 + GUARD，末尾不设 guard（下一阶段的行不会提前到）
W_GOOD     = (S_GOOD + GUARD,     S_NOTARGET)
W_NOTARGET = (S_NOTARGET + GUARD, S_SILENT)
W_SILENT   = (S_SILENT + GUARD,   S_GOOD2)
W_GOOD2    = (S_GOOD2 + GUARD,    S_BAD)
W_BAD      = (S_BAD + GUARD,      TOTAL_S)

# 🔴 护栏带里那一条 [VS] 行【状态是不确定的】，必须从【窗口和基线两边都排除】。
#
#    这一条是自测抓出来的，不是想出来的：最初基线截取点写的是"窗口起点"(阶段起点
#    + GUARD)，于是到达时刻恰好落在护栏带里的那条 [VS]（它已经反映了新阶段）被算进了
#    基线 —— 基线 = 新值，窗口末 = 新值，"没涨"，判据 2 在【全好的数据上】报了 FAIL。
#    所以要取"阶段切换之前"最后一条：那时片上状态还没翻转，是干净的。

# ============================================================================
# 二、两个正则 —— 按源码里的格式串定死，不是照抄文档
#
#   [VS] 出自 AVE/AVE_vision.c:536
#   [HB] 出自 AVE/AVE_app.c:380
#
# 只锚定行首（不用 $）：一是行尾可能带 \r\n，二是【截断的行也必须被判为不匹配】——
# AVE_Log 截断时在行尾补一个 '~'，那种行不该被当成"格式正确"。
# ============================================================================

RE_VS = re.compile(
    r"^\[VS\] t=(\d+) st=(\d+) se=(\d+) ok=(\d+) dr=(\d+) sp=(\d+) f=(-?\d+),(-?\d+),(\d+)"
)
RE_HB = re.compile(
    r"^\[HB\] t=(\d+) v=(\d+) m=(\d+) c=(\d+) hb=(\d+) rx=(\d+) drrx=(\d+) drtx=(\d+) ovf=(\d+) bf=(\d+)"
)

# 每条 [VS] 的字段名，用来把匹配结果变成一个能按名字取的结构
VS_FIELDS = ("t", "st", "se", "ok", "dr", "sp", "fx", "fy", "kind")
HB_FIELDS = ("t", "v", "m", "c", "hb", "rx", "drrx", "drtx", "ovf", "bf")


def _mk(fields, m):
    """把正则的匹配组变成 {字段名: int}。"""
    return {name: int(m.group(i + 1)) for i, name in enumerate(fields)}


# ============================================================================
# 三、判定核心 —— analyze() 是【纯函数】
#
# 为什么刻意写成纯函数：它只依赖 "一串 (到达时刻, 行文本)"，不碰串口、不碰时钟。
# 于是同一套断言既能判真板子的抓包，也能判 --selftest 的伪造文本。
# 这一点和产品代码里 `AVE_Vision_ParseLine()` 是纯函数是同一个理由：
# 【能让判据本身成为被测对象】。
# ============================================================================

def analyze(records):
    """
    records: [(到达秒, 行文本), ...]
    返回:    [(编号, 判据简述, 结论, 说明), ...]   结论 ∈ {PASS, FAIL, INV}
    """
    vs = [(t, _mk(VS_FIELDS, m)) for t, s in records for m in [RE_VS.match(s)] if m]
    hb = [(t, _mk(HB_FIELDS, m)) for t, s in records for m in [RE_HB.match(s)] if m]

    out = []

    # ── 先算几个后面反复要用的基线值 ────────────────────────────────────────
    def last_before(seq, when):
        """取 when 之前最后一条；没有就返回 None（不用默认值顶替 —— 那会掩盖问题）。"""
        hit = [r for t, r in seq if t < when]
        return hit[-1] if hit else None

    def in_win(seq, win):
        return [(t, r) for t, r in seq if win[0] <= t < win[1]]

    # 基线一律取【阶段切换之前】最后一条 —— 用 S_*（切换点）而不是 W_*（窗口起点），
    # 理由见上面那段护栏带的说明。
    se_ref = last_before(vs, S_GOOD)
    se_ref = se_ref["se"] if se_ref else None
    ok_ref = last_before(vs, S_GOOD)
    ok_ref = ok_ref["ok"] if ok_ref else None
    dr_ref = last_before(vs, S_BAD)
    dr_ref = dr_ref["dr"] if dr_ref else None

    # ── 判据 1：[HB] 行逐字不变，bf=0 仍成立 ────────────────────────────────
    # "逐字不变"的操作化定义：每条都以 [HB] 开头、都能被 10 字段正则吃下、都不含 '~'。
    tagged = [s for _, s in records if s.startswith("[HB]")]
    bad_fmt = [s for _, s in records if s.startswith("[HB]") and (not RE_HB.match(s) or "~" in s)]
    bf_nonzero = [r["bf"] for _, r in hb if r["bf"] != 0]
    if not tagged:
        out.append(("C1", "[HB] 逐字不变且 bf=0", "FAIL",
                    "一条 [HB] 都没有 —— 心跳没在跑，先解决这个，后面的判据全部无效"))
    elif bad_fmt:
        out.append(("C1", "[HB] 逐字不变且 bf=0", "FAIL",
                    f"{len(bad_fmt)} 条 [HB] 格式不对或被截断，第一条：{bad_fmt[0]!r}"))
    elif bf_nonzero:
        out.append(("C1", "[HB] 逐字不变且 bf=0", "FAIL", f"bf 出现非 0 值 {bf_nonzero}"))
    else:
        out.append(("C1", "[HB] 逐字不变且 bf=0", "PASS",
                    f"{len(tagged)} 条 [HB] 全部 10 字段可解析、无 '~'、bf=0"))

    # ── 判据 2：发 320,240,1 → ok 涨、st=0、f=320,240,2 ─────────────────────
    w = in_win(vs, W_GOOD)
    if not w:
        out.append(("C2", "好帧 → ok 涨 / st=0 / f=320,240,2", "INV",
                    f"{W_GOOD[0]:.1f}~{W_GOOD[1]:.1f}s 窗口内一条 [VS] 都没有（节拍 2s，窗口 3.7s，"
                    "理论上不该为空 —— 先查 [HB] 里的 v 有没有涨）"))
    elif ok_ref is None:
        out.append(("C2", "好帧 → ok 涨 / st=0 / f=320,240,2", "INV",
                    "窗口之前没有基线 [VS] 行，无法算 ok 的增量"))
    else:
        st_bad = [r["st"] for _, r in w if r["st"] != 0]
        ok_last = w[-1][1]["ok"]
        f_last = (w[-1][1]["fx"], w[-1][1]["fy"], w[-1][1]["kind"])
        if st_bad:
            out.append(("C2", "好帧 → ok 涨 / st=0 / f=320,240,2", "FAIL",
                        f"窗口内 st 非 0（={st_bad}）—— 好帧没能刷新 last_frame_ms"))
        elif ok_last <= ok_ref:
            out.append(("C2", "好帧 → ok 涨 / st=0 / f=320,240,2", "FAIL",
                        f"ok 没涨：基线 {ok_ref} → 窗口末 {ok_last}。"
                        "同时看 [HB] 的 rx：rx 涨了说明字节到了但没解析出来（换行符？），"
                        "rx 也没涨说明字节根本没到片子（接线 / 波特率）"))
        elif f_last != (320, 240, 2):
            out.append(("C2", "好帧 → ok 涨 / st=0 / f=320,240,2", "FAIL",
                        f"最新帧是 {f_last}，期望 (320, 240, 2)"))
        else:
            out.append(("C2", "好帧 → ok 涨 / st=0 / f=320,240,2", "PASS",
                        f"ok {ok_ref} → {ok_last}，st=0，f={f_last[0]},{f_last[1]},{f_last[2]}"))

    # ── 判据 3：发 -1,-1,0 → f=-1,-1,1、st 仍为 0、se 不涨 ──────────────────
    # 这一条是 §6.3 硬约束 3 的唯一守卫：无目标帧也必须刷新 last_frame_ms。
    #
    # 🆕 2026-09-26（收尾第十四次）补了 f 这条。原判据只有"st=0 且 se 不涨"，
    #    而那只证明【没出事】——证不了"无目标帧真的被解析并发布了"。
    #    两者的区别不是吹毛求疵：一个被静默判成协议错的行，和一个真被接受的行，
    #    在计数上只差一个数字（§四.P5 那个"少一个字段"的坑就是这样漏过去的）。
    #
    #    f 打的是 kind，不是线上的 flag（AVE_vision.c:528-534 有理由）：
    #      0 = 从未收到过任何一帧   1 = 无目标帧   2 = 有效目标帧
    #    所以 (-1,-1,1) 是"收到了、且确实是没有目标"的直接读数；
    #    (-1,-1,0) 是"一帧都没收到"（要查线），(320,240,2) 是"上一条好帧还留着"（这行没被接受）。
    C3_NAME = "无目标帧 → f=-1,-1,1 / st=0 / se 不涨"
    w = in_win(vs, W_NOTARGET)
    if not w:
        out.append(("C3", C3_NAME, "INV", "窗口内无 [VS] 行"))
    elif se_ref is None:
        out.append(("C3", C3_NAME, "INV", "拿不到 se 基线"))
    else:
        st_bad = [r["st"] for _, r in w if r["st"] != 0]
        se_bad = [r["se"] for _, r in w if r["se"] != se_ref]
        f_last = (w[-1][1]["fx"], w[-1][1]["fy"], w[-1][1]["kind"])
        if st_bad:
            out.append(("C3", C3_NAME, "FAIL",
                        f"窗口内 st 非 0（={st_bad}）—— 无目标帧【没有】刷新 last_frame_ms，"
                        "200ms 后就断流了（§6.3 硬约束 3 破了）"))
        elif se_bad:
            out.append(("C3", C3_NAME, "FAIL",
                        f"se 涨了：基线 {se_ref}，窗口内出现 {se_bad}"))
        elif f_last != (-1, -1, 1):
            out.append(("C3", C3_NAME, "FAIL",
                        f"最新帧是 {f_last}，期望 (-1, -1, 1)。"
                        "(-1,-1,0)=一帧都没收到（查线）；(320,240,2)=上一条好帧还留在最新位上"
                        "（说明这一行根本没被接受）；别的值说明它被判成了别的东西。"
                        "⚠️ 注意 st=0 只说明【有东西刷新了时间戳】，"
                        "**不**说明【刷新的东西是对的】—— 这正是本条要单独看 f 的原因"))
        else:
            out.append(("C3", C3_NAME, "PASS",
                        f"f=-1,-1,1，st=0，se 保持 {se_ref}"
                        "（无目标帧确实被解析、发布，并刷新了时间戳）"))

    # ── 判据 4：停发 → st=1、se 恰好 +1 ────────────────────────────────────
    w = in_win(vs, W_SILENT)
    if not w:
        out.append(("C4", "停发 → st=1 且 se 恰好 +1", "INV", "窗口内无 [VS] 行"))
    elif se_ref is None:
        out.append(("C4", "停发 → st=1 且 se 恰好 +1", "INV", "拿不到 se 基线"))
    else:
        st_bad = [r["st"] for _, r in w if r["st"] != 1]
        se_vals = sorted({r["se"] for _, r in w})
        if st_bad:
            out.append(("C4", "停发 → st=1 且 se 恰好 +1", "FAIL",
                        f"停发窗口内 st 仍为 0（={st_bad}）—— 断流检测没触发"))
        elif se_vals != [se_ref + 1]:
            out.append(("C4", "停发 → st=1 且 se 恰好 +1", "FAIL",
                        f"se 不是【恰好 +1】：基线 {se_ref}，窗口内出现 {se_vals}。"
                        "若远大于 +1（例如 +100），说明 se 被写成了【每次轮询都加】"
                        "而不是【每次跳变才加】—— 50Hz 轮询下 2 秒会涨约 100"))
        else:
            out.append(("C4", "停发 → st=1 且 se 恰好 +1", "PASS",
                        f"st=1，se {se_ref} → {se_ref + 1}（恰好一次，不是每轮 +1）"))

    # ── 判据 5：发 900,900,1 → dr 涨、st 转 1 ───────────────────────────────
    pre = in_win(vs, W_GOOD2)
    w = in_win(vs, W_BAD)
    pre_ok = bool(pre) and all(r["st"] == 0 for _, r in pre)
    if not pre:
        out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "INV",
                    f"前置窗口 {W_GOOD2[0]:.1f}~{W_GOOD2[1]:.1f}s 无 [VS] 行，"
                    "无法确认回灌好帧是否生效"))
    elif not pre_ok:
        out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "INV",
                    "🔴 前置未成立：回灌好帧后 st 仍非 0，说明 st 本来就是 1。"
                    "此时『坏帧让 st 转 1』什么都没验到 —— 判 INV，不判 PASS"))
    elif not w:
        out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "INV", "窗口内无 [VS] 行"))
    elif dr_ref is None:
        out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "INV", "拿不到 dr 基线"))
    else:
        dr_last = w[-1][1]["dr"]
        st_ones = [r["st"] for _, r in w if r["st"] == 1]
        if dr_last <= dr_ref:
            out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "FAIL",
                        f"dr 没涨：基线 {dr_ref} → 窗口末 {dr_last}。"
                        "900,900,1 应被判为协议错（越界）"))
        elif not st_ones:
            out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "FAIL",
                        f"dr 涨了（{dr_ref} → {dr_last}）但窗口内 st 始终为 0 —— "
                        "坏帧不该刷新 last_frame_ms，它刷新了"))
        else:
            out.append(("C5", "坏帧 → dr 涨 且 st 转 1", "PASS",
                        f"dr {dr_ref} → {dr_last}，窗口内出现过 st=1"))

    # ── 判据 6：ovf=0、bf=0、drtx 不涨 ─────────────────────────────────────
    if not hb:
        out.append(("C6", "ovf=0 / bf=0 / drtx 不涨", "FAIL", "一条 [HB] 都没有"))
    elif len(hb) < 5:
        # 样本太少时的"没涨"不是证据 —— 这是本判据最容易自欺的地方
        out.append(("C6", "ovf=0 / bf=0 / drtx 不涨", "INV",
                    f"只有 {len(hb)} 条 [HB]（需 ≥5）—— 样本太少，『没涨』不构成证据"))
    else:
        ovf_bad = sorted({r["ovf"] for _, r in hb})
        drtx_vals = sorted({r["drtx"] for _, r in hb})
        if ovf_bad != [0]:
            out.append(("C6", "ovf=0 / bf=0 / drtx 不涨", "FAIL",
                        f"🔴 ovf={ovf_bad} —— 栈溢出。这是本轮唯一可能真出新问题的判据："
                        "AVE_Log 现在也跑在 VisionTask（1024 字节栈）上"))
        elif drtx_vals != [0]:
            out.append(("C6", "ovf=0 / bf=0 / drtx 不涨", "FAIL",
                        f"drtx 出现非 0 值 {drtx_vals} —— 两种日志在同一瞬间撞上了（错相失效），"
                        "或日志打得比串口发得还快"))
        else:
            out.append(("C6", "ovf=0 / bf=0 / drtx 不涨", "PASS",
                        f"{len(hb)} 条 [HB] 全程 ovf=0、drtx=0（错相有效的证据）"))

    return out


# ============================================================================
# 四、诊断信息 —— 判据之外，现场排障真正要看的那几个数
#
# 这一节不参与判定，但它回答的是"判据红了之后往哪查"。
# 特别是 rx 与 ok 的对照：rx 涨而 ok 不涨 = 解析问题；rx 也不涨 = 接线问题。
# ============================================================================

def diagnostics(records):
    vs = [_mk(VS_FIELDS, m) for _, s in records for m in [RE_VS.match(s)] if m]
    hb = [_mk(HB_FIELDS, m) for _, s in records for m in [RE_HB.match(s)] if m]
    lines = []

    known = ("[HB]", "[VS]")
    junk = [s for _, s in records if not s.startswith(known)]
    if junk:
        lines.append(f"未识别行 {len(junk)} 条，前 3 条：{[j[:70] for j in junk[:3]]}")

    if hb:
        lines.append(f"[HB] {len(hb)} 条   rx {hb[0]['rx']} → {hb[-1]['rx']}"
                     f"   drrx {hb[0]['drrx']} → {hb[-1]['drrx']}"
                     f"   v {hb[0]['v']} → {hb[-1]['v']}   hb {hb[0]['hb']} → {hb[-1]['hb']}")
    if vs:
        lines.append(f"[VS] {len(vs)} 条   ok {vs[0]['ok']} → {vs[-1]['ok']}"
                     f"   dr {vs[0]['dr']} → {vs[-1]['dr']}"
                     f"   sp {vs[0]['sp']} → {vs[-1]['sp']}"
                     f"   se {vs[0]['se']} → {vs[-1]['se']}")
        lines.append(f"     最近一条：t={vs[-1]['t']} st={vs[-1]['st']} f="
                     f"{vs[-1]['fx']},{vs[-1]['fy']},{vs[-1]['kind']}")
    else:
        lines.append("🔴 一条 [VS] 都没有。排查顺序：① 是不是只抓了头 3 秒"
                     "（第一条 [VS] 在 t≈3000ms，因为 s_vs_last_log_ms 初值为 0，"
                     "t=1000 时被 2000ms 守卫挡掉）；② 上面那行的 v 在不在涨 ——"
                     " v 在涨而 [VS] 不来 = 错相/守卫问题；v 也不涨 = VisionTask 真的没跑")
    return lines


# ============================================================================
# 五、真上板：注入 + 抓包
# ============================================================================

def _open_real_serial(port, baud):
    import serial  # 只在真上板时才需要 pyserial，--selftest 不依赖它
    return serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=0)


def run_board(port, baud, opener=None, clock=None, sleep=None, echo=True):
    """
    注入 + 抓包的主循环。

    参数里那三个 `opener` / `clock` / `sleep` 是【为了能被离线测】而注入的：
    真上板时留空（用真的串口、真时钟、真 sleep），自测时换成假的 —— 于是
    这一段的时序与拆行逻辑【不接板子也能跑】，不必拿真硬件当第一次调试对象。

    返回 [(到达秒, 行文本), ...]；每一行都已剥掉 '\\r\\n' 与首尾空白。
    """
    opener = opener or _open_real_serial
    clock = clock or time.monotonic
    sleep = sleep or time.sleep

    ser = opener(port, baud)
    records = []
    buf = b""

    if echo:
        print(f"[i] {port} @ {baud} 已打开。")
        print(f"[i] 接线确认：TXD→PB7（灌数据）、RXD→PB10（读日志）、GND 共地。")
        print(f"[i] 全程 {TOTAL_S:.0f} 秒，中途不要动板子、不要拔线。")
        print(f"[i] 板子应【已经】在跑（先看到 [HB] 再启动本脚本更好）。\n")

    t0 = clock()
    sent = {name: start for name, start, _, _, _ in PHASES}   # 每个阶段下一次该发的时刻
    next_tick = 0.0

    try:
        while True:
            el = clock() - t0
            if el > TOTAL_S:
                break

            # ① 注入。`while` 而不是 `if`：某一轮若被拖慢，欠下的几次会在这里补齐，
            #    平均发送率仍等于 period。
            for name, start, end, payload, period in PHASES:
                if payload is None or not (start <= el < end):
                    continue
                while sent[name] <= el:
                    ser.write(payload)
                    sent[name] += period / 1000.0

            # ② 抓包，按 '\n' 切行。
            #    不能用 readline()：串口一次可能只吐半行，readline 会把那半行
            #    攒着不返回（或按 timeout 草率返回），两种都会让判据看到残缺的行。
            #    自己攒 buf、只在见到 '\n' 时才切，半行【留着等下一次】，一行都不会丢。
            data = ser.read(4096)
            if data:
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("ascii", errors="replace").strip()
                    if line:                       # 空行/纯空白行丢掉，别污染判定
                        records.append((clock() - t0, line))

            # ③ 进度与实时回显 —— 让人能当场看出"箭头"对不对
            if el - next_tick >= 1.0:
                next_tick = el
                phase = next((n for n, s, e, _, _ in PHASES if s <= el < e), "?")
                if echo:
                    print(f"  [{el:5.1f}s] {phase:9s} 已收 {len(records):4d} 行"
                          f"   最近：{records[-1][1][:72] if records else '(还没有)'}")

            # ④ 让出 CPU。没有这一句就是个 100% 占满一个核的忙等循环（25 秒）——
            #    功能上没错，但会让机器风扇狂转，而且掩盖"脚本卡住了"这类现象。
            sleep(0.001)
    finally:
        ser.close()

    return records


# ============================================================================
# 六、离线自测 —— 证明这套断言【判得对，也判得错】
#
# 9 份伪造回显：1 份应该是全 PASS，8 份各自【必须】让某条判据红。
# 伪造行的到达时刻特意选在判定窗口正中（3.0/7.0/11.0/…），
# 免得自测本身因为窗口边界而假红 —— 那样就分不清"断言错了"和"用例选歪了"。
# ============================================================================

def _fixture(overrides=None, drop_vs=False, hb_mut=None):
    """
    造一份 25 秒的伪造回显。
      overrides: {(到达秒): [要覆盖的字段 dict]}  —— 改某条 [VS]
      drop_vs:   True 表示一条 [VS] 都不发
      hb_mut:    fn(i, fields) -> fields          —— 逐条改 [HB]
    状态机：基线(st=1,se=1) → 好帧(st=0) → 无目标帧(st=0) → 停发(se=2)
            → 回灌好帧(st=0) → 坏帧(st=1, dr 涨)
    """
    overrides = overrides or {}
    recs = []

    # [HB]：每 2 秒一条，从 2.0 起
    for i, at in enumerate(range(2, 26, 2)):
        f = {"t": at * 1000, "v": 50 * i, "m": 100 * i, "c": 100 * i, "hb": i,
             "rx": 0, "drrx": 0, "drtx": 0, "ovf": 0, "bf": 0}
        if hb_mut:
            f = hb_mut(i, f)
        recs.append((float(at), "[HB] t={t} v={v} m={m} c={c} hb={hb} rx={rx} drrx={drrx}"
                              " drtx={drtx} ovf={ovf} bf={bf}".format(**f)))

    if drop_vs:
        return sorted(recs)

    # [VS]：每 2 秒一条，从 3.0 起；状态按到达时刻落在哪个阶段决定
    for at in range(3, 26, 2):
        a = float(at)
        if a < 5.0:
            s = dict(st=1, se=1, ok=0,  dr=0,  sp=0, fx=0,   fy=0,   kind=0)
        elif a < 9.0:
            s = dict(st=0, se=1, ok=17, dr=0,  sp=0, fx=320, fy=240, kind=2)
        elif a < 12.0:
            s = dict(st=0, se=1, ok=17, dr=0,  sp=0, fx=-1,  fy=-1,  kind=1)
        elif a < 16.0:
            s = dict(st=1, se=2, ok=17, dr=0,  sp=0, fx=-1,  fy=-1,  kind=1)
        elif a < 19.0:
            s = dict(st=0, se=2, ok=40, dr=0,  sp=0, fx=320, fy=240, kind=2)
        else:
            s = dict(st=1, se=3, ok=40, dr=12, sp=0, fx=320, fy=240, kind=2)
        s["t"] = at * 1000
        s.update(overrides.get(a, {}))
        recs.append((a, "[VS] t={t} st={st} se={se} ok={ok} dr={dr} sp={sp}"
                        " f={fx},{fy},{kind}".format(**s)))

    return sorted(recs)


# ============================================================================
# 六之二、注入 / 抓包那半段的离线自测
#
# 上面那 6 份伪造回显只证明了【判定逻辑】。可真上板真正要跑的是 run_board() ——
# 注入时序和拆行。那一段一行都没测过，等于"检查脚本自己没被检查"，
# 与本工程"没有计数的模块 = 没有仪表盘的飞机"这条原则直接冲突。
#
# 所以用假串口 + 虚拟时钟把它也拉到桌面上跑：
#   · 虚拟时钟：sleep(n) 直接把时间往前拨，25 秒的流程瞬间跑完且【完全可复现】；
#   · 假串口：write() 记下时刻，read() 回放预置字节流 —— 字节流可以故意切在行中间。
# 这样"半行会不会被丢掉""不该发的时候有没有发"这两件事都能在接板子之前查出来。
# ============================================================================

class VClock:
    """虚拟时钟：sleep(n) 直接把它往前拨 n 秒。"""

    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeSerial:
    """假串口。script = [(时刻, 字节), ...]，到点就把这些字节交给 read()。"""

    def __init__(self, script, clock):
        self.script = list(script)
        self.clock = clock
        self.writes = []
        self.closed = False

    def write(self, b):
        self.writes.append((self.clock(), b))
        return len(b)

    def read(self, n):
        now = self.clock()
        out = b""
        while self.script and self.script[0][0] <= now:
            out += self.script.pop(0)[1]
        return out

    def close(self):
        self.closed = True


def selftest_loop():
    print("\n" + "=" * 74)
    print("注入 / 抓包主循环的离线自测（不接板子）")
    print("=" * 74)

    HB = "[HB] t={t} v=1 m=1 c=1 hb=1 rx=0 drrx=0 drtx=0 ovf=0 bf=0"
    script = [
        (2.0, (HB.format(t=2000) + "\n").encode()),
        (3.0, b"[VS] t=3000 st=1 se=1 ok=0 dr=0 sp=0 f=0,0,0\n"),
        (4.0, (HB.format(t=4000) + "\r\n").encode()),          # 带 \r，必须被剥掉
        (5.0, b"[VS] t=5000 st=0 se=1 ok=9 dr=0 sp=0 f=320,240,2\n"),
        # ★ 一条 [HB] 被【切成两半】送达 —— readline() 最容易在这里出错
        (6.0, b"[HB] t=60"),
        (6.5, b"00 v=2 m=2 c=2 hb=2 rx=0 drrx=0 drtx=0 ovf=0 bf=0\n"),
        (7.0, b"\n   \n"),                                     # 空行必须被忽略
    ]
    ck = VClock()
    ser = FakeSerial(script, ck)
    recs = run_board("FAKE", 115200, opener=lambda p, b: ser,
                     clock=ck, sleep=ck.sleep, echo=False)

    checks = []

    def chk(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    chk("串口被正常关闭", ser.closed)
    chk("共收到 5 行（空行不算）", len(recs) == 5, f"实得 {len(recs)}：{[r[1] for r in recs]}")

    hb6000 = [r for r in recs if "t=6000" in r[1]]
    chk("被切成两半的 [HB] 拼回【一条】完整行", len(hb6000) == 1, f"实得 {len(hb6000)} 条")
    # 本节的要点：半行若被丢掉或拼错，判据 1（[HB] 逐字不变）会在真板子上
    # 【无端报红】，而这跟固件毫无关系 —— 那是最浪费一轮排障的假故障。
    if hb6000:
        chk("拼回的行能被 10 字段正则吃下", RE_HB.match(hb6000[0][1]) is not None, hb6000[0][1])

    chk("行尾的 \\r 被剥掉", all("\r" not in r[1] for r in recs))
    chk("没有空行混进来", all(r[1].strip() for r in recs))

    def phase_of(t):
        return next((n for n, s, e, _, _ in PHASES if s <= t < e), None)

    payload_phases = {n for n, _, _, p, _ in PHASES if p is not None}
    bad = [(round(t, 3), phase_of(t)) for t, _ in ser.writes if phase_of(t) not in payload_phases]
    chk("没在『不发』的阶段（基线/停发/收尾）里发过字节", not bad, f"越界 {bad[:5]}")

    counts = {}
    for t, _ in ser.writes:
        counts[phase_of(t)] = counts.get(phase_of(t), 0) + 1
    # good 阶段 5~9 秒、每 100ms 一次 → 约 40 次（浮点累加会差一两次，给 ±2）
    chk("好帧阶段发送约 40 次", abs(counts.get("good", 0) - 40) <= 2,
        f"实得 {counts.get('good', 0)}；各阶段次数 {counts}")

    npass = sum(1 for _, ok, _ in checks if ok)
    for name, ok, detail in checks:
        print(f"  [{'OK ' if ok else 'BAD'}] {name}{('  —— ' + detail) if detail else ''}")
    print(f"\n主循环自测：{npass}/{len(checks)} 通过")
    return npass == len(checks)


def _verdicts(res):
    return {c[0]: c[2] for c in res}


def selftest():
    print("=" * 74)
    print("离线自测 —— 每个用例都必须给出【预先写死】的结论")
    print("=" * 74)

    cases = []

    # ① 全好 —— 6 条全绿
    cases.append(("全好（应全 PASS）", _fixture(),
                  {"C1": "PASS", "C2": "PASS", "C3": "PASS", "C4": "PASS",
                   "C5": "PASS", "C6": "PASS"}))

    # ② [HB] 被截断（行尾补 '~'）—— 只有 C1 该红
    recs = _fixture()
    recs = [(t, (s + "~") if (s.startswith("[HB]") and t == 6.0) else s) for t, s in recs]
    cases.append(("一条 [HB] 带 '~'（C1 该红）", recs,
                  {"C1": "FAIL", "C2": "PASS", "C3": "PASS", "C4": "PASS",
                   "C5": "PASS", "C6": "PASS"}))

    # ③ drtx 在涨（错相失效）—— 只有 C6 该红
    def rising_drtx(i, f):
        f["drtx"] = i // 3
        return f
    cases.append(("drtx 在涨（C6 该红）", _fixture(hb_mut=rising_drtx),
                  {"C1": "PASS", "C2": "PASS", "C3": "PASS", "C4": "PASS",
                   "C5": "PASS", "C6": "FAIL"}))

    # ④ se 被写成"每轮都加"—— 停发窗口里 se 暴涨。只有 C4 该红
    cases.append(("停发期间 se 暴涨（C4 该红）",
                  _fixture(overrides={13.0: {"se": 101}, 15.0: {"se": 201}}),
                  {"C1": "PASS", "C2": "PASS", "C3": "PASS", "C4": "FAIL",
                   "C5": "PASS", "C6": "PASS"}))

    # ⑤ 一条 [VS] 都没有 —— C2~C5 必须【红或不可判定】，绝不许 PASS
    cases.append(("完全没有 [VS]（C2~C5 不许 PASS）", _fixture(drop_vs=True),
                  {"C1": "PASS", "C2": "INV", "C3": "INV", "C4": "INV",
                   "C5": "INV", "C6": "PASS"}))

    # ⑥ 回灌好帧没生效（st 一直是 1）—— C5 的前提不成立，必须判 INV，不许判 PASS
    cases.append(("回灌好帧没生效（C5 该 INV，不许 PASS）",
                  _fixture(overrides={17.0: {"st": 1}}),
                  {"C1": "PASS", "C2": "PASS", "C3": "PASS", "C4": "PASS",
                   "C5": "INV", "C6": "PASS"}))

    # ⑦⑧⑨ 2026-09-26 补：C3 的三条 FAIL 分支各配一个反例。
    #
    # 为什么必须补：C3 的原判据（只有 st / se）**一条反例都没有** ——
    #   也就是说它在真板子上红了，我没有任何证据说明"它红得对"。
    #   「一个全部通过的判据」和「一个不可能失败的判据」在输出上长得一模一样，
    #   本工程为此栽过一次（V11 把计数填成正好等于封顶值，去掉封顶的变异体安然全过）。
    #   这三条用例和①一起，构成 C3 的"判得对，也判得错"。

    # ⑦ 无目标帧没被发布（kind 停在 NONE）—— 只有 C3 该红
    cases.append(("无目标帧没被发布（C3 该红）",
                  _fixture(overrides={9.0: {"kind": 0}, 11.0: {"kind": 0}}),
                  {"C1": "PASS", "C2": "PASS", "C3": "FAIL", "C4": "PASS",
                   "C5": "PASS", "C6": "PASS"}))

    # ⑧ 无目标帧不刷新时间戳（st 一直是 1）—— 只有 C3 该红
    cases.append(("无目标帧不刷新时间戳（C3 该红）",
                  _fixture(overrides={11.0: {"st": 1}}),
                  {"C1": "PASS", "C2": "PASS", "C3": "FAIL", "C4": "PASS",
                   "C5": "PASS", "C6": "PASS"}))

    # ⑨ 无目标帧期间 se 涨了（断流被误触发）—— 只有 C3 该红
    cases.append(("无目标帧期间 se 涨（C3 该红）",
                  _fixture(overrides={11.0: {"se": 2}}),
                  {"C1": "PASS", "C2": "PASS", "C3": "FAIL", "C4": "PASS",
                   "C5": "PASS", "C6": "PASS"}))

    npass = 0
    for title, recs, want in cases:
        got = _verdicts(analyze(recs))
        ok = (got == want)
        npass += 1 if ok else 0
        print(f"\n[{'OK ' if ok else 'BAD'}] {title}")
        print(f"      期望 {want}")
        print(f"      实得 {got}")
        if not ok:
            for cid, name, verdict, detail in analyze(recs):
                if want.get(cid) != verdict:
                    print(f"      ↳ {cid} {name}: {verdict} —— {detail}")

    print("\n" + "=" * 74)
    print(f"判定逻辑自测：{npass}/{len(cases)} 个用例的结论与预期一致")
    print("=" * 74)

    # 🔴 两半都必须跑，缺一半都不算自测过。
    #    上面那一半只验了【判定逻辑】（`analyze`）；可真上板要跑的是 `run_board()` ——
    #    【注入时序】和【拆行】。那一段不测，就是"检查脚本自己没被检查"。
    #
    #    ⚠️ 2026-09-26 实测发现：`selftest_loop()` 早就写好了，但 **`main()` 从来没调用过它**
    #       （`grep -n selftest_loop board_check.py` 只命中它的定义那一行）。
    #       于是这一半从头到尾一次都没跑过 —— 它那 7 条断言是绿是红，没人知道。
    #       **写好的判据没被执行 = 没有判据。** 这条和本工程那条
    #       "没触发过的编译期检查等于没有检查"是同一件事。
    loop_ok = selftest_loop()

    if (npass != len(cases)) or (not loop_ok):
        print("🔴 自测有问题 —— 不要拿它去判真板子的数据。")
        return 1
    print("✅ 判定逻辑与注入/抓包主循环【都】既判得对、也判得错 —— 可以判真板子的数据了。")
    return 0


# ============================================================================
# 七、入口
# ============================================================================

def main():
    ap = argparse.ArgumentParser(description="第十四次上板验证：注入 + 抓包 + 判定")
    ap.add_argument("--port", default="COM4", help="调试口对应的串口（默认 COM4）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--selftest", action="store_true", help="不接板子，只验判定逻辑")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    try:
        records = run_board(args.port, args.baud)
    except Exception as e:  # 串口打不开是最常见的失败，给一句人话而不是栈
        print(f"🔴 打开 {args.port} 失败：{e}")
        print("   检查：板子插了吗 / 这个 COM 号对吗 / 串口助手是不是还占着它？")
        return 2

    res = analyze(records)
    diag = diagnostics(records)

    stamp = time.strftime("%Y%m%d_%H%M%S")
    logfile = f"board_check_{stamp}.log"
    with open(logfile, "w", encoding="utf-8") as fh:
        fh.write(f"# 第十四次上板验证原始抓包  {stamp}  {args.port}@{args.baud}\n")
        fh.write(f"# {len(records)} 行，全程 {TOTAL_S:.0f} 秒\n\n")
        for at, line in records:
            fh.write(f"{at:8.3f}  {line}\n")
        fh.write("\n# ── 判定 ──\n")
        for cid, name, verdict, detail in res:
            fh.write(f"{cid} [{verdict}] {name}：{detail}\n")
        fh.write("\n# ── 诊断 ──\n")
        for d in diag:
            fh.write(d + "\n")

    print("\n" + "=" * 74)
    print("判定结果")
    print("=" * 74)
    for cid, name, verdict, detail in res:
        mark = {"PASS": "✅", "FAIL": "🔴", "INV": "⚠️ "}[verdict]
        print(f"{mark} {cid} [{verdict}] {name}")
        print(f"        {detail}")

    print("\n" + "=" * 74)
    print("诊断（判据红了往这里查）")
    print("=" * 74)
    for d in diag:
        print("  " + d)

    nfail = sum(1 for _, _, v, _ in res if v == "FAIL")
    ninv = sum(1 for _, _, v, _ in res if v == "INV")
    print(f"\n原始日志已写入 {logfile}")
    print(f"合计：PASS {6 - nfail - ninv} / FAIL {nfail} / INV {ninv}")
    return 1 if (nfail or ninv) else 0


if __name__ == "__main__":
    sys.exit(main())
