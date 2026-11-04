/**
 * `main.h` 替身。CubeMX 生成的那个 main.h 主要是 `#include "stm32f4xx_hal.h"`
 * 加上引脚别名宏；本工程的产品代码**只用到后者中的零个** ——
 * 引脚全部由 AVE_board.h 自己命名（见那里的说明）。
 * 所以这里只需要把 HAL 头引进来即可。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#ifndef HOST_SHIM_MAIN_H
#define HOST_SHIM_MAIN_H

#include "stm32f4xx_hal.h"

#endif /* HOST_SHIM_MAIN_H */
