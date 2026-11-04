/*
 * 桌面侧假 `tim.h` —— 供 M1.3 / M1.5 / M2.4 三个套件使用。
 *
 * 产品代码 `AVE_board.h:29` 引它，只为拿 `htim2` / `htim5` 的**声明**（第 25 次新增）。
 * 这三个套件**不驱动任何定时器**，所以：
 *   · 只声明、**不定义** —— 只要没有代码取它们的地址，链接期就不会缺符号；
 *   · 若将来有人在这里取地址，症状是**链接失败** `undefined reference to htim2`，
 *     那时补两个哑句柄即可。**是响亮失败，不是静默错。**
 *
 * 🔴 步进仿真用的是**另一套** `host/fake/tim.h`（那里的句柄由 `test_stepper.c` 定义，
 *    `Instance` 指向仿真器里的假寄存器）。两套并存是**有意的**，别合并 ——
 *    合并会把仿真器依赖拖进这三个不需要它的套件。
 *
 * ⚠️ 本文件必须排在 `-I` 里**工程真头文件之前**，靠此遮蔽 `Core/Inc/tim.h`。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */
#ifndef HOST_SHIM_TIM_H
#define HOST_SHIM_TIM_H

/* 最小定时器句柄：只需要类型能过编译，字段一个都不用。 */
typedef struct { int unused; } TIM_HandleTypeDef;

extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim5;

#endif /* HOST_SHIM_TIM_H */
