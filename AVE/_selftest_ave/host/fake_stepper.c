/* ============================================================================
 * fake_stepper.c —— 假后端实现。每条分支都对照 AVE_stepper.c 抄，行号写在旁边。
 *
 * 🔴 最要命的一条是 SetFreqHz 里 hz==0 的【位置】：
 *    真后端在 AVE_stepper.c:408 把 `hz == 0 → Stop(axis)` 放在忙碌检查【之前】。
 *    意思是：只要有人传 0 Hz 进来，哪怕这条轴正在走定位，也会被打断。
 *    本文件照抄了这个次序 —— 正因为照抄了，用例 20 才有意义：
 *    它断言薄壳在忙时【根本没调用】SetFreqHz，从而证明薄壳自己挡住了，
 *    而不是"运气好后端没打断"。
 * ==========================================================================*/

#include "fake_stepper.h"
#include <string.h>

FakeStepper g_fake;

void fake_reset(void)
{
  int i;

  memset(&g_fake, 0, sizeof(g_fake));
  for (i = 0; i < (int)AVE_STEPPER_AXIS_NUM; i++)
  {
    g_fake.gate[i] = 1;   /* 默认开门，见头文件说明 */
  }
}

/* 轴号非法时返回 -1：真后端也是这个路子（axis_valid 失败即回 EBADARG）。 */
static int ax_ok(AVE_StepperAxis axis)
{
  return ((int)axis >= 0) && ((int)axis < (int)AVE_STEPPER_AXIS_NUM);
}

static int hz_in_range(int32_t hz)
{
  uint32_t m = (uint32_t)((hz < 0) ? -hz : hz);
  if (hz == 0)
  {
    return 0;   /* 0 由调用方单独处理（真后端：0 = 停，AVE_stepper.c:408） */
  }
  return (m >= (uint32_t)AVE_STEPPER_MIN_HZ) && (m <= (uint32_t)AVE_STEPPER_MAX_HZ);
}

int AVE_Stepper_Stop(AVE_StepperAxis axis)
{
  if (!ax_ok(axis))
  {
    return AVE_STEPPER_EBADARG;
  }
  g_fake.calls_stop[axis]++;
  /* 真后端 AVE_stepper.c:548 先清 pos_active，再停 PWM —— 也就是"停"就是"空闲"。 */
  g_fake.busy[axis] = 0;
  return AVE_STEPPER_EOK;
}

int AVE_Stepper_IsGateOpen(AVE_StepperAxis axis)
{
  if (!ax_ok(axis))
  {
    return 0;
  }
  return g_fake.gate[axis] ? 1 : 0;
}

int AVE_Stepper_SetGate(AVE_StepperAxis axis, uint8_t on)
{
  if (!ax_ok(axis) || (on > 1U))
  {
    return AVE_STEPPER_EBADARG;
  }
  g_fake.calls_setgate[axis]++;
  /* 真后端 AVE_stepper.c:365：关门时先 Stop(axis) 再落门控位。 */
  if (on == 0U)
  {
    (void)AVE_Stepper_Stop(axis);
  }
  g_fake.gate[axis] = on ? 1 : 0;
  return AVE_STEPPER_EOK;
}

int AVE_Stepper_IsBusy(AVE_StepperAxis axis)
{
  if (!ax_ok(axis))
  {
    return 0;
  }
  g_fake.calls_isbusy[axis]++;
  return g_fake.busy[axis] ? 1 : 0;
}

int AVE_Stepper_SetFreqHz(AVE_StepperAxis axis, int32_t hz)
{
  if (!ax_ok(axis))
  {
    return AVE_STEPPER_EBADARG;
  }
  g_fake.calls_setfreq[axis]++;

  if (g_fake.rc_setfreq[axis] != 0)
  {
    return g_fake.rc_setfreq[axis];   /* 注入用：强制某个返回码 */
  }

  /* ① 真后端 AVE_stepper.c:408 —— 0 Hz 就是"停"，而且这一条排在忙碌检查【之前】。 */
  if (hz == 0)
  {
    return AVE_Stepper_Stop(axis);
  }
  /* ② AVE_stepper.c:410 */
  if (g_fake.busy[axis] != 0)
  {
    return AVE_STEPPER_EBUSY;
  }
  /* ③ AVE_stepper.c:411 */
  if (g_fake.gate[axis] == 0)
  {
    return AVE_STEPPER_EGATE;
  }
  /* ④ 频率量值范围（真后端按 [MIN_HZ, MAX_HZ] 判） */
  if (!hz_in_range(hz))
  {
    return AVE_STEPPER_ERANGE;
  }

  g_fake.last_hz[axis] = hz;
  return AVE_STEPPER_EOK;
}

int AVE_Stepper_MovePulses(AVE_StepperAxis axis, int32_t pulses, uint32_t hz)
{
  uint32_t mag;

  if (!ax_ok(axis))
  {
    return AVE_STEPPER_EBADARG;
  }
  g_fake.calls_movepulses[axis]++;

  if (g_fake.rc_movepulses[axis] != 0)
  {
    return g_fake.rc_movepulses[axis];
  }

  /* ① 真后端 AVE_stepper.c:488 —— 0 脉冲是合法的空动作，直接 EOK，【且不置忙碌】。 */
  if (pulses == 0)
  {
    return AVE_STEPPER_EOK;
  }
  /* ② AVE_stepper.c:490 */
  if (g_fake.busy[axis] != 0)
  {
    return AVE_STEPPER_EBUSY;
  }
  /* ③ AVE_stepper.c:491 */
  if (g_fake.gate[axis] == 0)
  {
    return AVE_STEPPER_EGATE;
  }
  /* ④ 脉冲量值上限 */
  mag = (uint32_t)((pulses < 0) ? -pulses : pulses);
  if (mag > (uint32_t)AVE_STEPPER_MAX_MOVE_PULSES)
  {
    return AVE_STEPPER_ERANGE;
  }
  /* ⑤ 频率量值范围（形参无符号，方向在 pulses 的符号里） */
  if ((hz < (uint32_t)AVE_STEPPER_MIN_HZ) || (hz > (uint32_t)AVE_STEPPER_MAX_HZ))
  {
    return AVE_STEPPER_ERANGE;
  }

  g_fake.last_pulses[axis] = pulses;
  g_fake.last_mag[axis]    = hz;
  return AVE_STEPPER_EOK;
}
