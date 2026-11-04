/*
 * 桌面侧假 tim.h —— `AVE_board.h` 引它来拿 `htim2` / `htim5` 的**声明**。
 * 真件里它们由 `Core/Src/tim.c` 定义；桌面上由 `test_stepper.c` 定义，
 * 这样句柄的 `Instance` 就指向仿真器里的那两块假寄存器。
 */
#ifndef FAKE_TIM_H
#define FAKE_TIM_H

#include "main.h"

extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim5;

#endif
