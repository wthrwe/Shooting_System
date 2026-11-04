/* 第七十二次降速的换算校验（主机侧，不碰硬件）。
 *
 * 目的：把 AVE_tuning.h 里的真值代进来，算清楚"这个偏差要转多少度/多少秒"，
 *       确认 2 rpm 下每跨过一个偏差所需时间 >> 一帧的间隔，否则降速没意义。
 *
 * 编译：gcc -std=c99 -Wall -Wextra -Wformat -Werror=format -I AVE \
 *            _selftest_ave/_gain_check.c -o _selftest_ave/_gain_check.exe
 * 判据：deviation 53 px 时必须 >= 3 帧（帧间隔按 100 ms 上限算）。
 */
#include <stdio.h>
#include "AVE_tuning.h"

#define FOV_PX    224.0f  /* AVE_tuning.h 注释里 "224 px ~= 30 deg" 的取景口径 */
#define FOV_DEG    30.0f
#define FRAME_MS  100.0f  /* 闭环延迟预算上限（注释里的 50~100 ms 取大值） */

/* 与 AVE_app.c:vision_deadzone 逐字同构：扣掉死区，输出在边界连续 */
static int dz(int e)
{
  if (e >  AVE_VISION_TOL_PX) { return e - AVE_VISION_TOL_PX; }
  if (e < -AVE_VISION_TOL_PX) { return e + AVE_VISION_TOL_PX; }
  return 0;
}

/* 与 AVE_app.c:vision_clamp_rpm 逐字同构 */
static float clamp_rpm(float r)
{
  if (r >  (float)AVE_VISION_TRACK_MAX_RPM) { return  (float)AVE_VISION_TRACK_MAX_RPM; }
  if (r < -(float)AVE_VISION_TRACK_MAX_RPM) { return -(float)AVE_VISION_TRACK_MAX_RPM; }
  return r;
}

int main(void)
{
  const int errs[] = { 12, 53, 112, 224 };
  const float px_per_deg = FOV_PX / FOV_DEG;
  int fail = 0;

  printf("Kp=%.3f TOL=%d MAX=%d rpm  (%.2f px/deg)\n",
         (double)AVE_VISION_YAW_PID_KP, AVE_VISION_TOL_PX,
         AVE_VISION_TRACK_MAX_RPM, (double)px_per_deg);

  for (unsigned i = 0; i < (unsigned)(sizeof(errs) / sizeof(errs[0])); ++i) {
    int   e   = errs[i];
    int   d   = dz(e);
    float raw = AVE_VISION_YAW_PID_KP * (float)d;
    float rpm = clamp_rpm(raw);
    float dps = (rpm < 0.0f ? -rpm : rpm) * 6.0f;   /* 1 rpm = 6 deg/s */
    float deg = (float)e / px_per_deg;
    float sec = (dps == 0.0f) ? 0.0f : deg / dps;
    float frames = sec * 1000.0f / FRAME_MS;

    printf("ex=%3d dz=%3d raw=%6.3f -> %5.2f rpm %5.1f deg/s, "
           "cross %4.1f deg in %5.2f s = %4.1f frames\n",
           e, d, (double)raw, (double)rpm, (double)dps, (double)deg,
           (double)sec, (double)frames);

    /* 1) 偏差超出死区时，必须给得出 >=3 帧的修正时间 */
    if (e > AVE_VISION_TOL_PX && frames < 3.0f) { fail = 1; }
    /* 2) 原始命令超过限速时，夹限必须真的生效（不是恰好没超） */
    if (raw > (float)AVE_VISION_TRACK_MAX_RPM &&
        rpm != (float)AVE_VISION_TRACK_MAX_RPM)   { fail = 1; }
    if (raw < -(float)AVE_VISION_TRACK_MAX_RPM &&
        rpm != -(float)AVE_VISION_TRACK_MAX_RPM)  { fail = 1; }
    /* 3) 没超限速时不许被削 */
    if (raw <= (float)AVE_VISION_TRACK_MAX_RPM &&
        raw >= -(float)AVE_VISION_TRACK_MAX_RPM && rpm != raw) { fail = 1; }
  }

  printf("%s\n", fail ? "FAIL" : "PASS");
  return fail;
}
