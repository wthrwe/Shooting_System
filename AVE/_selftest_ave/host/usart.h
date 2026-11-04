/**
 * `usart.h` 替身 —— 只提供 AVE_board.h 引用的三个句柄。
 *
 * ⚠️ 真实工程里 `huart1/2/3` 的定义在 CubeMX 生成的 `usart.c`。
 *    这里由 fake_hal.c 定义，供产品代码链接。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#ifndef HOST_SHIM_USART_H
#define HOST_SHIM_USART_H

#include "stm32f4xx_hal.h"

extern UART_HandleTypeDef huart1;   /* USART1 —— 视觉 MaixCAM */
extern UART_HandleTypeDef huart2;   /* USART2 —— 电机总线 */
extern UART_HandleTypeDef huart3;   /* USART3 —— 调试口 / ESP8266 */

void MX_USART1_UART_Init(void);
void MX_USART2_UART_Init(void);
void MX_USART3_UART_Init(void);

#endif /* HOST_SHIM_USART_H */
