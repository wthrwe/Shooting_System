

#ifndef AVE_CONFIG_H
#define AVE_CONFIG_H

/* 调参入口（功能开关 / PID / 限速 / 地址 / 机械换算 / 时序） */
#include "AVE_tuning.h"

/* 板级定义（引脚 / 外设实例 / 激光极性） */
#include "AVE_board.h"

/* 凭据文件仅开启云端时才需要。默认 AVE_ENABLE_CLOUD = 0，
   没有 AVE_secrets.h 也能编译（明文凭据，禁止外传，不进版本库） */
#if AVE_ENABLE_CLOUD
  #include "AVE_secrets.h"
#endif

#endif /* AVE_CONFIG_H */
