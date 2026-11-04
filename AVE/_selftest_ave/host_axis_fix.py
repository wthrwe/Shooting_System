# -*- coding: utf-8 -*-
"""host_axis_fix.py -- 把 _selftest_ave/host/ 下两个桌面测试从「旧轴名 + 旧绑定」接到新真相。

【为什么必须改】
  第四十六次把 PAN/TILT 的【值】对调（PAN=水平、TILT=俯仰），第四十七次把名字换成
  YAW/PITCH。但 host 测试里的「轴 <-> 定时器」绑定还是第四十六次【之前】写死的：

      sim_pan  (= 轴0) 绑 fake_TIM2 / CH3
      sim_tilt (= 轴1) 绑 fake_TIM5 / CH4

  而现在的真相是：
      轴0  YAW   (水平) 在 htim5 / CH4 / CC4 族 / DIR PA5
      轴1  PITCH (俯仰) 在 htim2 / CH3 / CC3 族 / DIR PA4

  => 正好相反。症状分两层：
     (1) 宏名没了                  -> 编译不过（这是好事：唯一的、响亮的告警）
     (2) 就算补上宏名，绑定还是反的 -> 测试「看着能过」却测错了轴，把绑定错埋掉

【改法】
  A. test_stepper.c —— 既改名、又对调硬件：
       名字：PAN -> YAW、TILT -> PITCH（与源码一致）
       硬件：两条轴的整条身份对调（fake_TIMx / htimx / CCRx / TIM_IT_CCx /
             TIM_CHANNEL_x / TIM_CCER_CCx* / TIM_CCMR2_OCxM* / TIM_CCMR2_CCxS），
             因为轴0 的定时器从 htim2 换成了 htim5。
       对调后仍自洽的依据（已 grep 核对 AVE_stepper.c）：
             s_cfg[0] = YAW   用 CC4 族（.ocm_pos=OC4M_Pos, .ccxp=CC4P, :43/:45）
             s_cfg[1] = PITCH 用 CC3 族（.ocm_pos=OC3M_Pos, .ccxp=CC3P, :58/:60）
       另外：只出现在 FAIL 消息里的「裸」通道名（OC3M/OC3PE/CC3S/CC3P/CC3E）
             也要跟着换，否则消息会指着错的通道（代码里出现的都带前缀，用词界排除）。

  B. test_gimbal.c —— 纯改名，不碰硬件：
       它走 fake_stepper.c 的「按轴号索引数组」，而**轴号序号没变**（YAW=0/PITCH=1），
       所以只要把名字跟上即可。

本脚本【行数不变】（全是在位替换），跑完自带后置断言。控制台只输出 ASCII。
用法：  python -I host_axis_fix.py            # dry-run，只报条数
        python -I host_axis_fix.py apply      # 真写盘
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "host")

# ---- A1. 硬件身份对调（两条轴整条互换）----
SWAP = [
    ("TIM_CCMR2_OC3M", "TIM_CCMR2_OC4M"),   # _Pos 变体靠前缀关系一起换
    ("TIM_CCMR2_OC3PE", "TIM_CCMR2_OC4PE"),
    ("TIM_CCMR2_CC3S", "TIM_CCMR2_CC4S"),
    ("TIM_CCER_CC3E", "TIM_CCER_CC4E"),
    ("TIM_CCER_CC3P", "TIM_CCER_CC4P"),
    ("TIM_IT_CC3", "TIM_IT_CC4"),
    ("TIM_CHANNEL_3", "TIM_CHANNEL_4"),
    ("fake_TIM2", "fake_TIM5"),
    ("htim2", "htim5"),
    ("CCR3", "CCR4"),
]

# ---- A2. 只出现在 FAIL 消息里的裸通道名（用词界把带前缀的排除掉）----
BARE = [
    ("OC3M", "OC4M"),
    ("OC3PE", "OC4PE"),
    ("CC3S", "CC4S"),
    ("CC3P", "CC4P"),
    ("CC3E", "CC4E"),
]

# ---- A3. 单纯改名（PAN->YAW、TILT->PITCH，与源码一致）----
STE_NAMES = [
    ("AVE_STEPPER_AXIS_PAN", "AVE_STEPPER_AXIS_YAW"),
    ("AVE_STEPPER_AXIS_TILT", "AVE_STEPPER_AXIS_PITCH"),
    ("AVE_PAN_POS_DIR_LEVEL", "AVE_YAW_POS_DIR_LEVEL"),
    ("AVE_TILT_POS_DIR_LEVEL", "AVE_PITCH_POS_DIR_LEVEL"),
    ("AVE_PAN_DIR_PORT", "AVE_YAW_DIR_PORT"),
    ("AVE_PAN_DIR_PIN", "AVE_YAW_DIR_PIN"),
    ("AVE_TILT_DIR_PORT", "AVE_PITCH_DIR_PORT"),
    ("AVE_TILT_DIR_PIN", "AVE_PITCH_DIR_PIN"),
    ("sim_pan", "sim_yaw"),
    ("sim_tilt", "sim_pitch"),
    ("break_pan_", "break_yaw_"),
    ("break_tilt_", "break_pitch_"),
    ("s_bad_pan", "s_bad_yaw"),
    ("s_bad_tilt", "s_bad_pitch"),
]

# ---- B. 云台测试：纯改名 ----
GIM_NAMES = [
    ("AVE_GIMBAL_AXIS_PAN", "AVE_GIMBAL_AXIS_YAW"),
    ("AVE_GIMBAL_AXIS_TILT", "AVE_GIMBAL_AXIS_PITCH"),
    ("AVE_PAN_STEPS_PER_DEG", "AVE_YAW_STEPS_PER_DEG"),
    ("AVE_TILT_STEPS_PER_DEG", "AVE_PITCH_STEPS_PER_DEG"),
]


def _bare_re(tok):
    """匹配一个「裸」标识符：左右都不是 [A-Za-z0-9_]，因此 TIM_CCMR2_OC3M 不算。"""
    return re.compile(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(tok))


def swap_pairs(t, pairs, bare, log):
    """把每一对的 a/b 互换（两趟 + 占位符，避免自噬）。bare=True 时用词界正则。"""
    for i, (a, b) in enumerate(pairs):
        ph = "\x01%d\x01" % i
        if bare:
            pa, pb = _bare_re(a), _bare_re(b)
            na, nb = len(pa.findall(t)), len(pb.findall(t))
            if na + nb == 0:
                continue
            t = pa.sub(ph, t)       # 所有 a -> 占位
            t = pb.sub(a, t)        # 所有 b -> a
            t = t.replace(ph, b)    # 占位 -> b
        else:
            na, nb = t.count(a), t.count(b)
            if na + nb == 0:
                continue
            t = t.replace(a, ph)
            t = t.replace(b, a)
            t = t.replace(ph, b)
        log.append("  %-4s %-18s <-> %-18s  %d/%d"
                   % ("bare" if bare else "swap", a, b, na, nb))
    return t


def rename(t, pairs, log):
    for a, b in pairs:
        n = t.count(a)
        if n:
            t = t.replace(a, b)
            log.append("  name %-24s -> %-24s  %d" % (a, b, n))
    return t


def words(t, log):
    for a, b in (("PAN", "YAW"), ("TILT", "PITCH")):
        r = _bare_re(a)
        n = len(r.findall(t))
        if n:
            t = r.sub(b, t)
            log.append("  word %-24s -> %-24s  %d" % (a, b, n))
    return t


def main():
    apply = (len(sys.argv) > 1 and sys.argv[1] == "apply")

    # ================= A. test_stepper.c =================
    ps = os.path.join(HOST, "test_stepper.c")
    raw = io.open(ps, "rb").read().decode("utf-8")
    nlines = raw.count("\n")
    log = []
    t = raw
    t = swap_pairs(t, SWAP, False, log)
    t = swap_pairs(t, BARE, True, log)
    t = rename(t, STE_NAMES, log)
    t = words(t, log)
    print("== test_stepper.c ==")
    for line in log:
        print(line)

    # --- 后置断言 ---
    left = sorted(set(re.findall(r"[A-Za-z_]*[Pp][Aa][Nn][A-Za-z_]*|[A-Za-z_]*[Tt][Ii][Ll][Tt][A-Za-z_]*", t)))
    assert not left, "残留旧轴名: %s" % left
    for pat, what in ((r"sim_yaw\.hh\s*=\s*&htim5;", "sim_yaw.hh=&htim5"),
                      (r"sim_yaw\.inst\s*=\s*&fake_TIM5;", "sim_yaw.inst=&fake_TIM5"),
                      (r"sim_yaw\.channel\s*=\s*TIM_CHANNEL_4;", "sim_yaw.channel=CH4"),
                      (r"sim_pitch\.hh\s*=\s*&htim2;", "sim_pitch.hh=&htim2"),
                      (r"sim_pitch\.inst\s*=\s*&fake_TIM2;", "sim_pitch.inst=&fake_TIM2"),
                      (r"sim_pitch\.channel\s*=\s*TIM_CHANNEL_3;", "sim_pitch.channel=CH3"),
                      (r"TIM_HandleTypeDef\s+htim2\s*=\s*\{\s*&fake_TIM2\s*\};", "htim2->fake_TIM2"),
                      (r"TIM_HandleTypeDef\s+htim5\s*=\s*\{\s*&fake_TIM5\s*\};", "htim5->fake_TIM5")):
        assert re.search(pat, t), "后置断言失败: %s" % what
    assert t.count("\n") == nlines, "行数变了: %d -> %d" % (nlines, t.count("\n"))
    print("  POST-OK  no legacy axis name; sim_yaw<->TIM5/CH4, sim_pitch<->TIM2/CH3; lines=%d" % nlines)
    if apply:
        io.open(ps, "wb").write(t.encode("utf-8"))

    # ================= B. test_gimbal.c =================
    pg = os.path.join(HOST, "test_gimbal.c")
    raw2 = io.open(pg, "rb").read().decode("utf-8")
    nlines2 = raw2.count("\n")
    log2 = []
    t2 = rename(raw2, GIM_NAMES, log2)
    t2 = words(t2, log2)
    print("== test_gimbal.c ==")
    for line in log2:
        print(line)
    left2 = sorted(set(re.findall(r"[A-Za-z_]*[Pp][Aa][Nn][A-Za-z_]*|[A-Za-z_]*[Tt][Ii][Ll][Tt][A-Za-z_]*", t2)))
    assert not left2, "残留旧轴名: %s" % left2
    assert t2.count("\n") == nlines2, "行数变了"
    print("  POST-OK  no legacy axis name; lines=%d" % nlines2)
    if apply:
        io.open(pg, "wb").write(t2.encode("utf-8"))

    print("WROTE" if apply else "DRY-RUN (nothing written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
