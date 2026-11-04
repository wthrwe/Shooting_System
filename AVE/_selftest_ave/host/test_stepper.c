/**
 * AVE_stepper.c 的桌面行为测试
 *
 * ── 这个测试到底在测什么 ────────────────────────────────────────────────────
 *   `AVE_stepper.c` 是**寄存器级**的：它直接写 `CR1.CEN` / `CCER.CCxE` /
 *   `DIER.CCxIE` / `ARR` / `CCRx` / `EGR.UG`。所以这里搭了一个**定时器仿真器**：
 *   按 RM0090 的规则一个计数一个计数地推进 `CNT`，在 `CNT == CCR` 时触发比较中断，
 *   并按 "PWM1 + 极性 LOW" 算引脚电平。测试**数引脚的上升沿**来判断走了几步。
 *
 *   ⇒ 于是"位置模式恰好 N 步"、"同向改频后还在出脉冲"这两件**在硬件上很难查、
 *     出错又静默**的事，变成了桌面上能跑的断言。
 *
 * ── ⚠️ 这个测试**不能**证明什么（诚实边界）──────────────────────────────────
 *   ⬜ 仿真器是**照我对手册的理解手写的**，不是硅片。它证明的是
 *      "模块的寄存器时序符合手册模型"，**不证明**"芯片上真的这样"。
 *   ⬜ 完全不碰电气：共阳/共阴接法、光耦电流、驱动器最小脉宽、丢步 —— 一条都没测。
 *   ⬜ 不测 `HAL_TIM_IRQHandler` 的分发（那是 HAL 的代码，桌面上没有）。
 *      这里直接调 `HAL_TIM_PWM_PulseFinishedCallback()`，等于**假设**分发是通的
 *      —— 那条假设的证据在 `MDK-ARM/AVE/AVE.map:176`（见 AVE_stepper.h 文件头）。
 *   ⬜ SR 标志位**没有**按 rc_w0 建模（仿真器只是在事件时置位、在 tick 末尾被
 *      模块的写覆盖）。所以"HAL 有没有清 CCxIF 导致中断重入"这件事**没测**。
 *   ⬜ **T8 的自检证明不了**「CubeMX 真的把定时器配成了 `sim_cubemx_config()` 里那些
 *      值」—— 那一段是我照 `Core/Src/tim.c` **抄进去的预期**。T8 证明的是
 *      "自检的每条判据都是活的、两轴掩码没串、时钟推算两个分支都对"。
 *      **"板上到底是不是这些值"只有开机那条 `[TIM]` 行能回答（上板 ⬜）。**
 *   ⬜ 仿真器**不建模 ARPE**：它无条件在 UEV 时把 ARR/CCR 搬进活动寄存器，
 *      即等价于"ARPE 恒为 1"。所以 `CHK_ARPE` 抓的是寄存器位，**不是**改频行为
 *      在 ARPE=0 下的真实后果（那个后果没有桌面模型）。
 */

#include <stdio.h>
#include <string.h>

#include "AVE_stepper.h"

/* ============================================================================
 * 一、假外设的真身（`fake/stm32f4xx_hal.h` 里声明，这里定义）
 * ==========================================================================*/

TIM_TypeDef         fake_TIM5;
TIM_TypeDef         fake_TIM2;
GPIO_TypeDef        fake_GPIOA;
GPIO_TypeDef        fake_GPIOB;
TIM_HandleTypeDef   htim5 = { &fake_TIM5 };
TIM_HandleTypeDef   htim2 = { &fake_TIM2 };
fake_DWT_Type       fake_dwt;
fake_CoreDebug_Type fake_coredebug;
uint32_t            fake_dwt_access_count;
uint32_t            SystemCoreClock = 168000000U;   /* F407 的 HCLK */

/* RCC：真件里 168 MHz → AHB 168 → APB1 42 MHz → 定时器时钟 84 MHz（RM0090 §6.2）。
   所以 `fake_pclk1_hz` = 42 MHz，而 CFGR.PPRE1 要填成"已分频"（见 sim_reset）。 */
RCC_TypeDef fake_RCC;
uint32_t    fake_pclk1_hz = 42000000U;

uint32_t HAL_RCC_GetPCLK1Freq(void)
{
  return fake_pclk1_hz;
}

/**
 * 每访问一次 `DWT` 就把 `CYCCNT` 推进 1。
 * 目的：让 `stepper_delay_us()` 的忙等**能退出**，且 `dwt_init()` 的自检通过
 * （走 DWT 路而不是空转退路 ⇒ 测试里 `dwt_fallbacks` 应为 0）。
 */
fake_DWT_Type *fake_dwt_touch(void)
{
  fake_dwt_access_count++;
  fake_dwt.CYCCNT++;
  return &fake_dwt;
}

/* ============================================================================
 * 二、定时器仿真器
 * ==========================================================================*/

typedef struct
{
  TIM_HandleTypeDef *hh;
  TIM_TypeDef       *inst;
  uint32_t           channel;      /* TIM_CHANNEL_4 / _4 */
  uint32_t           cc_it;        /* 比较中断使能位 */
  uint32_t           ccxe;         /* 通道输出使能位 */

  /* 影子寄存器：ARPE=1 / OCxPE=1 时，写进去的值要等到 UEV 才生效 */
  uint32_t active_arr;
  uint32_t active_ccr;

  int      pin;                    /* 当前引脚电平：0 = 低（脉冲中），1 = 高（空闲） */
  int      pin_prev;
  uint32_t rising_edges;           /* 0→1 的次数 = 驱动器认为走了几步 */
  uint32_t cc_fired;               /* 比较中断触发次数 */
} SimAxis;

static SimAxis sim_yaw;
static SimAxis sim_pitch;

/** 取该通道的 `CCRx`。CMSIS 里是四个独立成员，必须分支。 */
static uint32_t sim_ccr(const SimAxis *s)
{
  switch (s->channel)
  {
    case TIM_CHANNEL_1: return s->inst->CCR1;
    case TIM_CHANNEL_2: return s->inst->CCR2;
    case TIM_CHANNEL_4: return s->inst->CCR4;
    default:            return s->inst->CCR3;
  }
}

/**
 * 处理 `EGR`。**与 `CEN` 无关** —— RM0090 里 UG 是软件事件，计数器停着也能产生。
 * `EGR` 在硬件里是**只写**的（读回 0），所以这里处理完就清 0，
 * 这样模块下一次写 `UG` 才是一次"新的事件"。
 */
static void sim_apply_egr(SimAxis *s)
{
  if ((s->inst->EGR & TIM_EGR_UG) != 0U)
  {
    s->active_arr = s->inst->ARR;      /* UG 把 ARR 搬进活动寄存器 */
    s->active_ccr = sim_ccr(s);        /* 也把 CCR 搬进去 */
    s->inst->CNT  = 0U;                /* 并把 CNT 归零 */
    s->inst->SR  |= TIM_SR_UIF;
    s->inst->EGR  = 0U;                /* 只写寄存器，读回 0 */
  }
}

/** 推进一个计数周期（1/840 kHz ≈ 1.19 µs）。 */
static void sim_step(SimAxis *s)
{
  int newpin;

  sim_apply_egr(s);

  if ((s->inst->CR1 & TIM_CR1_CEN) != 0U)
  {
    s->inst->CNT++;

    if (s->inst->CNT > s->active_arr)
    {
      /* 溢出 = 更新事件。ARPE=1 ⇒ 新 ARR 在此刻生效；OCxPE=1 ⇒ CCR 也是 */
      s->inst->CNT = 0U;
      s->inst->SR |= TIM_SR_UIF;
      s->active_arr = s->inst->ARR;
      s->active_ccr = sim_ccr(s);
    }

    if (((s->inst->CCER & s->ccxe) != 0U) && (s->inst->CNT == s->active_ccr))
    {
      s->inst->SR |= s->cc_it;
      if ((s->inst->DIER & s->cc_it) != 0U)
      {
        s->cc_fired++;
        /* 这就是目标上的路径：TIMx_IRQHandler → HAL_TIM_IRQHandler → 这里 */
        HAL_TIM_PWM_PulseFinishedCallback(s->hh);
        sim_apply_egr(s);   /* 模块可能在里面写了 EGR（pwm_stop 就会），当场生效 */
      }
    }
  }

  /* PWM1 + 极性 LOW ⇒ CNT < CCR 时输出低电平（有效），否则高（空闲） */
  newpin     = (s->inst->CNT < s->active_ccr) ? 0 : 1;
  s->pin_prev = s->pin;
  s->pin      = newpin;
  if ((s->pin_prev == 0) && (s->pin == 1)) { s->rising_edges++; }
}

/* ============================================================================
 * 三、测试脚手架
 * ==========================================================================*/

static int g_pass;
static int g_fail;

static void check(int cond, const char *what, long got, long want)
{
  if (cond) { g_pass++; printf("  PASS  %s\n", what); }
  else      { g_fail++; printf("  FAIL  %s   (得到 %ld，期望 %ld)\n", what, got, want); }
}

/*
 * 🔴 必须先把两边算进**临时变量**再比。
 *
 * 第一版写的是 `check((long)(got) == (long)(want), what, (long)(got), (long)(want))`
 * —— 于是 `got` 被求值**两次**。而 `got` 常常是 `AVE_Stepper_SetFreqHz(...)`
 * 这种**有副作用**的调用：第一次真的执行了，第二次返回 EBUSY。
 *
 * 症状很阴：打印出来是「得到 0，期望 0」却判 FAIL —— 因为**条件**用的是第一次的
 * 返回值、**打印**用的是第二次的。这条 bug 会同时制造假阴和假阳（比如
 * `gate_rejects` 被记了 2 倍），是脚手架的问题，不是模块的问题。
 */
#define CHECK_EQ(got, want, what)                       \
  do {                                                  \
    long _g = (long)(got);                              \
    long _w = (long)(want);                             \
    check(_g == _w, what, _g, _w);                      \
  } while (0)

#define CHECK_TRUE(cond, what)      check((cond) != 0, what, 0, 1)

static void sim_reset(void)
{
  memset(&fake_TIM5, 0, sizeof(fake_TIM5));
  memset(&fake_TIM2, 0, sizeof(fake_TIM2));
  memset(&fake_GPIOA, 0, sizeof(fake_GPIOA));
  memset(&fake_GPIOB, 0, sizeof(fake_GPIOB));

  memset(&sim_yaw,  0, sizeof(sim_yaw));
  memset(&sim_pitch, 0, sizeof(sim_pitch));

  sim_yaw.hh      = &htim5;  sim_yaw.inst  = &fake_TIM5;     /* 轴0 YAW=水平: TIM5_CH4->PA3 */
  sim_yaw.channel = TIM_CHANNEL_4; sim_yaw.cc_it = TIM_IT_CC4; sim_yaw.ccxe = TIM_CCER_CC4E;
  sim_pitch.hh    = &htim2;  sim_pitch.inst = &fake_TIM2;    /* 轴1 PITCH=俯仰: TIM2_CH3->PB10 */
  sim_pitch.channel = TIM_CHANNEL_3; sim_pitch.cc_it = TIM_IT_CC3; sim_pitch.ccxe = TIM_CCER_CC3E;

  /* 上电电平：`GPIO_NOPULL` 的 AF 引脚在 CCxE=0 之前是悬空的，
     仿真里按"高"起算（空闲），与 CCxE 打开后的确定电平一致 */
  sim_yaw.pin  = sim_yaw.pin_prev  = 1;
  sim_pitch.pin = sim_pitch.pin_prev = 1;

  fake_dwt.CYCCNT = 0U;
  fake_dwt_access_count = 0U;

  /* RCC 复位成 CubeMX 配完的样子：APB1 = /4（PPRE1 = 0b101）⇒ 定时器时钟 84 MHz。
     ⚠️ 这里**只**复位 RCC，不复位 TIM 的 PSC/ARR/CR1 —— 见 sim_cubemx_config() 的说明。 */
  fake_RCC.CFGR  = (0x5U << RCC_CFGR_PPRE1_Pos);
  fake_pclk1_hz  = 42000000U;
}

static void run_ticks(uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++) { sim_step(&sim_yaw); sim_step(&sim_pitch); }
}

/** 跑到该轴不再忙为止（用于位置模式）。返回实际推进的计数个数。 */
static uint32_t run_until_idle(AVE_StepperAxis axis, uint32_t budget)
{
  uint32_t t = 0U;
  while (t < budget)
  {
    sim_step(&sim_yaw); sim_step(&sim_pitch); t++;
    if (AVE_Stepper_IsBusy(axis) == 0) { break; }
  }
  return t;
}

/**
 * 取某轴 `Dir` 引脚电平（0/1），读的是仿真的 `BSRR`。
 *
 * ⚠️ 端口/引脚**故意从 `AVE_board.h` 的宏取**，不写死。
 *    写死（比如两轴都按 GPIOA 读）的话，万一哪天配置挪到 GPIOB，
 *    这个测试会**照样通过** —— 那就成了假绿。
 *
 * ⚠️⚠️ **`BSRR` 是只写寄存器**，仿真里它保存的是**最后一次写入的值**。
 *    一条轴写 `Dir` 会**盖掉**另一条轴的痕迹（两轴的 DIR 都在 GPIOA 上）。
 *    这带来一条使用协议，测试里必须遵守：
 *
 *      **观测某轴的 `Dir` 之前，先 `dir_forget()`；中间不许有另一条轴写 `Dir`。**
 *
 *    不遵守的症状：读到 -1（"两条轴的位都没有"），而实际写入是正确的。
 *    （本测试第一版就是这么误报的 —— 见 `dir_forget()` 的注释。）
 */
static int dir_level(AVE_StepperAxis axis)
{
  GPIO_TypeDef *port;
  uint32_t      pin;

  if (axis == AVE_STEPPER_AXIS_YAW)
  {
    port = AVE_YAW_DIR_PORT;   pin = AVE_YAW_DIR_PIN;
  }
  else
  {
    port = AVE_PITCH_DIR_PORT;  pin = AVE_PITCH_DIR_PIN;
  }

  if ((port->BSRR & pin) != 0U)             { return 1; }   /* 低 16 位 = 置位 */
  if ((port->BSRR & (pin << 16U)) != 0U)    { return 0; }   /* 高 16 位 = 复位 */
  return -1;                                                /* 这个 bit 没被写过 */
}

/**
 * 抹掉 `Dir` 的写入痕迹，好让**下一次**写入成为唯一可观测的那一次。
 *
 * ⚠️ `AVE_Stepper_Init()` **没法这样单独观测** —— 它内部对两条轴**连续**写 `Dir`，
 *    中间没有观测点，`BSRR` 只剩后写的那条（PITCH）。所以 T1 里：
 *      · PITCH 的 `Dir` 直接断言（它就是"最后一次写入"）；
 *      · YAW 的 `Dir` 由下面一次**显式操作**覆盖（先 `dir_forget()` 再调）。
 *    这不是偷懒 —— 是"只写寄存器"在仿真里的固有限制，如实写在这里。
 */
static void dir_forget(void)
{
  fake_GPIOA.BSRR = 0U;
  fake_GPIOB.BSRR = 0U;
}

/* ============================================================================
 * 四、测试
 * ==========================================================================*/

static void t1_init_is_static_and_fail_safe(void)
{
  printf("\n[T1] Init 之后必须是静止的，而且门是关的\n");

  sim_reset();
  AVE_Stepper_Init();

  CHECK_EQ(fake_TIM5.CR1 & TIM_CR1_CEN, 0, "T1.1 YAW 计数器没启动（CEN=0）");
  CHECK_EQ(fake_TIM2.CR1 & TIM_CR1_CEN, 0, "T1.2 PITCH 计数器没启动");
  CHECK_EQ(fake_TIM5.DIER & TIM_IT_CC4, 0, "T1.3 YAW 比较中断没开");
  CHECK_EQ(fake_TIM2.DIER & TIM_IT_CC3, 0, "T1.4 PITCH 比较中断没开");

  /* CCxE 必须**开着**：关着的话 PA3/PB10 是悬空的（把函数头那段理由钉住） */
  CHECK_TRUE((fake_TIM5.CCER & TIM_CCER_CC4E) != 0U, "T1.5 YAW 通道输出使能开着（引脚不悬空）");
  CHECK_TRUE((fake_TIM2.CCER & TIM_CCER_CC3E) != 0U, "T1.6 PITCH 通道输出使能开着");

  /* OCxPE 必须**开着**：CubeMX 没给 OCPreload 赋值，HAL 会清掉它（硬约束六） */
  CHECK_TRUE((fake_TIM5.CCMR2 & TIM_CCMR2_OC4PE) != 0U, "T1.7 YAW 的 OC4PE 已由模块补上");
  CHECK_TRUE((fake_TIM2.CCMR2 & TIM_CCMR2_OC3PE) != 0U, "T1.8 PITCH 的 OC3PE 已由模块补上");

  CHECK_EQ(AVE_Stepper_IsGateOpen(AVE_STEPPER_AXIS_YAW), 0, "T1.9 门默认关着（fail-safe）");
  CHECK_EQ(AVE_Stepper_IsGateOpen(AVE_STEPPER_AXIS_PITCH), 0, "T1.10 门默认关着");

  /* 门关着时叫它转：必须是 EGATE，而且**一个脉冲都不许出** */
  CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000), AVE_STEPPER_EGATE, "T1.11 门关着返回 EGATE");
  CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_PITCH, 1000), AVE_STEPPER_EGATE, "T1.12 门关着返回 EGATE");
  run_ticks(300000U);
  CHECK_EQ(sim_yaw.rising_edges, 0, "T1.13 门关着 → YAW 一步没走");
  CHECK_EQ(sim_pitch.rising_edges, 0, "T1.14 门关着 → PITCH 一步没走");
  CHECK_EQ(AVE_Stepper_Stats()->gate_rejects, 2, "T1.15 gate_rejects 记了 2 次");

  /*
   * Init 把 DIR 摆在正方向电平上。
   * ⚠️ 只能直接断言 PITCH：Init 内部对两条轴**连续**写 Dir，中间没有观测点，
   *    仿真的 BSRR 只剩后写的那条（PITCH）。YAW 的那一份由下面一次显式操作覆盖。
   */
  CHECK_EQ(dir_level(AVE_STEPPER_AXIS_PITCH), AVE_PITCH_POS_DIR_LEVEL,
           "T1.16 PITCH 上电 DIR = 正方向电平（它是 Init 里最后一次写入的）");

  dir_forget();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);
  CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, -1000), AVE_STEPPER_EOK, "T1.17 YAW 反向 1000 Hz 接受");
  CHECK_EQ(dir_level(AVE_STEPPER_AXIS_YAW), AVE_YAW_POS_DIR_LEVEL ? 0 : 1,
           "T1.18 ★ YAW 的 Dir 写对了端口/引脚，且反方向电平正确");
  CHECK_EQ(AVE_Stepper_Stats()->dir_changes, 1, "T1.19 反向计一次换向");
  AVE_Stepper_Stop(AVE_STEPPER_AXIS_YAW);
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 0);
}

static void t2_freq_math(void)
{
  printf("\n[T2] 频率 → ARR / CCR 换算，以及「实际频率」与「请求频率」的差\n");

  /* 先把这三个常数钉死：下面的算式全靠它们 */
  CHECK_EQ(AVE_STEPPER_TIMEBASE_HZ, 840000, "T2.0 时基 = 840 kHz");
  CHECK_EQ(AVE_STEPPER_MIN_HZ, 1, "T2.1 MIN_HZ = 1");
  CHECK_EQ(AVE_STEPPER_MAX_HZ, 40000, "T2.2 MAX_HZ = 40000");

  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);

  CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000), AVE_STEPPER_EOK, "T2.4 1000 Hz 接受");
  CHECK_EQ(fake_TIM5.ARR, 839, "T2.5 1000 Hz → ARR = 840000/1000 - 1 = 839");
  CHECK_EQ(fake_TIM5.CCR4, 420, "T2.6 1000 Hz → CCR = 840/2 = 420（约 50% 占空）");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 1000, "T2.7 实际频率 = 1000");
  CHECK_TRUE((fake_TIM5.CR1 & TIM_CR1_CEN) != 0U, "T2.8 计数器起来了");

  /* 40 kHz：脉宽 = 10/840000 = 11.9 µs，仍在驱动器能认的范围里 */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 40000);
  CHECK_EQ(fake_TIM5.ARR, 20, "T2.9 40000 Hz → ARR = 20");
  CHECK_EQ(fake_TIM5.CCR4, 10, "T2.10 40000 Hz → CCR = 10 ⇒ 脉宽 11.9 µs");

  /* ❗ 关键：999 Hz 除不尽，实际输出是 1000 Hz —— GetFreqHz 必须说实话 */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 999);
  CHECK_EQ(fake_TIM5.ARR, 839, "T2.11 999 Hz → div = 840000/999 = 840 ⇒ ARR = 839");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 1000,
           "T2.12 ★ 999 Hz 请求 → GetFreqHz 报**实际值 1000**，不是回显请求值");

  /* 下限 1 Hz */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1);
  CHECK_EQ(fake_TIM5.ARR, 839999, "T2.13 1 Hz → ARR = 839999（32 位 ARR 装得下，没有 16 位那个 12.8 Hz 下限）");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 1, "T2.14 实际频率 = 1");
}

static void t3_position_exactly_n(void)
{
  uint32_t i;
  static const int32_t counts[3] = { 250, 17, 1 };

  printf("\n[T3] 位置模式：走 N 步就得**恰好**是 N 步（比较中断 vs 更新中断在这里分胜负）\n");

  for (i = 0; i < 3U; i++)
  {
    long before;

    sim_reset();
    AVE_Stepper_Init();
    AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);
    AVE_Stepper_SetGate(AVE_STEPPER_AXIS_PITCH, 1);

    before = (long)AVE_Stepper_Stats()->move_done;

    CHECK_EQ(AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, counts[i], 1000), AVE_STEPPER_EOK, "T3.a 启动返回 EOK");
    CHECK_EQ(AVE_Stepper_IsBusy(AVE_STEPPER_AXIS_YAW), 1, "T3.b 启动后立刻是忙的（不等第一次中断）");

    (void)run_until_idle(AVE_STEPPER_AXIS_YAW, 4000000U);

    CHECK_EQ(AVE_Stepper_IsBusy(AVE_STEPPER_AXIS_YAW), 0, "T3.c 走完自己停了");
    CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 0, "T3.d 停下后频率归零");
    {
      char msg[96];
      sprintf(msg, "T3.e ★ N=%ld → 恰好 %ld 个上升沿", (long)counts[i], (long)counts[i]);
      CHECK_EQ(sim_yaw.rising_edges, counts[i], msg);
    }
    CHECK_EQ(AVE_Stepper_Stats()->move_done, before + 1, "T3.f move_done +1");

    /* 停住之后不许再动：再跑 20 万个计数，边沿数不许变 */
    {
      uint32_t locked = sim_yaw.rising_edges;
      run_ticks(200000U);
      CHECK_EQ(sim_yaw.rising_edges, locked, "T3.g 走完之后**不再**出脉冲");
    }
    CHECK_EQ(sim_pitch.rising_edges, 0, "T3.h 另一条轴完全没受影响");

    /* 断言空闲态是"确定的高电平"，不是悬空、也不是杵在低电平上 */
    CHECK_EQ(sim_yaw.pin, 1, "T3.i 空闲时 Stp 被驱到高电平（光耦不导通）");
    CHECK_EQ(fake_TIM5.CR1 & TIM_CR1_CEN, 0, "T3.j 空闲时计数器是停的");
    CHECK_EQ(fake_TIM5.CCR4, 0, "T3.k 空闲时 CCR 被清 0");
  }

  /* 反方向：DIR 换掉，步数照样准 */
  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);

  /* Init 已把 DIR 摆在正方向，所以这一次反向请求**一定**会写 Dir（可观测） */
  dir_forget();
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, -1000);
  run_ticks(5000U);
  CHECK_EQ(dir_level(AVE_STEPPER_AXIS_YAW), AVE_YAW_POS_DIR_LEVEL ? 0 : 1,
           "T3.l ★ 反向时 DIR = 反方向电平（dir_apply 的端口/引脚/极性都对）");

  sim_yaw.rising_edges = 0U;
  CHECK_EQ(AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, -100, 1000), AVE_STEPPER_EOK, "T3.m 反向定位启动");
  (void)run_until_idle(AVE_STEPPER_AXIS_YAW, 4000000U);
  CHECK_EQ(dir_level(AVE_STEPPER_AXIS_YAW), AVE_YAW_POS_DIR_LEVEL ? 0 : 1, "T3.n 定位期间 DIR 保持反方向电平");
  CHECK_EQ(sim_yaw.rising_edges, 100, "T3.o ★ 反向也是恰好 100 步");
}

static void t4_same_direction_reprogram_keeps_pulsing(void)
{
  uint32_t i;
  uint32_t edges_at_1s;

  printf("\n[T4] ★★ 硬约束五：同向改频**不许**重启计数器（这个错了是静默的）\n");

  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);

  /*
   * 复现 PID 的节奏：1 Hz（ARR = 839999，一个周期要 840000 个计数 = 1 秒），
   * 每 10 ms（8400 个计数）就调一次 SetFreqHz，共 240 次 = 2.4 秒。
   *
   * 正确实现（只写影子寄存器）：2.4 秒里应该出 2 个上升沿。
   * 错误实现（每次改频都 CEN=0 → CNT=0 → CEN=1）：CNT 永远走不到 839999
   *   ⇒ **一个上升沿都没有**，而且零报错。
   */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1);

  for (i = 0U; i < 240U; i++)
  {
    run_ticks(8400U);                                  /* 10 ms */
    AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1);     /* 同频，也应该只改影子寄存器 */
  }

  edges_at_1s = sim_yaw.rising_edges;
  printf("        （1 Hz 跑了 2.4 秒，得到 %u 个上升沿）\n", edges_at_1s);

  CHECK_TRUE(edges_at_1s >= 2U, "T4.1 ★★★ 1 Hz 下反复改频，脉冲**还在出**（>=2 个沿）");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 1, "T4.2 频率仍是 1 Hz");
  CHECK_TRUE((fake_TIM5.CR1 & TIM_CR1_CEN) != 0U, "T4.3 计数器一直没被停掉");

  /* 40 kHz 那一段：改频要真的生效（不能被"延迟到下一个 UEV"变成永不生效） */
  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000);
  run_ticks(840U * 20U);                                /* 20 个脉冲 */
  {
    uint32_t at_1k = sim_yaw.rising_edges;
    AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 2000);
    run_ticks(840U * 20U);                              /* 同样多的计数，频率翻倍 ⇒ 脉冲数约翻倍 */
    CHECK_TRUE((sim_yaw.rising_edges - at_1k) >= 39U,
               "T4.4 ★ 改成 2000 Hz 后，同样时间里脉冲数翻倍（改频真的生效）");
  }
}

static void t5_stop_and_rejections(void)
{
  AVE_StepperStats st;

  printf("\n[T5] 停、打断、以及各种拒绝路径\n");

  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);

  /* 位置模式跑一半被打断 */
  {
    uint32_t before_stop;

    AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, 100000, 1000);
    run_ticks(840U * 30U);                               /* 1000 Hz ⇒ 正好 30 个周期 */
    before_stop = sim_yaw.rising_edges;
    CHECK_TRUE(before_stop > 0U, "T5.1 打断前确实在走");

    CHECK_EQ(AVE_Stepper_Stop(AVE_STEPPER_AXIS_YAW), AVE_STEPPER_EOK, "T5.2 Stop 返回 EOK");
    CHECK_EQ(AVE_Stepper_IsBusy(AVE_STEPPER_AXIS_YAW), 0, "T5.3 打断后不忙了");
    CHECK_EQ(AVE_Stepper_Stats()->move_aborted, 1, "T5.4 move_aborted 记了 1 次");

    /*
     * 先跑几个 tick，让 `pwm_stop()` 引起的电平变化被采样到
     * （仿真里引脚电平是在 tick 末尾才重算的）。
     *
     * 🔴 打断**可能多走 1 步**：停的那一刻若正处在脉冲中途（引脚还是低电平），
     *    把它拉回空闲高电平就**产生了一个上升沿**。这是"立即停"的固有代价，
     *    已经写在 `AVE_stepper.h` 里 `AVE_Stepper_Stop()` 的注释中。
     *    这里把上界钉死：最多只许多 1 步。
     */
    run_ticks(4U);
    printf("        （打断前 %u 步，打断后 %u 步 ⇒ 多走了 %u 步）\n",
           before_stop, sim_yaw.rising_edges, sim_yaw.rising_edges - before_stop);
    CHECK_TRUE(sim_yaw.rising_edges <= before_stop + 1U, "T5.5a ★ 打断最多只多走 1 步，不会失控");

    before_stop = sim_yaw.rising_edges;
    run_ticks(200000U);
    CHECK_EQ(sim_yaw.rising_edges, before_stop, "T5.5b ★★ 停下之后一步都不再走");
  }
  CHECK_EQ(sim_yaw.pin, 1, "T5.6 Stop 之后引脚回到确定的空闲高电平");

  /* 位置模式跑着的时候不接受新指令 */
  AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, 100000, 1000);
  run_ticks(8400U);
  CHECK_EQ(AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, 10, 1000), AVE_STEPPER_EBUSY, "T5.7 定位中再来一条 → EBUSY");
  CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 500), AVE_STEPPER_EBUSY, "T5.8 定位中改频 → EBUSY");
  CHECK_EQ(AVE_Stepper_Stats()->busy_rejects, 2, "T5.9 busy_rejects = 2");
  AVE_Stepper_Stop(AVE_STEPPER_AXIS_YAW);

  /* 范围拒绝：拒绝时**原频率不许动** */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000);
  {
    uint32_t arr_before = fake_TIM5.ARR;
    CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, (int32_t)AVE_STEPPER_MAX_HZ + 1), AVE_STEPPER_ERANGE, "T5.10 超过 MAX_HZ → ERANGE");
    CHECK_EQ(fake_TIM5.ARR, arr_before, "T5.11 ★ 被拒绝时 ARR 原样不动（不是「先停一下再报错」）");
    CHECK_TRUE((fake_TIM5.CR1 & TIM_CR1_CEN) != 0U, "T5.12 被拒绝时还在继续跑");
  }
  AVE_Stepper_Stop(AVE_STEPPER_AXIS_YAW);

  CHECK_EQ(AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, (int32_t)AVE_STEPPER_MAX_MOVE_PULSES + 1, 1000), AVE_STEPPER_ERANGE, "T5.13 脉冲数超上限 → ERANGE");
  CHECK_EQ(AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, (int32_t)0x80000000, 1000), AVE_STEPPER_ERANGE,
           "T5.14 ★ INT32_MIN 不会取负溢出（走 int64，结果仍是 ERANGE）");
  CHECK_EQ(AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_YAW, 0, 1000), AVE_STEPPER_EOK, "T5.15 0 步是合法的空动作");
  CHECK_EQ(AVE_Stepper_IsBusy(AVE_STEPPER_AXIS_YAW), 0, "T5.16 空动作之后不忙");

  /* 0 Hz = 停 */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000);
  CHECK_EQ(AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 0), AVE_STEPPER_EOK, "T5.17 0 Hz = 停，返回 EOK");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 0, "T5.18 停之后频率 = 0");
  CHECK_EQ(fake_TIM5.CR1 & TIM_CR1_CEN, 0, "T5.19 停之后计数器停了");

  /* 关闸要立刻停脉冲 */
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000);
  CHECK_EQ(AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 0), AVE_STEPPER_EOK, "T5.20 关闸返回 EOK");
  CHECK_EQ(fake_TIM5.CR1 & TIM_CR1_CEN, 0, "T5.21 ★ 关闸立刻停脉冲（不是只拦新指令）");
  CHECK_EQ(AVE_Stepper_IsGateOpen(AVE_STEPPER_AXIS_YAW), 0, "T5.22 门确实关了");

  /* 轴号越界 */
  CHECK_EQ(AVE_Stepper_SetFreqHz((AVE_StepperAxis)99, 1000), AVE_STEPPER_EBADARG, "T5.23 轴号越界 → EBADARG");
  CHECK_EQ(AVE_Stepper_MovePulses((AVE_StepperAxis)-1, 10, 1000), AVE_STEPPER_EBADARG, "T5.24 负轴号 → EBADARG");
  CHECK_EQ(AVE_Stepper_Stop((AVE_StepperAxis)7), AVE_STEPPER_EBADARG, "T5.25 Stop 轴号越界 → EBADARG");

  st = *AVE_Stepper_Stats();
  printf("        （累计：range_rejects=%u, busy_rejects=%u, gate_rejects=%u, stray_irq=%u, dwt_fallbacks=%u）\n",
         st.range_rejects, st.busy_rejects, st.gate_rejects, st.stray_irq, st.dwt_fallbacks);
}

static void t6_direction_change_ordering(void)
{
  uint32_t before;

  printf("\n[T6] 换向：停脉冲 → 改 DIR → 等建立时间 → 才出脉冲\n");

  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);

  CHECK_EQ(AVE_Stepper_Stats()->dir_changes, 0, "T6.1 Init 之后换向次数 = 0（DIR 已经是对的了）");

  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000);
  CHECK_EQ(AVE_Stepper_Stats()->dir_changes, 0, "T6.2 第一次正向不算换向（Init 已摆好）");
  CHECK_EQ(fake_dwt_access_count > 0U, 1, "T6.3 起步也走了建立时间");

  run_ticks(8400U);

  /* 换向：DWT 的访问次数应该跳增（= 真的等了 10 µs） */
  before = fake_dwt_access_count;
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, -1000);

  CHECK_EQ(AVE_Stepper_Stats()->dir_changes, 1, "T6.4 换向记了 1 次");
  CHECK_EQ(dir_level(AVE_STEPPER_AXIS_YAW), AVE_YAW_POS_DIR_LEVEL ? 0 : 1, "T6.5 DIR 电平已翻");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), -1000, "T6.6 频率带负号");
  CHECK_TRUE((fake_dwt_access_count - before) >= 1680U,
             "T6.7 ★ 换向真的等了 10 µs（DWT 被轮询 >= 1680 次 = 10µs × 168MHz）");
  CHECK_EQ(AVE_Stepper_Stats()->dwt_fallbacks, 0, "T6.8 走的是 DWT 路，不是空转退路");

  {
    uint32_t locked = sim_yaw.rising_edges;
    run_ticks(8400U);
    CHECK_TRUE(sim_yaw.rising_edges > locked, "T6.9 换向之后继续出脉冲");
  }

  /* 同一个方向再给一次：**不许**算换向、**不许**再等建立时间 */
  before = fake_dwt_access_count;
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, -2000);
  CHECK_EQ(AVE_Stepper_Stats()->dir_changes, 1, "T6.10 同向改频不算换向");
  CHECK_EQ(fake_dwt_access_count, before, "T6.11 ★ 同向改频**不**等建立时间（不白花 10 µs）");

  /* 上下两条轴互不干扰 */
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_PITCH, 1);
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_PITCH, 500);
  run_ticks(84000U);
  CHECK_TRUE(sim_pitch.rising_edges > 0U, "T6.12 PITCH 同时也在走");
  CHECK_TRUE(sim_yaw.rising_edges > 0U, "T6.13 YAW 没被 PITCH 影响");
  CHECK_TRUE((fake_TIM2.ARR != fake_TIM5.ARR), "T6.14 两轴 ARR 各自独立");
}

static void t7_stray_interrupt(void)
{
  TIM_TypeDef      bogus_inst;
  TIM_HandleTypeDef bogus;

  printf("\n[T7] 认不出来的中断源：只记账，不猜\n");

  sim_reset();
  AVE_Stepper_Init();

  bogus.Instance = &bogus_inst;
  AVE_Stepper_PulseFinishedIsr(&bogus);
  CHECK_EQ(AVE_Stepper_Stats()->stray_irq, 1, "T7.1 陌生 Instance → stray_irq = 1");

  AVE_Stepper_PulseFinishedIsr(NULL);
  CHECK_EQ(AVE_Stepper_Stats()->stray_irq, 2, "T7.2 NULL 句柄 → stray_irq = 2（不崩）");

  /* 速度模式下不该有比较中断。真有的话记账，但**不许**把电机停了 */
  sim_reset();
  AVE_Stepper_Init();
  AVE_Stepper_SetGate(AVE_STEPPER_AXIS_YAW, 1);
  AVE_Stepper_SetFreqHz(AVE_STEPPER_AXIS_YAW, 1000);
  AVE_Stepper_PulseFinishedIsr(&htim5);
  CHECK_EQ(AVE_Stepper_Stats()->stray_irq, 1, "T7.3 速度模式下收到比较中断 → 记账");
  CHECK_EQ(fake_TIM5.CR1 & TIM_CR1_CEN, 1, "T7.4 ★ 但**没有**擅自把脉冲停掉");
  CHECK_EQ(AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW), 1000, "T7.5 频率没被弄乱");
}

/* ============================================================================
 * 五、定时器配置自检（T8）
 * ==========================================================================*/

/**
 * 把两条轴配成 `MX_TIM2_Init()` / `MX_TIM5_Init()` **留下的样子**。
 *
 * 🔴 这一段是**被测对象之外的输入**：自检要验的正是"这些值还在不在"。
 *    所以必须逐条抄真件 `Core/Src/tim.c`，抄错了这个测试会**在错的地方变绿**
 *    （比如以为 CubeMX 会置 OCxPE，那 T8 就永远抓不到"模块补置 OCxPE"这件事）。
 *
 * 🔴 刻意**不**放进 `sim_reset()`：那会让现有 119 条用例的运行环境变样，
 *    而它们的结论是在旧环境下取得的。要改环境就得把那 119 条重新判一遍。
 */
static void sim_cubemx_config(void)
{
  /* tim.c:46 / :100   APB1 定时器时钟 84 MHz / 840 kHz = 100 ⇒ PSC = 99 */
  fake_TIM2.PSC = 99U;
  fake_TIM5.PSC = 99U;

  /* tim.c:48 / :102   周期先摆成 100 Hz 的闲值，运行时会由 pwm_run 改写 */
  fake_TIM2.ARR = 8399U;
  fake_TIM5.ARR = 8399U;

  /* tim.c:50 / :104   AutoReloadPreload = ENABLE ⇒ CR1.ARPE = 1 */
  fake_TIM2.CR1 |= TIM_CR1_ARPE;
  fake_TIM5.CR1 |= TIM_CR1_ARPE;

  /* tim.c:70 / :124   OCMode = PWM1（110b）：CH3 在 bit6:4，CH4 在 bit14:12 */
  fake_TIM2.CCMR2 |= (0x6U << TIM_CCMR2_OC3M_Pos);
  fake_TIM5.CCMR2 |= (0x6U << TIM_CCMR2_OC4M_Pos);

  /* tim.c:72 / :126   OCPolarity = LOW ⇒ **CCxP = 1**（STM32 里置 1 才是低有效）。
     🔴 这一行原本写的是"⇒ CCxP = 0。本来就是 0，显式写出来当判据"——**是错的**，而且错得隐蔽：
     仿真器和自检共享了同一个反向的心智模型，于是桌面 **166 项全过**，
     直到上板才由 `[TIM]` 报出 `bad=10,10`（第二十八次）。
     ⇒ 这就是"抄下来的预期，抄错就一起错"的实证。现按**硅片实测**显式置上：
     SWD 直读 TIM2 `CCER=0x0300`、TIM5 `CCER=0x3000`，两轴 CCxP 都是 1。 */
  fake_TIM2.CCER |= TIM_CCER_CC3P;
  fake_TIM5.CCER |= TIM_CCER_CC4P;

  /* ⚠️ **故意不置 OCxPE**：CubeMX 没给 sConfigOC.OCPreload 赋值，HAL 见 0 就把它清掉。
     这正是 AVE_Stepper_Init() 必须自己补的那一位（见 AVE_stepper.c 的 init_one_axis）。
     这里替 CubeMX 补上就永远测不到那条修复。 */
}

/*
 * `AVE_Stepper_Init()` 补上、而 CubeMX **不给**的两样。
 * 这也是"Init 至少要干这两件事"的可失败判据：漏掉任一件，T8.b 立刻变红。
 */
#define AVE_TEST_INIT_SUPPLIES   (AVE_STEPPER_CHK_OCPE | AVE_STEPPER_CHK_CCE)

/* ---- 逐条反例：一次只弄坏一样，对应的位必须亮，而且**只有**它亮 ---- */

typedef void (*bad_setter)(void);

typedef struct
{
  uint32_t    bit;      /* 应该亮的位 */
  const char *what;     /* 弄坏了什么（人看的） */
  bad_setter  make;
} BadCase;

static void break_yaw_arpe(void)   { fake_TIM5.CR1  &= ~TIM_CR1_ARPE; }
static void break_yaw_cms(void)    { fake_TIM5.CR1  |= TIM_CR1_CMS; }
static void break_yaw_ocpe(void)   { fake_TIM5.CCMR2 &= ~TIM_CCMR2_OC4PE; }
static void break_yaw_ccs(void)    { fake_TIM5.CCMR2 |= TIM_CCMR2_CC4S; }
static void break_yaw_pol(void)    { fake_TIM5.CCER  &= ~TIM_CCER_CC4P; }
static void break_yaw_cce(void)    { fake_TIM5.CCER  &= ~TIM_CCER_CC4E; }
static void break_yaw_psc(void)    { fake_TIM5.PSC    = 98U; }
static void break_yaw_ocmode(void)
{
  fake_TIM5.CCMR2 = (fake_TIM5.CCMR2 & ~TIM_CCMR2_OC4M) | (0x7U << TIM_CCMR2_OC4M_Pos);
}

static void break_pitch_ocpe(void)  { fake_TIM2.CCMR2 &= ~TIM_CCMR2_OC3PE; }
static void break_pitch_pol(void)   { fake_TIM2.CCER  &= ~TIM_CCER_CC3P; }
static void break_pitch_ocmode(void)
{
  fake_TIM2.CCMR2 = (fake_TIM2.CCMR2 & ~TIM_CCMR2_OC3M) | (0x7U << TIM_CCMR2_OC3M_Pos);
}

static const BadCase s_bad_yaw[] =
{
  { AVE_STEPPER_CHK_ARPE,   "YAW CR1.ARPE 被清（同向改频会撕波形）",     break_yaw_arpe   },
  { AVE_STEPPER_CHK_CMS,    "YAW CR1.CMS 被设成中心对齐（脉冲数翻倍）",  break_yaw_cms    },
  { AVE_STEPPER_CHK_OCMODE, "YAW OC4M 被改成非 PWM1",                   break_yaw_ocmode },
  { AVE_STEPPER_CHK_OCPE,   "YAW OC4PE 被清（= Init 没跑到）",          break_yaw_ocpe   },
  { AVE_STEPPER_CHK_CCS,    "YAW CC4S 被设成输入捕获",                  break_yaw_ccs    },
  { AVE_STEPPER_CHK_POL,    "YAW CC4P 被清（丢掉低有效 ⇒ 光耦常亮）",   break_yaw_pol    },
  { AVE_STEPPER_CHK_CCE,    "YAW CC4E 被清（引脚悬空）",                break_yaw_cce    },
  { AVE_STEPPER_CHK_PSC,    "YAW PSC 被改成 98（所有速度成比例偏）",    break_yaw_psc    },
};

static const BadCase s_bad_pitch[] =
{
  { AVE_STEPPER_CHK_OCMODE, "PITCH OC3M 被改成非 PWM1",                  break_pitch_ocmode },
  { AVE_STEPPER_CHK_OCPE,   "PITCH OC3PE 被清",                          break_pitch_ocpe   },
  { AVE_STEPPER_CHK_POL,    "PITCH CC3P 被清（同上）",                   break_pitch_pol    },
};

static void t8_timer_selftest(void)
{
  AVE_StepperSelfTest st;
  uint32_t            i;
  long                r;

  printf("\n[T8] 定时器配置自检：把「配置漂了也没有任何症状」变成可失败的判据\n");

  /* ---- T8.a 没调 Init 之前：恰好 OCxPE + CCxE 两位亮 ---- */
  sim_reset();
  sim_cubemx_config();
  r = (long)AVE_Stepper_SelfTest(&st);

  CHECK_EQ(r, AVE_STEPPER_ECONFIG, "T8.1 有配置不符时返回 ECONFIG（不是 EOK）");
  CHECK_EQ(st.bad[AVE_STEPPER_AXIS_YAW], AVE_TEST_INIT_SUPPLIES,
           "T8.2 ★★ Init 之前 YAW 恰好亮 OCxPE|CCxE 两位（CubeMX 不给这两样）");
  CHECK_EQ(st.bad[AVE_STEPPER_AXIS_PITCH], AVE_TEST_INIT_SUPPLIES,
           "T8.3 ★★ Init 之前 PITCH 同样恰好那两位");

  /* 时钟推算：APB1 = /4 ⇒ PCLK1 42 MHz ⇒ 定时器时钟 ×2 = 84 MHz */
  CHECK_EQ(st.timer_clk_hz, 84000000, "T8.4 ★ PPRE1=/4 ⇒ 定时器时钟 = PCLK1×2 = 84 MHz");
  CHECK_EQ(st.expected_psc, 99,       "T8.5 ★ 84 MHz / 840 kHz − 1 = 99");
  CHECK_EQ(st.timebase_hz, AVE_STEPPER_TIMEBASE_HZ, "T8.6 快照里的时基就是模块用的那个宏");
  CHECK_EQ(st.psc[AVE_STEPPER_AXIS_YAW], 99, "T8.7 读回来的 PSC 是实际值 99");

  /* ---- T8.b 调完 Init：必须全 0 ---- */
  AVE_Stepper_Init();
  r = (long)AVE_Stepper_SelfTest(&st);

  CHECK_EQ(r, AVE_STEPPER_EOK, "T8.8 ★★ Init 之后返回 EOK");
  CHECK_EQ(st.bad[AVE_STEPPER_AXIS_YAW],  0, "T8.9 ★★ YAW bad = 0");
  CHECK_EQ(st.bad[AVE_STEPPER_AXIS_PITCH], 0, "T8.10 ★★ PITCH bad = 0");

  /* 原始寄存器值也读回来了，且两轴的掩码没有串位 */
  CHECK_EQ(st.ccmr2[AVE_STEPPER_AXIS_YAW]  & TIM_CCMR2_OC4M, (long)(0x6U << TIM_CCMR2_OC4M_Pos),
           "T8.11 YAW 的 OC4M 读出来是 110b（掩码/位移对）");
  CHECK_EQ(st.ccmr2[AVE_STEPPER_AXIS_PITCH] & TIM_CCMR2_OC3M, (long)(0x6U << TIM_CCMR2_OC3M_Pos),
           "T8.12 ★ PITCH 的 OC3M 在 bit6:4，不在 bit14:12（两轴掩码真的分开的）");
  CHECK_EQ(st.ccer[AVE_STEPPER_AXIS_YAW]  & TIM_CCER_CC4E, TIM_CCER_CC4E, "T8.13 YAW CC4E = 1");
  CHECK_EQ(st.ccer[AVE_STEPPER_AXIS_PITCH] & TIM_CCER_CC3E, TIM_CCER_CC3E, "T8.14 PITCH CC3E = 1");

  /* ---- T8.c 逐条反例：一次只弄坏一样 ---- */
  for (i = 0U; i < (sizeof(s_bad_yaw) / sizeof(s_bad_yaw[0])); i++)
  {
    char msg[128];

    sim_reset();
    sim_cubemx_config();
    AVE_Stepper_Init();
    s_bad_yaw[i].make();

    (void)AVE_Stepper_SelfTest(&st);
    sprintf(msg, "T8.P%u ★ %s → YAW 恰好亮那一位", (unsigned)i + 1U, s_bad_yaw[i].what);
    CHECK_EQ(st.bad[AVE_STEPPER_AXIS_YAW], s_bad_yaw[i].bit, msg);
    CHECK_EQ(st.bad[AVE_STEPPER_AXIS_PITCH], 0, "        └ PITCH 不受影响（两轴不串）");
  }

  for (i = 0U; i < (sizeof(s_bad_pitch) / sizeof(s_bad_pitch[0])); i++)
  {
    char msg[128];

    sim_reset();
    sim_cubemx_config();
    AVE_Stepper_Init();
    s_bad_pitch[i].make();

    (void)AVE_Stepper_SelfTest(&st);
    sprintf(msg, "T8.T%u ★ %s → PITCH 恰好亮那一位", (unsigned)i + 1U, s_bad_pitch[i].what);
    CHECK_EQ(st.bad[AVE_STEPPER_AXIS_PITCH], s_bad_pitch[i].bit, msg);
    CHECK_EQ(st.bad[AVE_STEPPER_AXIS_YAW], 0, "        └ YAW 不受影响");
  }

  /* ---- T8.d 时钟推算的两个分支 ---- */
  sim_reset();
  sim_cubemx_config();
  AVE_Stepper_Init();

  /* APB1 不分频（PPRE1 = 0）：定时器时钟**就是** PCLK1，不许乘 2 */
  fake_RCC.CFGR = 0U;
  (void)AVE_Stepper_SelfTest(&st);
  CHECK_EQ(st.timer_clk_hz, 42000000, "T8.15 ★ PPRE1=0 ⇒ 定时器时钟 = PCLK1 = 42 MHz（**不**乘 2）");
  CHECK_EQ(st.expected_psc, 49,       "T8.16 ★ 42 MHz / 840 kHz − 1 = 49");
  CHECK_TRUE((st.bad[AVE_STEPPER_AXIS_YAW] & AVE_STEPPER_CHK_PSC) != 0U,
             "T8.17 ★★ 此时 PSC 还是 99 ⇒ CHK_PSC 亮（分频判据是活的）");

  /* 时钟被降级（PLL 没起来，HSI 16 MHz、APB1 不分频 ⇒ PCLK1 8 MHz）。
     这是"降级运行"在桌面上的替身：所有速度会按同一比例错，而 [HB] 的 bf 只在
     启动失败时非 0 —— 这种情况以前**没有任何东西**能发现。 */
  fake_pclk1_hz = 8000000U;
  (void)AVE_Stepper_SelfTest(&st);
  CHECK_EQ(st.timer_clk_hz, 8000000, "T8.18 降级时钟 8 MHz 如实读出来");
  CHECK_EQ(st.expected_psc, 8,       "T8.19 8 MHz / 840 kHz = 9（截断）− 1 = 8");
  CHECK_TRUE((st.bad[AVE_STEPPER_AXIS_YAW] & AVE_STEPPER_CHK_PSC) != 0U,
             "T8.20 ★★ 降级时钟下 CHK_PSC 亮 —— 「时钟错了」第一次有了症状");

  /* 时钟低到时间基准以下：不许下溢成 0xFFFFFFFF */
  fake_pclk1_hz = 500000U;
  (void)AVE_Stepper_SelfTest(&st);
  CHECK_EQ(st.expected_psc, 0, "T8.21 ★ 时钟 < 时间基准时 expected_psc 归 0，不绕成 0xFFFFFFFF");

  /* ---- T8.e 只读、可重复：连调两次结果必须一致 ---- */
  sim_reset();
  sim_cubemx_config();
  AVE_Stepper_Init();
  {
    AVE_StepperSelfTest a;
    AVE_StepperSelfTest b;

    (void)AVE_Stepper_SelfTest(&a);
    (void)AVE_Stepper_SelfTest(&b);
    CHECK_EQ(memcmp(&a, &b, sizeof(a)), 0, "T8.22 ★ 连调两次结果逐字节相同（自检不写寄存器）");
    CHECK_EQ(fake_TIM5.CR1 & TIM_CR1_CEN, 0, "T8.23 自检没有把计数器打开");
    CHECK_EQ(AVE_Stepper_Stats()->stray_irq, 0, "T8.24 自检没有触发任何中断记账");
  }

  CHECK_EQ(AVE_Stepper_SelfTest(NULL), AVE_STEPPER_EBADARG, "T8.25 out = NULL → EBADARG（不崩）");
}

int main(void)
{
  printf("============================================================\n");
  printf(" AVE_stepper.c 桌面行为测试（定时器仿真，非硅片）\n");
  printf("============================================================\n");

  t1_init_is_static_and_fail_safe();
  t2_freq_math();
  t3_position_exactly_n();
  t4_same_direction_reprogram_keeps_pulsing();
  t5_stop_and_rejections();
  t6_direction_change_ordering();
  t7_stray_interrupt();
  t8_timer_selftest();

  printf("\n============================================================\n");
  printf(" 通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  printf("============================================================\n");
  return (g_fail == 0) ? 0 : 1;   /* 常规约定：0 = 全过，1 = 有失败 */
}
