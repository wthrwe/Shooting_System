#ifndef AVE_FAKE_STEPPER_H
#define AVE_FAKE_STEPPER_H

#include <stdint.h>
#include "AVE_stepper.h"

/* ============================================================================
 * fake_stepper.h —— 云台层宿主测试用的【假后端】
 *
 * 【为什么需要它】
 *   AVE_gimbal.c 是"薄壳"，它自己不产生波形，只做两件事：
 *     ① 角度/转速 换算成 脉冲/频率；② 把指令转发给 AVE_Stepper_*。
 *   要测这两件事，就必须把 AVE_Stepper_* 换成可摆布的东西 —— 否则测不了
 *   "后端说忙时薄壳怎么办"这类分叉。
 *
 * 【它不是被测物，但它是"合同"】
 *   本文件【刻意复刻】AVE_stepper.c 的可观测行为，每条都标了源码行号。
 *   它错了，测试就等于在测一个不存在的后端 —— 所以改它必须回 AVE_stepper.c 对账。
 *   🔴 但被测的【期望值】一律手算成字面数字写死在 test_gimbal.c 里，
 *      绝不在测试里重算 rpm*6*steps_per_deg —— 那是第 29 次"桌面全绿、硅片对不上"
 *      的根因（基线和被测物共用了同一个反向模型）。
 *
 * 【怎么用】
 *   fake_reset() 之后直接摆字段，再调云台层函数：
 *      g_fake.busy[0]  = 1;        // 假装水平轴正在走定位
 *      g_fake.gate[0]  = 0;        // 假装水平轴门控关着
 *      g_fake.rc_movepulses[0] = AVE_STEPPER_EGATE;   // 强制返回某个码
 *   然后看 last_hz / last_pulses / calls_* 有没有变化。
 * ==========================================================================*/

typedef struct
{
  /* ---- 输入：测试摆这些，决定假后端怎么应答 ---- */
  int      gate[AVE_STEPPER_AXIS_NUM];   /* 0=门关 1=门开 */
  int      busy[AVE_STEPPER_AXIS_NUM];   /* 0=空闲 1=正在走定位 */
  int      rc_movepulses[AVE_STEPPER_AXIS_NUM]; /* 0=按真实逻辑判；非 0=强制返回该码 */
  int      rc_setfreq[AVE_STEPPER_AXIS_NUM];    /* 同上 */

  /* ---- 输出：测试读这些，看薄壳到底发了什么 ---- */
  int32_t  last_hz[AVE_STEPPER_AXIS_NUM];      /* 最近一次 SetFreqHz 的频率 */
  int32_t  last_pulses[AVE_STEPPER_AXIS_NUM];  /* 最近一次 MovePulses 的【带符号】脉冲 */
  uint32_t last_mag[AVE_STEPPER_AXIS_NUM];     /* 最近一次 MovePulses 的 hz 形参 */

  /* ---- 调用计数：证明"某函数被调了没有"，是回归测试（用例 20）的判据 ---- */
  uint32_t calls_setfreq[AVE_STEPPER_AXIS_NUM];
  uint32_t calls_movepulses[AVE_STEPPER_AXIS_NUM];
  uint32_t calls_stop[AVE_STEPPER_AXIS_NUM];
  uint32_t calls_setgate[AVE_STEPPER_AXIS_NUM];
  uint32_t calls_isbusy[AVE_STEPPER_AXIS_NUM];
} FakeStepper;

extern FakeStepper g_fake;

/** 全部清零 + 两轴门控置【开】。默认开门是为了让"门控"只在专门用例里当变量，
 *  否则每个用例都得先开一次门，噪音太大。 */
void fake_reset(void);

#endif /* AVE_FAKE_STEPPER_H */
