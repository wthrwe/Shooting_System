/* ============================================================================
 * 第 37 次桌面自测：脉冲计数 pulses_total 到底数不数得对
 * ----------------------------------------------------------------------------
 * 被测物是**真实的** AVE/AVE_stepper.c（从工程里原样拷来，一字未改）；
 * 它引用的 AVE_tuning.h / AVE_board.h / stm32f4xx_hal.h 用等价替身（见同目录，
 * 数值都标了在真文件里的行号）。所以本测试证明的是**计数逻辑与分支顺序**，
 * **不是**定时器配置对不对 —— 后者靠上板 [TIM] bad=0,0。
 *
 * 为什么要手喂中断而不是真跑定时器：宿主上没有 TIM2/TIM5 的 CNT 在涨，
 * `AVE_Stepper_PulseFinishedIsr` 的语义是"一个 STEP 上升沿 = 一次调用"，
 * 手喂就是直接构造这个前提。硬件上"CNT 到 CCR 会不会真产生一次 CC 中断"
 * 是另一件事，由上板 [JOG] 覆盖。
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include "AVE_stepper.h"

/* AVE_board.h 里 extern 的那两个句柄 + 替身外设 */
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim5;
static TIM_TypeDef s_tim2;
static TIM_TypeDef s_tim5;
GPIO_TypeDef stub_gpioa;
GPIO_TypeDef stub_gpiob;
DWT_TypeDef  stub_dwt;
CoreDebug_TypeDef stub_coredebug;
RCC_TypeDef  stub_rcc;
uint32_t SystemCoreClock = 168000000UL;

uint32_t HAL_RCC_GetPCLK1Freq(void) { return 84000000UL; }

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                                  \
  do {                                                                         \
    if (cond) { g_pass++; printf("  [ok  ] " fmt "\n", ##__VA_ARGS__); }        \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); }        \
  } while (0)

/* 一次"喂 N 个 STEP 上升沿" */
static void feed(TIM_HandleTypeDef *h, int n)
{
  int i;
  for (i = 0; i < n; i++) { AVE_Stepper_PulseFinishedIsr(h); }
}

static const AVE_StepperStats *ST(void) { return AVE_Stepper_Stats(); }

int main(void)
{
  uint32_t p0, p1;
  int rc;

  htim2.Instance = &s_tim2;
  htim5.Instance = &s_tim5;

  AVE_Stepper_Init();

  printf("== T1 上电初值：任何动作之前，两轴计数必须是 0 ==\n");
  CHECK(ST()->pulses_total[AVE_STEPPER_AXIS_PAN] == 0, "PAN  p=%lu",
        (unsigned long)ST()->pulses_total[AVE_STEPPER_AXIS_PAN]);
  CHECK(ST()->pulses_total[AVE_STEPPER_AXIS_TILT] == 0, "TILT p=%lu",
        (unsigned long)ST()->pulses_total[AVE_STEPPER_AXIS_TILT]);

  printf("== T2 一次 1 脉冲定位 —— 与上板 AVE_DIAG_JOG_PULSES=1 同一档 ==\n");
  rc = AVE_Stepper_SetGate(AVE_STEPPER_AXIS_PAN, 1U);
  CHECK(rc == AVE_STEPPER_EOK, "开 PAN 门控 rc=%d（0=EOK）", rc);
  /* ⚠️ 上板时门控默认是关的，是 app_boot_jog() 里显式开的；这里照做 */
  p0 = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
  rc = AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_PAN, 1, 200U);
  CHECK(rc == AVE_STEPPER_EOK, "MovePulses(1,200Hz) rc=%d", rc);
  feed(&htim2, 1);
  p1 = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
  CHECK(p1 - p0 == 1U, "喂 1 个边沿 ⇒ 计数 +%lu（期望 +1）", (unsigned long)(p1 - p0));
  CHECK(ST()->move_done == 1U, "move_done=%lu（期望 1，说明这一拍真的走完了）",
        (unsigned long)ST()->move_done);
  CHECK(AVE_Stepper_IsBusy(AVE_STEPPER_AXIS_PAN) == 0,
        "走完就该不忙（上板 [JOG] 的 b=0 同一个判据）");

  printf("== T3 一次 5 脉冲定位 ⇒ 累计再 +5 ==\n");
  p0 = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
  rc = AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_PAN, 5, 1000U);
  CHECK(rc == AVE_STEPPER_EOK, "MovePulses(5,1000Hz) rc=%d", rc);
  feed(&htim2, 5);
  p1 = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
  CHECK(p1 - p0 == 5U, "喂 5 个边沿 ⇒ 计数 +%lu（期望 +5）", (unsigned long)(p1 - p0));

  printf("== T4 【关键】走完之后的残留/速度模式边沿也必须计 ==\n");
  /* 这 3 次调用进的是 ISR 里 `!pos_active` 那个分支。计数若写在那个 return **之后**，
     这 3 个就全丢 —— 本测试就是用来钉死"计数必须在判断之前"的。
     方向性理由：漏计会让人以为"一个脉冲都没发"，多计不会。 */
  p0 = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
  feed(&htim2, 3);
  p1 = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
  CHECK(p1 - p0 == 3U, "pos_active==0 时喂 3 个 ⇒ 计数 +%lu（期望 +3）",
        (unsigned long)(p1 - p0));

  printf("== T5 轴隔离：喂 TILT 不能动 PAN ==\n");
  {
    uint32_t pan_a = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
    uint32_t tilt_a = ST()->pulses_total[AVE_STEPPER_AXIS_TILT];
    rc = AVE_Stepper_SetGate(AVE_STEPPER_AXIS_TILT, 1U);
    CHECK(rc == AVE_STEPPER_EOK, "开 TILT 门控 rc=%d", rc);
    rc = AVE_Stepper_MovePulses(AVE_STEPPER_AXIS_TILT, 7, 1000U);
    CHECK(rc == AVE_STEPPER_EOK, "MovePulses(TILT,7) rc=%d", rc);
    feed(&htim5, 7);
    CHECK(ST()->pulses_total[AVE_STEPPER_AXIS_TILT] - tilt_a == 7U,
          "TILT 计数 +%lu（期望 +7）",
          (unsigned long)(ST()->pulses_total[AVE_STEPPER_AXIS_TILT] - tilt_a));
    CHECK(ST()->pulses_total[AVE_STEPPER_AXIS_PAN] == pan_a,
          "PAN 纹丝不动（期望不变，仍是 %lu）", (unsigned long)pan_a);
  }

  printf("== T6 认不出的 htim：只记 stray_irq，不计数、不崩 ==\n");
  {
    TIM_HandleTypeDef alien;
    static TIM_TypeDef s_tim9;      /* 第三条定时器，不在 s_cfg 表里 */
    uint32_t pan_a = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
    uint32_t stray_a = ST()->stray_irq;
    memset(&alien, 0, sizeof(alien));
    /* ⚠️ 必须换 Instance：axis_of_htim() 是按 **Instance 指针**认轴的（不是按句柄
       地址），所以只把句柄换个名字没用 —— 一开始就是这么写错的，T6 报了 FAIL。
       顺带证明了一件事：HAL 回调里传进来的句柄即使与 s_cfg 里那个不是同一个
       结构体，只要 Instance 一样就能认对轴。 */
    alien.Instance = &s_tim9;
    feed(&alien, 2);
    CHECK(ST()->pulses_total[AVE_STEPPER_AXIS_PAN] == pan_a,
          "不认识的中断不动计数（期望不变 %lu）", (unsigned long)pan_a);
    CHECK(ST()->stray_irq - stray_a == 2U, "stray_irq +%lu（期望 +2）",
          (unsigned long)(ST()->stray_irq - stray_a));
  }

  printf("== T7 反证：GetCounter() 读不出脉冲数（第 36 次那个 clk 为什么天生恒 0）==\n");
  {
    uint32_t cnt = AVE_Stepper_GetCounter(AVE_STEPPER_AXIS_PAN);
    uint32_t p   = ST()->pulses_total[AVE_STEPPER_AXIS_PAN];
    printf("  PAN: CNT=%lu  pulses_total=%lu\n",
           (unsigned long)cnt, (unsigned long)p);
    CHECK(p > 0U, "本轴确实发过脉冲（pulses_total=%lu）", (unsigned long)p);
    CHECK(cnt == 0U,
          "同一次定位之后 CNT 却是 0 ⇒ 拿它当验收判据是空的（第 36 次的缺陷复现）");
  }

  printf("\n---- 汇总：%d 通过 / %d 失败 ----\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
