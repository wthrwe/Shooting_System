/* 替身。真值 AVE_board.h:35/64/66 —— 两轴 DIR 都在 GPIOA。 */
#ifndef STUB_AVE_BOARD_H
#define STUB_AVE_BOARD_H
#include "stm32f4xx_hal.h"
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim5;
#define AVE_PAN_DIR_PORT      GPIOA
#define AVE_PAN_DIR_PIN       (1U << 4)      /* PA4 */
#define AVE_TILT_DIR_PORT     GPIOA
#define AVE_TILT_DIR_PIN      (1U << 5)      /* PA5 */
#define AVE_HTIM_PAN_STEP     (&htim2)
#define AVE_HTIM_TILT_STEP    (&htim5)
#define AVE_STEP_PAN_CHANNEL  TIM_CHANNEL_3
#define AVE_STEP_TILT_CHANNEL TIM_CHANNEL_4
#endif
