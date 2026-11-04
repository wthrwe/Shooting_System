/**
 * 桌面侧【假】HAL —— 只提供 AVE_stepper.c / AVE_board.h 真正用到的类型与宏。
 *
 * 为什么能做这件事：`AVE_stepper.c` 是**寄存器级**的，它碰的全是 `CNT` / `ARR` /
 * `CCER` 这种普通内存映射位。把它们退化成**普通变量**，这个模块就能在桌面上跑真
 * 单元测试；反过来，如果它调的是 `HAL_TIM_PWM_Start()`，就什么都测不了。
 * （这是"直接写寄存器"除了可验证性之外的第二个好处。）
 *
 * ⚠️ 边界：这里仿的是**手册（RM0090）规定的行为**，不是硅片。
 *    仿真器里的定时器是 `test_stepper.c` 里手写的，它按我对手册的理解推进 CNT、
 *    产生比较事件。所以本测试能证明"模块的寄存器时序符合手册模型"，
 *    **不能**证明"芯片上真的这样"。后者只能上板（M2.5）。
 */
#ifndef FAKE_STM32F4XX_HAL_H
#define FAKE_STM32F4XX_HAL_H

#include <stdint.h>

/* ---- 定时器寄存器。字段名与顺序照抄真实类型，值都用不到（这里是普通内存）---- */
typedef struct
{
  volatile uint32_t CR1, CR2, SMCR, DIER, SR, EGR,
                    CCMR1, CCMR2, CCER, CNT, PSC, ARR, RCR,
                    CCR1, CCR2, CCR3, CCR4;
} TIM_TypeDef;

/* ---- GPIO。只需要 BSRR（模块写它来改 Dir），但照抄其余成员以免将来踩空 ---- */
typedef struct
{
  volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2];
} GPIO_TypeDef;

typedef struct { TIM_TypeDef *Instance; } TIM_HandleTypeDef;

extern TIM_TypeDef  fake_TIM2, fake_TIM5;
extern GPIO_TypeDef fake_GPIOA, fake_GPIOB;

#define TIM2  (&fake_TIM2)
#define TIM5  (&fake_TIM5)
#define GPIOA (&fake_GPIOA)
#define GPIOB (&fake_GPIOB)

/* ---- 引脚号：真实 CMSIS 里是 (1 << n) ---- */
#define GPIO_PIN_3  ((uint16_t)0x0008)
#define GPIO_PIN_4  ((uint16_t)0x0010)
#define GPIO_PIN_5  ((uint16_t)0x0020)
#define GPIO_PIN_10 ((uint16_t)0x0400)

/* ---- 通道号：真实 HAL 的取值就是 0/4/8/12 ---- */
#define TIM_CHANNEL_1  (0x00000000U)
#define TIM_CHANNEL_2  (0x00000004U)
#define TIM_CHANNEL_3  (0x00000008U)
#define TIM_CHANNEL_4  (0x0000000CU)

/* ---- 寄存器位。值抄 RM0090（名字已由 armcc 用真头文件验过）---- */
#define TIM_CR1_CEN       (0x0001U)   /* CR1  bit0  */
#define TIM_EGR_UG        (0x0001U)   /* EGR  bit0  */
#define TIM_SR_UIF        (0x0001U)   /* SR   bit0  */
#define TIM_IT_CC3        (0x0008U)   /* DIER bit3  */
#define TIM_IT_CC4        (0x0010U)   /* DIER bit4  */
#define TIM_CCER_CC3E     (0x0100U)   /* CCER bit8  */
#define TIM_CCER_CC4E     (0x1000U)   /* CCER bit12 */
#define TIM_CCMR2_OC3PE   (0x0008U)   /* CCMR2 bit3 */
#define TIM_CCMR2_OC4PE   (0x0800U)   /* CCMR2 bit11 */

/*
 * ---- 自检（AVE_Stepper_SelfTest）用到的位 ----
 * 🔴 值逐条取自真头文件 `stm32f407xx.h` 的 `*_Msk`（0x60 / 0x80 / 0x03 / 0x70 /
 *    0x300 / 0x7000 / 0x200 / 0x2000），**不是**照手册转抄的 —— 转抄错了这个
 *    测试会在错的地方变绿，而 armcc 那边验的只是"名字存在"，验不了值。
 */
#define TIM_CR1_CMS        (0x0060U)  /* CR1   bit6:5   非 0 = 中心对齐 */
#define TIM_CR1_ARPE       (0x0080U)  /* CR1   bit7     */
#define TIM_CCMR2_CC3S     (0x0003U)  /* CCMR2 bit1:0   */
#define TIM_CCMR2_OC3M     (0x0070U)  /* CCMR2 bit6:4   */
#define TIM_CCMR2_OC3M_Pos (4U)
#define TIM_CCMR2_CC4S     (0x0300U)  /* CCMR2 bit9:8   */
#define TIM_CCMR2_OC4M     (0x7000U)  /* CCMR2 bit14:12 */
#define TIM_CCMR2_OC4M_Pos (12U)
#define TIM_CCER_CC3P      (0x0200U)  /* CCER  bit9     */
#define TIM_CCER_CC4P      (0x2000U)  /* CCER  bit13    */

/*
 * ---- RCC：只够算 APB1 定时器时钟 ----
 *
 * `AVE_Stepper_SelfTest()` 拿 `RCC->CFGR.PPRE1` 判"APB1 分没分频"，
 * 再拿 `HAL_RCC_GetPCLK1Freq()` 的结果决定要不要乘 2（RM0090 §6.2 时钟树）。
 * 桌面上这两个都退化成普通变量/普通函数。
 */
typedef struct { volatile uint32_t CFGR; } RCC_TypeDef;

extern RCC_TypeDef fake_RCC;
extern uint32_t    fake_pclk1_hz;   /* 测试里设成 42000000（168 MHz / 4） */

#define RCC                  (&fake_RCC)
#define RCC_CFGR_PPRE1       (0x1C00U)   /* 真值取自 stm32f407xx.h:9589 */
#define RCC_CFGR_PPRE1_Pos   (10U)       /* 真值取自 stm32f407xx.h:9588 */

/** 真件里返回 APB1 的时钟频率；这里返回 `fake_pclk1_hz`。 */
uint32_t HAL_RCC_GetPCLK1Freq(void);

/*
 * ---- DWT 周期计数器：带"每次访问自增"的仿真钩子 ----
 *
 * 换向等待 `stepper_delay_us()` 靠轮询 `DWT->CYCCNT` 计时间。在桌面上如果
 * CYCCNT 恒为 0，那个 while 会**永远转下去**（测试直接挂死），
 * 或者退化成 `s_dwt_ok = 0` 走空转退路 —— 于是 DWT 这条路**一行都测不到**。
 *
 * 所以把 `DWT` 定义成一次**函数调用**：每访问一次 CYCCNT，它就 +1。
 * 于是 "读两次看它有没有动" 的自检会通过，而忙等循环也会在 1680 次访问后退出。
 * 这样两条路都能被验证到（`dwt_fallbacks` 应为 0 = 走的是 DWT 路）。
 */
typedef struct { volatile uint32_t CTRL, CYCCNT; } fake_DWT_Type;
typedef struct { volatile uint32_t DHCSR, DCRSR, DCRDR, DEMCR; } fake_CoreDebug_Type;

extern fake_DWT_Type        fake_dwt;
extern fake_CoreDebug_Type  fake_coredebug;
extern uint32_t             fake_dwt_access_count;

/** 每访问一次 DWT，就把 CYCCNT 推进 1 —— 纯测试仿真，别照搬到目标代码里。 */
fake_DWT_Type *fake_dwt_touch(void);

#define DWT       (fake_dwt_touch())
#define CoreDebug (&fake_coredebug)

#define CoreDebug_DEMCR_TRCENA_Msk (0x01000000U)
#define DWT_CTRL_CYCCNTENA_Msk     (0x00000001U)

extern uint32_t SystemCoreClock;   /* 测试里设成 168000000 */

/*
 * HAL 的定时器 PWM 回调。
 * 真件里：声明在 `stm32f4xx_hal_tim.h`，**空弱定义**在 `stm32f4xx_hal_tim.c`
 *        （`AVE.map:3553` 显示它 2 字节），由 `HAL_TIM_IRQHandler` 调用（`AVE.map:176`）。
 * 本工程里：定义改由 `AVE_stepper.c` 提供（全工程唯一一份）。
 * 桌面上没有 HAL 库，所以这个声明由假头补上；仿真器直接调它来触发比较中断。
 */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim);

#endif /* FAKE_STM32F4XX_HAL_H */
