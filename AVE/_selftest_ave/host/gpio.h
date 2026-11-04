/**
 * `gpio.h` 替身 —— AVE_board.h 只引它拿 `GPIO_PIN_*` 等宏（已在 stm32f4xx_hal.h 里）。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#ifndef HOST_SHIM_GPIO_H
#define HOST_SHIM_GPIO_H

#include "stm32f4xx_hal.h"

void MX_GPIO_Init(void);

#endif /* HOST_SHIM_GPIO_H */
