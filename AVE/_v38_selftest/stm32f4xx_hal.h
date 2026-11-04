/* 替身头（不是 ST 原件）：只为让真实的 AVE_stepper.c 能在宿主 gcc 上编过。
   寄存器字段名与位定义按 STM32F4 参考手册抄，值本身对本测试无影响。 */
#ifndef STUB_STM32F4XX_HAL_H
#define STUB_STM32F4XX_HAL_H
#include <stdint.h>
#define __IO volatile
typedef struct { __IO uint32_t CR1,CR2,SMCR,DIER,SR,EGR,CCMR1,CCMR2,CCER,CNT,PSC,ARR,
                            RCR,CCR1,CCR2,CCR3,CCR4,BDTR,DCR,DMAR,OR; } TIM_TypeDef;
typedef struct { __IO uint32_t MODER,OTYPER,OSPEEDR,PUPDR,IDR,ODR,BSRR,LCKR,AFR[2]; } GPIO_TypeDef;
typedef struct { TIM_TypeDef *Instance; } TIM_HandleTypeDef;
typedef struct { __IO uint32_t CTRL, CYCCNT; } DWT_TypeDef;
extern DWT_TypeDef stub_dwt;
#define DWT (&stub_dwt)
#define DWT_CTRL_CYCCNTENA_Msk (1UL << 0)
extern GPIO_TypeDef stub_gpioa;
#define GPIOA (&stub_gpioa)
extern GPIO_TypeDef stub_gpiob;
#define GPIOB (&stub_gpiob)
#define TIM_CR1_CEN        (1UL << 0)
#define TIM_CR1_CMS        (3UL << 5)
#define TIM_CR1_ARPE       (1UL << 7)
#define TIM_SR_UIF         (1UL << 0)
#define TIM_EGR_UG         (1UL << 0)
#define TIM_IT_CC3         (1UL << 3)
#define TIM_IT_CC4         (1UL << 4)
#define TIM_CCER_CC3E      (1UL << 8)
#define TIM_CCER_CC3P      (1UL << 9)
#define TIM_CCER_CC4E      (1UL << 12)
#define TIM_CCER_CC4P      (1UL << 13)
#define TIM_CCMR2_CC3S     (3UL << 0)
#define TIM_CCMR2_OC3PE    (1UL << 3)
#define TIM_CCMR2_OC3M     (7UL << 4)
#define TIM_CCMR2_OC3M_Pos 4U
#define TIM_CCMR2_CC4S     (3UL << 8)
#define TIM_CCMR2_OC4PE    (1UL << 11)
#define TIM_CCMR2_OC4M     (7UL << 12)
#define TIM_CCMR2_OC4M_Pos 12U
#define TIM_CHANNEL_1      0U
#define TIM_CHANNEL_2      4U
#define TIM_CHANNEL_3      8U
#define TIM_CHANNEL_4      12U
#define TIM_OCMODE_PWM1    6U
#define TIM_OCPOLARITY_LOW 1U
typedef struct { __IO uint32_t DHCSR, DCRSR, DCRDR, DEMCR; } CoreDebug_TypeDef;
extern CoreDebug_TypeDef stub_coredebug;
#define CoreDebug (&stub_coredebug)
#define CoreDebug_DEMCR_TRCENA_Msk (1UL << 24)
typedef struct { __IO uint32_t CR, PLLCFGR, CFGR, CIR; } RCC_TypeDef;
extern RCC_TypeDef stub_rcc;
#define RCC (&stub_rcc)
#define RCC_CFGR_PPRE1      (7UL << 10)
#define RCC_CFGR_PPRE1_Pos  10U
extern uint32_t SystemCoreClock;
uint32_t HAL_RCC_GetPCLK1Freq(void);
#endif
