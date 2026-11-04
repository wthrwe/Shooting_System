/* AVE_board.c —— 板级实现：激光安全电平 + 启动故障记录 */

#include "AVE_config.h"

/* ===== 私有状态 ===== */

/* 最近一次设定的激光电平，按极性宏置关，初始为关 */
static GPIO_PinState s_laser_level = AVE_LASER_LEVEL(0);

/* 启动故障：第一个故障码 + 发生次数，由 Error_Handler() 写入 */
static volatile uint32_t s_boot_fail_code  = (uint32_t)AVE_BOARD_BOOT_FAIL_NONE; //上电后第一个故障根因
static volatile uint32_t s_boot_fail_count = 0U;  //故障总发生次数

/* 复位原因缓存。0xFFFFFFFF 表示"还没读过" */
static uint32_t s_reset_cause = 0xFFFFFFFFu;

/* ===== 对外接口 ===== */

 /**
  * 函    数：记录一个启动故障码
  * 参    数：code 故障码
  * 返 回 值：无
  * 注意事项：0 表示"无故障"，拿来当参数是笔误，直接挡掉——否则真码再也进不来。
  *           先到先得：只留第一个根因码，后续调用只累加计数。
  */
void AVE_Board_NoteBootFail(uint32_t code)
{
  /* 0：它表示无故障 */
  if (code == (uint32_t)AVE_BOARD_BOOT_FAIL_NONE)  //强制类型转换
  {
    return;
  }

  /* 先到先得，只保存第一个故障根因，后面故障只计数，不覆盖根因 */
  if (s_boot_fail_code == (uint32_t)AVE_BOARD_BOOT_FAIL_NONE)
  {
    s_boot_fail_code = code;
  }
  s_boot_fail_count++;
}

/**
  * 函    数：取第一个启动故障码
  * 参    数：无
  * 返 回 值：故障码，无故障时返回 AVE_BOARD_BOOT_FAIL_NONE
  */
uint32_t AVE_Board_BootFailCode(void)
{
  return s_boot_fail_code;
}

/**
  * 函    数：取启动故障发生次数
  * 参    数：无
  * 返 回 值：发生次数，同一个码记多次也会累加
  */
uint32_t AVE_Board_BootFailCount(void)
{
  return s_boot_fail_count;
}

/**
  * 函    数：读本次启动的复位原因
  * 参    数：无
  * 返 回 值：复位原因位掩码，可同时置多位；全没置位返回 0
  * 注意事项：内置缓存、幂等，只真正读一次硬件便清标志；须读完所有位再清，RMVF 是全清开关。
  */
uint32_t AVE_Board_ResetCause(void)
{
  uint32_t v = 0u;

  /* 只真正读一次硬件，之后走缓存 */
  if (s_reset_cause != 0xFFFFFFFFu)
  {
    return s_reset_cause;
  }

  /* 各标志可同时置位，须逐位判断，不能 else if */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST)  != RESET) { v |= (uint32_t)AVE_RST_POR;  }
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST)  != RESET) { v |= (uint32_t)AVE_RST_BOR;  }
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST)  != RESET) { v |= (uint32_t)AVE_RST_PIN;  }
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)  != RESET) { v |= (uint32_t)AVE_RST_SOFT; }
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) { v |= (uint32_t)AVE_RST_IWDG; }
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != RESET) { v |= (uint32_t)AVE_RST_WWDG; }
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST) != RESET) { v |= (uint32_t)AVE_RST_LPWR; }

  /* 清标志须在读完全部位的位之后：RMVF 是清全部的单一开关 */
  __HAL_RCC_CLEAR_RESET_FLAGS();

  s_reset_cause = v;
  return v;
}

//板级初始化
void AVE_Board_Init(void)
{
  /* 依据极性宏强制把激光置关，纠正 CubeMX 那句写死的 RESET */
  AVE_Laser_Set(0);   //强制关闭激光

  /* 串口接收启动故意不在这里做：HAL 全局回调归属 AVE_uart.c */
}

//激光开关函数
void AVE_Laser_Set(uint8_t on)
{
  uint8_t want = on;

  /* 启动故障时强制置关：上层想点不代表这台机器有资格出光 */
  if (AVE_Board_BootFailCode() != (uint32_t)AVE_BOARD_BOOT_FAIL_NONE)
  {
    want = 0U;
  }

  s_laser_level = AVE_LASER_LEVEL(want);
  HAL_GPIO_WritePin(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN, s_laser_level);
}


