/**
 * M1.2 自检 —— 板级定义。
 *
 * 重点在**激光电平映射**：这是安全攸关的（写反了上电就出光），
 * 却又最难在桌面上发现（现象只是"激光一直亮着"，看不出是代码还是接线问题）。
 *
 * 本文件对**两种极性都成立** —— 所以它能在 ACTIVE_HIGH=1 和 =0 下各跑一次，
 * 两次都通过才说明映射在两种配置下都正确。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "AVE_config.h"

#define CHK(name, cond) typedef char chk_##name[(cond) ? 1 : -1]

/* ── 激光电平映射：关必须是不出光的那个电平 ─────────────────────────────── */
#if AVE_LASER_ACTIVE_HIGH
  CHK(off_is_low,  AVE_LASER_LEVEL(0) == GPIO_PIN_RESET);
  CHK(on_is_high,  AVE_LASER_LEVEL(1) == GPIO_PIN_SET);
#else
  CHK(off_is_high, AVE_LASER_LEVEL(0) == GPIO_PIN_SET);
  CHK(on_is_low,   AVE_LASER_LEVEL(1) == GPIO_PIN_RESET);
#endif

/* ── 极性宏本身必须是 0 或 1（写成别的值会让上面两个分支都无法正确覆盖）── */
CHK(polarity_binary, (AVE_LASER_ACTIVE_HIGH == 0) || (AVE_LASER_ACTIVE_HIGH == 1));

/* ── 引脚映射：让每个宏都被真正用到，未定义的会立刻报错 ─────────────────── */
int ave_m12_selftest(void)
{
  /* 三路串口实例 */
  USART_TypeDef *insts[3];
  insts[0] = AVE_CAM_UART_INSTANCE;
  insts[1] = AVE_MOTOR_UART_INSTANCE;
  insts[2] = AVE_ESP_UART_INSTANCE;

  /* 三路串口句柄（业务侧用的逻辑名） */
  UART_HandleTypeDef *hands[3];
  hands[0] = AVE_HUART_CAM;
  hands[1] = AVE_HUART_MOTOR;
  hands[2] = AVE_HUART_ESP;

  /* 引脚 */
  GPIO_TypeDef *ports[7];
  ports[0] = AVE_CAM_UART_TX_PORT;
  ports[1] = AVE_CAM_UART_RX_PORT;
  ports[2] = AVE_MOTOR_UART_TX_PORT;
  ports[3] = AVE_MOTOR_UART_RX_PORT;
  ports[4] = AVE_ESP_UART_TX_PORT;
  ports[5] = AVE_ESP_UART_RX_PORT;
  ports[6] = AVE_LASER_GPIO_PORT;

  uint16_t pins[7];
  pins[0] = AVE_CAM_UART_TX_PIN;
  pins[1] = AVE_CAM_UART_RX_PIN;
  pins[2] = AVE_MOTOR_UART_TX_PIN;
  pins[3] = AVE_MOTOR_UART_RX_PIN;
  pins[4] = AVE_ESP_UART_TX_PIN;
  pins[5] = AVE_ESP_UART_RX_PIN;
  pins[6] = AVE_LASER_GPIO_PIN;

  /* 复用功能号 */
  uint8_t afs[3];
  afs[0] = AVE_CAM_UART_AF;
  afs[1] = AVE_MOTOR_UART_AF;
  afs[2] = AVE_ESP_UART_AF;

  /* 把数组用掉，避免"声明未使用"的警告掩盖真实错误 */
  int total = 0;
  total += (insts[0] != 0) + (hands[0] != 0) + (ports[0] != 0);
  total += (int)pins[0] + (int)afs[0];
  total += (int)pins[6];
  return total;
}
