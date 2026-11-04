/* 替身：真实 AVE_config.h 会牵进 AVE_tuning.h/AVE_board.h 一大串 HAL 依赖，
   本测试只关心计数逻辑，故用等价替身，只提供 AVE_stepper.c 真正用到的宏。 */
#ifndef STUB_AVE_CONFIG_H
#define STUB_AVE_CONFIG_H
#include "stm32f4xx_hal.h"
#include "AVE_tuning.h"
#include "AVE_board.h"
#endif
