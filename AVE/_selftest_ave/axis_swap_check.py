# -*- coding: utf-8 -*-
"""
axis_swap_check.py -- 两轴命名更正的自测脚本（第四十六次「值对调」+ 第四十七次「改名」）

第三十七~四十六次这条线上的每次改动都跑它。做三件事，都通过才算数：
  A. 源码一致性：AVE_board.h / AVE_stepper.c / AVE_stepper.h / AVE_gimbal.h /
     AVE_gimbal.c / AVE_tuning.h / AVE_app.c 七个文件讲的必须是同一件事
     （宏值、配置表字面量、枚举顺序、新名字）。
  A2. 改名收尾：全树不许再出现 `AVE_...PAN` / `AVE_...TILT` 形式的标识符
      （第四十七次 PAN→YAW、TILT→PITCH）；同时**必须**保住 AVE_tuning.h:241/248
      两行里"旧称 水平 PAN / 旧称 俯仰 TILT"这段**历史引述** —— 那两处是留痕，
      改了名不等于把历史抹掉。
  B. 镜像一致性：从 AVE.hex 里把 s_cfg[] 那张常量表的字节读出来，逐字段核对
     它真的是 (&htim5, CH4, CC4家族...) 和 (&htim2, CH3, CC3家族...)。
     —— 只比 Program Size 是假绿（本工程第二十九次的教训），所以必须落到镜像字节。
     —— 改名**不产生任何机器码差异**，所以这一节的作用是「反证第 47 次没顺手改坏值」。

判据里的地址来自 MDK-ARM/AVE/AVE.map：
    s_cfg  = 0x08006638  (size 88 = 2 x 44)
    htim2  = 0x200000C8
    htim5  = 0x20000110
    GPIOA  = 0x40020000

控制台只输出 ASCII（本工程 GBK 控制台会炸在非 ASCII 的 print 上）。
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # ...\AVE
SRC = os.path.join(ROOT, "AVE")
HEX = os.path.join(ROOT, "MDK-ARM", "AVE", "AVE.hex")

fails = []


def chk(cond, msg):
    print(("PASS  " if cond else "FAIL  ") + msg)
    if not cond:
        fails.append(msg)


def txt(name):
    with open(os.path.join(SRC, name), "rb") as f:
        return f.read().decode("utf-8", "replace")


board = txt("AVE_board.h")
steph = txt("AVE_stepper.h")
stepc = txt("AVE_stepper.c")
gimb = txt("AVE_gimbal.h")
gimbc = txt("AVE_gimbal.c")
tun = txt("AVE_tuning.h")
app = txt("AVE_app.c")


def has(text, pat, msg):
    chk(re.search(pat, text) is not None, msg)


# ---------------------------------------------------------------- A. 源码
print("--- A. source consistency ---")

has(board, r"#define\s+AVE_HTIM_YAW_STEP\s+\(&htim5\)", "board.h: YAW STEP timer = &htim5 (TIM5)")
has(board, r"#define\s+AVE_HTIM_PITCH_STEP\s+\(&htim2\)", "board.h: PITCH STEP timer = &htim2 (TIM2)")
has(board, r"#define\s+AVE_STEP_YAW_CHANNEL\s+TIM_CHANNEL_4", "board.h: YAW channel = TIM_CHANNEL_4")
has(board, r"#define\s+AVE_STEP_PITCH_CHANNEL\s+TIM_CHANNEL_3", "board.h: PITCH channel = TIM_CHANNEL_3")
has(board, r"#define\s+AVE_YAW_STEP_PIN\s+GPIO_PIN_3\b", "board.h: YAW STEP pin = PA3")
has(board, r"#define\s+AVE_PITCH_STEP_PIN\s+GPIO_PIN_10\b", "board.h: PITCH STEP pin = PB10")
has(board, r"#define\s+AVE_YAW_DIR_PIN\s+GPIO_PIN_5\b", "board.h: YAW DIR pin = PA5")
has(board, r"#define\s+AVE_PITCH_DIR_PIN\s+GPIO_PIN_4\b", "board.h: PITCH DIR pin = PA4")
chk(re.search(r"#define\s+AVE_YAW_STEP_PIN\s+GPIO_PIN_10", board) is None,
    "board.h: no leftover 'YAW STEP pin = PB10'")
chk(re.search(r"#define\s+AVE_HTIM_YAW_STEP\s+\(&htim2\)", board) is None,
    "board.h: no leftover 'YAW timer = &htim2'")

has(stepc,
    r"\(AVE_STEP_YAW_CHANNEL\s*==\s*TIM_CHANNEL_4\)\s*&&\s*\(AVE_STEP_PITCH_CHANNEL\s*==\s*TIM_CHANNEL_3\)",
    "stepper.c: static assert expects (YAW==CH4) && (PITCH==CH3)")
chk(re.search(r"AVE_STEP_YAW_CHANNEL\s*==\s*TIM_CHANNEL_3", stepc) is None,
    "stepper.c: no leftover assert 'YAW==CH3'")

# --- s_cfg 两个条目：用 .htim 赋值语句切成两段，避免被花括号/缩进格式绊倒
m0 = re.search(r"\.htim\s*=\s*AVE_HTIM_YAW_STEP", stepc)
m1 = re.search(r"\.htim\s*=\s*AVE_HTIM_PITCH_STEP", stepc)
chk(m0 is not None and m1 is not None and m0.start() < m1.start(),
    "stepper.c: s_cfg[0]=YAW, s_cfg[1]=PITCH (YAW entry comes first)")
if m0 and m1:
    e0 = stepc[m0.start():m1.start()]
    e1 = stepc[m1.start():stepc.index("};", m1.start())]

    CC4 = ["TIM_IT_CC4", "TIM_CCER_CC4E", "TIM_CCMR2_OC4PE", "TIM_CCMR2_OC4M",
           "TIM_CCMR2_OC4M_Pos", "TIM_CCMR2_CC4S", "TIM_CCER_CC4P"]
    CC3 = ["TIM_IT_CC3", "TIM_CCER_CC3E", "TIM_CCMR2_OC3PE", "TIM_CCMR2_OC3M",
           "TIM_CCMR2_OC3M_Pos", "TIM_CCMR2_CC3S", "TIM_CCER_CC3P"]

    for tok in CC4:
        n = len(re.findall(r"\b" + tok + r"\b", e0))
        chk(n == 1, "stepper.c: YAW entry uses %s exactly once (got %d)" % (tok, n))
    for tok in CC3:
        n = len(re.findall(r"\b" + tok + r"\b", e0))
        chk(n == 0, "stepper.c: YAW entry has NO %s (got %d)" % (tok, n))
    for tok in CC3:
        n = len(re.findall(r"\b" + tok + r"\b", e1))
        chk(n == 1, "stepper.c: PITCH entry uses %s exactly once (got %d)" % (tok, n))
    for tok in CC4:
        n = len(re.findall(r"\b" + tok + r"\b", e1))
        chk(n == 0, "stepper.c: PITCH entry has NO %s (got %d)" % (tok, n))

    chk(re.search(r"\.channel\s*=\s*AVE_STEP_YAW_CHANNEL", e0) is not None,
        "stepper.c: YAW entry binds AVE_STEP_YAW_CHANNEL")
    chk(re.search(r"\.channel\s*=\s*AVE_STEP_PITCH_CHANNEL", e1) is not None,
        "stepper.c: PITCH entry binds AVE_STEP_PITCH_CHANNEL")
    chk(re.search(r"\.dir_pin\s*=\s*AVE_YAW_DIR_PIN", e0) is not None,
        "stepper.c: YAW entry binds AVE_YAW_DIR_PIN")
    chk(re.search(r"\.dir_pin\s*=\s*AVE_PITCH_DIR_PIN", e1) is not None,
        "stepper.c: PITCH entry binds AVE_PITCH_DIR_PIN")

# --- 枚举顺序：YAW 必须仍是 0、PITCH 仍是 1（对调的是"值",不是"序号"）
i_yaw = steph.find("AVE_STEPPER_AXIS_YAW")
i_pit = steph.find("AVE_STEPPER_AXIS_PITCH")
chk(0 <= i_yaw < i_pit, "stepper.h: AVE_STEPPER_AXIS_YAW still enumerates before PITCH")
has(steph, r"AVE_STEPPER_AXIS_YAW\s*=\s*0", "stepper.h: AVE_STEPPER_AXIS_YAW == 0")

g_yaw = gimb.find("AVE_GIMBAL_AXIS_YAW")
g_pit = gimb.find("AVE_GIMBAL_AXIS_PITCH")
chk(0 <= g_yaw < g_pit, "gimbal.h: AVE_GIMBAL_AXIS_YAW still enumerates before PITCH")
has(gimb, r"AVE_GIMBAL_AXIS_YAW\s*=\s*0", "gimbal.h: AVE_GIMBAL_AXIS_YAW == 0")

# ---------------------------------------------------------------- A2. 改名收尾
print("--- A2. rename is complete (no AVE_*PAN / AVE_*TILT identifiers left) ---")

LEFTOVER = re.compile(r"\bAVE_[A-Za-z0-9_]*(?:PAN|TILT)[A-Za-z0-9_]*\b")
for nm, text in (("AVE_board.h", board), ("AVE_stepper.h", steph), ("AVE_stepper.c", stepc),
                 ("AVE_gimbal.h", gimb), ("AVE_gimbal.c", gimbc),
                 ("AVE_tuning.h", tun), ("AVE_app.c", app)):
    found = sorted(set(LEFTOVER.findall(text)))
    chk(not found, "%s: no leftover AVE_*PAN/TILT identifier (found %s)" % (nm, found))

# 反向护栏：改名不许把「旧称」这段历史引述一起抹掉
chk(re.search(r'旧称"水平 PAN"', tun) is not None,
    "tuning.h: historical quote of legacy name PAN is still on record")
chk(re.search(r'旧称"俯仰 TILT"', tun) is not None,
    "tuning.h: historical quote of legacy name TILT is still on record")

# ---------------------------------------------------------------- B. 镜像
print("--- B. built image (AVE.hex) ---")

mem = {}
upper = 0
with open(HEX, "r") as f:
    for line in f:
        line = line.strip()
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        data = b[4:4 + n]
        if typ == 0x00:
            for i, dv in enumerate(data):
                mem[upper + addr + i] = dv
        elif typ == 0x04:
            upper = ((data[0] << 8) | data[1]) << 16
        elif typ == 0x01:
            break


def w(a):
    return mem[a] | (mem[a + 1] << 8) | (mem[a + 2] << 16) | (mem[a + 3] << 24)


def h(a):
    return mem[a] | (mem[a + 1] << 8)


S_CFG = 0x08006638
HTIM2 = 0x200000C8
HTIM5 = 0x20000110
GPIOA = 0x40020000
TIM_CH1, TIM_CH2, TIM_CH3, TIM_CH4 = 0x00, 0x04, 0x08, 0x0C

chk(S_CFG in mem, "hex: s_cfg address 0x08006638 is present in the image")
# htim2/htim5 是 .bss 里的对象，地址在 RAM 段 —— 理所当然**不在** flash 镜像里。
# 表里存的是它们的**地址值**，下面按地址值逐字段核对。
chk(HTIM2 not in mem and HTIM5 not in mem,
    "hex: htim2/htim5 are .bss (RAM) objects, correctly absent from the flash image")

# ⚠️ 这里踩过自己的坑：TIM_IT_CCx 是 **DIER 的位** —— CC3IE=bit3=0x08、CC4IE=bit4=0x10，
#    不是 0x200/0x400（那是别处 TIM 寄存器的形状）。第一版脚本按 0x200/0x400 写，
#    于是把**正确**的镜像判成 FAIL。判据值必须逐个从 HAL 定义核过再写进测试。
IT_CC3 = 0x0008
IT_CC4 = 0x0010

# s_cfg[0] = YAW = 水平(左右) = TIM5_CH4 -> PA3, DIR PA5
E0 = S_CFG
chk(w(E0 + 0) == HTIM5, "image s_cfg[0].htim      = &htim5   (got 0x%08X)" % w(E0 + 0))
chk(w(E0 + 4) == TIM_CH4, "image s_cfg[0].channel   = CH4      (got 0x%08X)" % w(E0 + 4))
chk(w(E0 + 8) == IT_CC4, "image s_cfg[0].cc_it     = IT_CC4   (got 0x%08X)" % w(E0 + 8))
chk(w(E0 + 12) == 0x1000, "image s_cfg[0].ccxe      = CCER_CC4E(got 0x%08X)" % w(E0 + 12))
chk(w(E0 + 16) == 0x0800, "image s_cfg[0].ocpe      = CCMR2_OC4PE (got 0x%08X)" % w(E0 + 16))
chk(w(E0 + 20) == 0x7000, "image s_cfg[0].ocm_msk   = CCMR2_OC4M  (got 0x%08X)" % w(E0 + 20))
chk(w(E0 + 24) == 12, "image s_cfg[0].ocm_pos   = OC4M_Pos = 12 (got %d)" % w(E0 + 24))
chk(w(E0 + 28) == 0x0300, "image s_cfg[0].ccs_msk   = CCMR2_CC4S  (got 0x%08X)" % w(E0 + 28))
chk(w(E0 + 32) == 0x2000, "image s_cfg[0].ccxp      = CCER_CC4P  (got 0x%08X)" % w(E0 + 32))
chk(w(E0 + 36) == GPIOA, "image s_cfg[0].dir_port  = GPIOA       (got 0x%08X)" % w(E0 + 36))
chk(h(E0 + 40) == 0x0020, "image s_cfg[0].dir_pin   = PIN_5       (got 0x%04X)" % h(E0 + 40))

# s_cfg[1] = PITCH = 俯仰(上下) = TIM2_CH3 -> PB10, DIR PA4
E1 = S_CFG + 44
chk(w(E1 + 0) == HTIM2, "image s_cfg[1].htim      = &htim2   (got 0x%08X)" % w(E1 + 0))
chk(w(E1 + 4) == TIM_CH3, "image s_cfg[1].channel   = CH3      (got 0x%08X)" % w(E1 + 4))
chk(w(E1 + 8) == IT_CC3, "image s_cfg[1].cc_it     = IT_CC3   (got 0x%08X)" % w(E1 + 8))
chk(w(E1 + 12) == 0x0100, "image s_cfg[1].ccxe      = CCER_CC3E(got 0x%08X)" % w(E1 + 12))
chk(w(E1 + 16) == 0x0008, "image s_cfg[1].ocpe      = CCMR2_OC3PE (got 0x%08X)" % w(E1 + 16))
chk(w(E1 + 20) == 0x0070, "image s_cfg[1].ocm_msk   = CCMR2_OC3M  (got 0x%08X)" % w(E1 + 20))
chk(w(E1 + 24) == 4, "image s_cfg[1].ocm_pos   = OC3M_Pos = 4 (got %d)" % w(E1 + 24))
chk(w(E1 + 28) == 0x0003, "image s_cfg[1].ccs_msk   = CCMR2_CC3S  (got 0x%08X)" % w(E1 + 28))
chk(w(E1 + 32) == 0x0200, "image s_cfg[1].ccxp      = CCER_CC3P  (got 0x%08X)" % w(E1 + 32))
chk(w(E1 + 36) == GPIOA, "image s_cfg[1].dir_port  = GPIOA       (got 0x%08X)" % w(E1 + 36))
chk(h(E1 + 40) == 0x0010, "image s_cfg[1].dir_pin   = PIN_4       (got 0x%04X)" % h(E1 + 40))

# 反证：镜像里不该存在"YAW 条目用 CC3 家族"那套（旧值）
chk(not (w(E0 + 8) == IT_CC3 and w(E0 + 12) == 0x0100),
    "image: s_cfg[0] does NOT carry the old CC3 family (anti-regression)")

# ------------------------------------------------------------------ 结论
print("--- summary ---")
print("checks failed: %d" % len(fails))
sys.exit(1 if fails else 0)
