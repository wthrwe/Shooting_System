/* ============================================================================
 * test_gimbal.c —— AVE_gimbal.c（云台薄壳）桌面行为测试
 *
 * 【怎么跑】见 ../run_test_gimbal.sh，或直接：
 *   gcc -std=c99 -O0 -g -Wall -Wextra -I host/fake -I host -I <AVE> \
 *       -o out/test_gimbal.exe host/test_gimbal.c host/fake_stepper.c \
 *       <AVE>/AVE_gimbal.c
 *
 * 🔴 【本文件的铁律】期望值一律手算成【字面数字】写死，绝不在测试里重算公式。
 *    反例（禁止）：CHECK(hz == (int32_t)(rpm * 6.0f * 8.889f))
 *    正例（本文件）：CHECK(g_fake.last_hz[YAW] == 10667, ...)  —— 200×6×8.889 = 10666.8 → 10667
 *    ── 第 29 次最大教训：桌面 166/166 全绿却与硅片对不上，根因就是
 *       测试的期望值和被测代码**共用同一个反向模型**，两边一起错、错得一致。
 *       手算的字面量是唯一独立于被测代码的基线。
 *    （⚠️ 别在块注释里嵌块注释符号：本文件第一版就是这么挂的 ——
 *      嵌套的开标记报 -Wcomment，闭标记直接把本注释提前结束，
 *      后面整段中文都变成代码，报一串 stray '\342'。）
 *
 * 【两个锚点数字，凡是它俩对不上就是硬伤】
 *   3200 脉冲 = 1 圈 = 360°（16 细分 × 200 步/圈 = 3200）
 *   3200 脉冲/分 = 60 转/分 = 1 圈/分
 * ==========================================================================*/

#include <stdio.h>
#include <math.h>
#include "AVE_gimbal.h"
#include "AVE_tuning.h"
#include "fake_stepper.h"

/* ===== 极简断言框架 ===== */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                                  \
  do {                                                                    \
    if (cond) { g_pass++; }                                               \
    else {                                                                \
      g_fail++;                                                           \
      printf("    [FAIL] 第%d行: ", __LINE__);                            \
      printf(__VA_ARGS__);                                                \
      printf("\n");                                                       \
    }                                                                     \
  } while (0)

#define CHECK_EQ(got, want, ...)                                          \
  CHECK((got) == (want), __VA_ARGS__)

static void group(const char *name)
{
  printf("\n---- %s ----\n", name);
}

/* 用例之间清场：云台层状态 + 假后端状态一起复位。
   不复位就会互相污染 —— 上一组留下的"忙"会让下一组莫名其妙 EBUSY。 */
static void reset_all(void)
{
  fake_reset();
  AVE_Gimbal_Init();
}

int main(void)
{
  AVE_GimbalAxis YAW  = AVE_GIMBAL_AXIS_YAW;
  AVE_GimbalAxis PITCH = AVE_GIMBAL_AXIS_PITCH;

  printf("================================================================\n");
  printf(" AVE_gimbal.c 桌面行为测试\n");
  printf(" 参数：每度脉冲数 %.3f / %.3f，最大转速 %.1f 转/分\n",
         (double)AVE_YAW_STEPS_PER_DEG, (double)AVE_PITCH_STEPS_PER_DEG,
         (double)AVE_GIMBAL_MAX_RPM);
  printf("================================================================\n");

  /* ================================================================
   * A 组 · 转速换算   hz = 转/分 × 6 × 每度脉冲数
   * ==============================================================*/
  group("A 转速换算（转/分 → 步进频率 Hz）");

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 200.0f), AVE_GIMBAL_EOK, "A1 200转/分 应受理");
  /* 200 × 6 = 1200；1200 × 8.889 = 10666.8 → 四舍五入 10667 */
  CHECK_EQ(g_fake.last_hz[YAW], 10667, "A1 频率应为 10667，实得 %d", (int)g_fake.last_hz[YAW]);

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, -200.0f), AVE_GIMBAL_EOK, "A2 反向应受理");
  /* 负号必须原样传给后端（后端靠它的符号摆 DIR） */
  CHECK_EQ(g_fake.last_hz[YAW], -10667, "A2 应保留负号，实得 %d", (int)g_fake.last_hz[YAW]);

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 60.0f), AVE_GIMBAL_EOK, "A3 60转/分 应受理");
  /* 🔴 锚点：60 × 6 = 360；360 × 8.889 = 3200.04 → 3200。
     3200 脉冲/分 = 1 圈/分（3200 脉冲/圈）—— 对不上就是换算错了 */
  CHECK_EQ(g_fake.last_hz[YAW], 3200, "A3 应为 3200，实得 %d", (int)g_fake.last_hz[YAW]);

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 0.0f), AVE_GIMBAL_EOK, "A4 零速应受理（= 停）");
  CHECK_EQ(g_fake.last_hz[YAW], 0, "A4 应为 0，实得 %d", (int)g_fake.last_hz[YAW]);
  CHECK_EQ(g_fake.calls_setfreq[YAW], 1U, "A4 零速也必须真的发给后端");

  reset_all();
  g_fake.last_hz[YAW] = 7777;   /* 哨兵：被拒时不许被改写 */
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 200.5f), AVE_GIMBAL_ERANGE, "A5 超最大转速应 ERANGE");
  CHECK_EQ(g_fake.last_hz[YAW], 7777, "A5 被拒后不该动后端");
  CHECK_EQ(g_fake.calls_setfreq[YAW], 0U, "A5 被拒时不该调用后端");

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, NAN), AVE_GIMBAL_ERANGE, "A6 NaN 应 ERANGE");
  CHECK_EQ(g_fake.calls_setfreq[YAW], 0U, "A6 被拒时不该调用后端");

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, INFINITY), AVE_GIMBAL_ERANGE, "A7 无穷大应 ERANGE");

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 0.01f), AVE_GIMBAL_EOK, "A8 极慢速仍应受理");
  /* 0.01 × 6 = 0.06；0.06 × 8.889 = 0.5333 → 四舍五入 1 */
  CHECK_EQ(g_fake.last_hz[YAW], 1, "A8 应为 1，实得 %d", (int)g_fake.last_hz[YAW]);

  reset_all();
  /* 0.005 × 6 = 0.03；0.03 × 8.889 = 0.26667 → 四舍五入 0。
     🔴 而 0 在后端【就是"停"】⇒ 必须在这里拦住，不能放行。
        放行的症状：下发"几乎不转"，电机却【停下来】—— 完全不相干的行为。 */
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 0.005f), AVE_GIMBAL_ERANGE,
           "A9 慢到换算成 0Hz 应 ERANGE（0 在后端等于急停）");
  CHECK_EQ(g_fake.calls_setfreq[YAW], 0U, "A9 被拒时不该调用后端");

  /* ================================================================
   * B 组 · 角度换算 + 相对移动
   * ==============================================================*/
  group("B 角度 → 脉冲（相对移动）");

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 10.0f, 200.0f), AVE_GIMBAL_EOK, "B1 10度 应受理");
  /* 10 × 8.889 = 88.89 → 89 */
  CHECK_EQ(g_fake.last_pulses[YAW], 89, "B1 应为 89 脉冲，实得 %d", (int)g_fake.last_pulses[YAW]);
  CHECK_EQ(g_fake.last_mag[YAW], 10667U, "B1 频率应为 10667，实得 %u", g_fake.last_mag[YAW]);
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 89, "B1 累计应为 89");

  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, -10.0f, 200.0f), AVE_GIMBAL_EOK, "B2 反向 应受理");
  CHECK_EQ(g_fake.last_pulses[YAW], -89, "B2 应为 -89，实得 %d", (int)g_fake.last_pulses[YAW]);
  CHECK_EQ(g_fake.last_mag[YAW], 10667U, "B2 频率形参是无符号的，仍应为 10667");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "B2 累计应回到 0");

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 1.0f, 200.0f), AVE_GIMBAL_EOK, "B3 1度 应受理");
  /* 1 × 8.889 = 8.889 → 9（四舍五入，不是截断 8） */
  CHECK_EQ(g_fake.last_pulses[YAW], 9, "B3 应为 9（四舍五入），实得 %d", (int)g_fake.last_pulses[YAW]);

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 360.0f, 200.0f), AVE_GIMBAL_EOK, "B4 一圈 应受理");
  /* 🔴 锚点：360 × 8.889 = 3200.04 → 3200 = 正好一圈 */
  CHECK_EQ(g_fake.last_pulses[YAW], 3200, "B4 一圈应为 3200 脉冲，实得 %d", (int)g_fake.last_pulses[YAW]);

  reset_all();
  /* 0.05 × 8.889 = 0.4444 → 0 脉冲。空动作用户看不见，也不该去打扰后端 */
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 0.05f, 200.0f), AVE_GIMBAL_EOK, "B5 空动作应 EOK");
  CHECK_EQ(g_fake.calls_movepulses[YAW], 0U, "B5 空动作不该调用后端");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "B5 空动作累计不应变");

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 0.06f, 200.0f), AVE_GIMBAL_EOK, "B6 半个脉冲 应受理");
  /* 0.06 × 8.889 = 0.5333 → 1 */
  CHECK_EQ(g_fake.last_pulses[YAW], 1, "B6 应为 1，实得 %d", (int)g_fake.last_pulses[YAW]);

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, NAN, 200.0f), AVE_GIMBAL_ERANGE, "B7 NaN 角度应 ERANGE");
  CHECK_EQ(g_fake.calls_movepulses[YAW], 0U, "B7 被拒时不该调用后端");

  reset_all();
  /* 1e9 × 8.889 = 8.889e9，远超 int32 ⇒ 必须在这里 ERANGE，
     不能先强制转换（那是 UB，armcc 和 gcc 可以给不同结果） */
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 1e9f, 200.0f), AVE_GIMBAL_ERANGE, "B8 超大角度应 ERANGE");

  reset_all();
  /* 300000 × 8.889 = 2666700 —— 在 int32 内，但超过后端单次上限 2000000 */
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 300000.0f, 200.0f), AVE_GIMBAL_ERANGE,
           "B9 超后端单次上限应 ERANGE");
  CHECK_EQ(g_fake.calls_movepulses[YAW], 0U, "B9 被拒时不该调用后端");

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(PITCH, 1000.0f, 200.0f), AVE_GIMBAL_EOK, "B10 俯仰轴应受理");
  /* 1000 × 8.889 = 8889 */
  CHECK_EQ(g_fake.last_pulses[PITCH], 8889, "B10 应为 8889，实得 %d", (int)g_fake.last_pulses[PITCH]);
  CHECK_EQ(AVE_Gimbal_GetPulses(PITCH), 8889, "B10 俯仰轴累计应为 8889");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "B10 水平轴不该被牵连");

  /* ================================================================
   * C 组 · 仲裁（定位优先，视觉让路）
   * ==============================================================*/
  group("C 仲裁：该轴忙时，视觉这一拍让路");

  reset_all();
  g_fake.busy[YAW] = 1;   /* 假装水平轴正在走文本定位 */
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 10.0f, 200.0f), AVE_GIMBAL_EBUSY, "C1 忙时应 EBUSY");
  CHECK_EQ(g_fake.calls_movepulses[YAW], 0U, "C1 忙时【一个脉冲都不许发】");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "C1 让路时累计不许动");
  CHECK_EQ(AVE_Gimbal_Stats()->moves_bypassed, 1U, "C1 应记一次 bypassed");

  /* 🔴 C2 = 本套件最重要的一条回归。
     后端 AVE_stepper.c:408 把 `hz == 0 → Stop()` 排在忙碌检查【之前】。
     于是"下发 0 转/分"在后端等价于【急停】。若薄壳把 rpm==0 直接透传，
     视觉每拍结束时发的"零速"就会把正在走的文本定位打断 —— 而且不报任何错。
     判据不能只看返回码，必须断言【后端根本没被调用】：
     单看 EBUSY 无法区分"薄壳挡住了"和"后端挡住了"。 */
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 0.0f), AVE_GIMBAL_EBUSY,
           "C2 忙时下发零速也必须 EBUSY（否则会把定位打断）");
  CHECK_EQ(g_fake.calls_setfreq[YAW], 0U,
           "C2 🟥 忙时零速【必须一步都不走到后端】——否则后端的 0Hz=停 会打断定位");

  group("C 仲裁（续）");
  reset_all();
  g_fake.busy[YAW] = 1;
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 100.0f), AVE_GIMBAL_EBUSY, "C3 忙时视觉改频应 EBUSY");
  CHECK_EQ(g_fake.calls_setfreq[YAW], 0U, "C3 忙时不该调用后端");

  CHECK_EQ(AVE_Gimbal_IsBusy(YAW), 1, "C4 忙状态应透传");
  g_fake.busy[YAW] = 0;   /* 定位走完 */
  CHECK_EQ(AVE_Gimbal_IsBusy(YAW), 0, "C4 走完后应为空闲");
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 100.0f), AVE_GIMBAL_EOK, "C4 空闲后应受理");
  /* 100 × 6 = 600；600 × 8.889 = 5333.4 → 5333 */
  CHECK_EQ(g_fake.last_hz[YAW], 5333, "C4 应为 5333，实得 %d", (int)g_fake.last_hz[YAW]);

  /* ================================================================
   * D 组 · 门控（**不是**使能 —— 关门不会让电机松轴）
   * ==============================================================*/
  group("D 门控");

  reset_all();
  g_fake.gate[YAW] = 0;   /* 关门 */
  CHECK_EQ(AVE_Gimbal_MoveRelDeg(YAW, 10.0f, 200.0f), AVE_GIMBAL_EGATE, "D1 关门应 EGATE");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "D1 被门挡住时累计不许动");
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm(YAW, 100.0f), AVE_GIMBAL_EGATE, "D2 关门改频应 EGATE");

  reset_all();
  /* 门本来就开着 ⇒ 再开一次不算一次"变化"；关上才算 */
  CHECK_EQ(AVE_Gimbal_SetGate(YAW, 1), AVE_GIMBAL_EOK, "D3 开门应 EOK");
  CHECK_EQ(AVE_Gimbal_Stats()->gate_changes, 0U, "D3 本来就开着，不该记变化");
  CHECK_EQ(AVE_Gimbal_IsGateOpen(YAW), 1, "D3 应报开门");
  CHECK_EQ(AVE_Gimbal_SetGate(YAW, 0), AVE_GIMBAL_EOK, "D3 关门应 EOK");
  CHECK_EQ(AVE_Gimbal_Stats()->gate_changes, 1U, "D3 关上门应记一次变化");
  CHECK_EQ(AVE_Gimbal_IsGateOpen(YAW), 0, "D3 应报关门");

  CHECK_EQ(AVE_Gimbal_SetGate(YAW, 2), AVE_GIMBAL_EBADARG, "D4 门控值非法应 EBADARG");
  CHECK_EQ(AVE_Gimbal_SetGateAll(2), AVE_GIMBAL_EBADARG, "D4 门控值非法应 EBADARG");
  CHECK_EQ(AVE_Gimbal_SetGateAll(1), AVE_GIMBAL_EOK, "D5 两轴开门应 EOK");
  CHECK_EQ(AVE_Gimbal_IsGateOpen(YAW), 1, "D5 水平轴应开门");
  CHECK_EQ(AVE_Gimbal_IsGateOpen(PITCH), 1, "D5 俯仰轴应开门");

  /* ================================================================
   * E 组 · 绝对移动与软件累计脉冲
   * ==============================================================*/
  group("E 绝对移动（相对软件零点）");

  reset_all();
  CHECK_EQ(AVE_Gimbal_MoveAbsDeg(YAW, 10.0f, 200.0f), AVE_GIMBAL_EOK, "E1 绝对移动到10度");
  CHECK_EQ(g_fake.last_pulses[YAW], 89, "E1 应为 89，实得 %d", (int)g_fake.last_pulses[YAW]);
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 89, "E1 累计应为 89");

  /* 已经在 10 度 ⇒ 增量 0 ⇒ 不该打扰后端 */
  CHECK_EQ(AVE_Gimbal_MoveAbsDeg(YAW, 10.0f, 200.0f), AVE_GIMBAL_EOK, "E2 原地不动应 EOK");
  CHECK_EQ(g_fake.calls_movepulses[YAW], 1U, "E2 增量为 0 时不该再调用后端");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 89, "E2 累计不应变");

  CHECK_EQ(AVE_Gimbal_MoveAbsDeg(YAW, 20.0f, 200.0f), AVE_GIMBAL_EOK, "E3 绝对移动到20度");
  /* want = 20 × 8.889 = 177.78 → 178；delta = 178 - 89 = 89 */
  CHECK_EQ(g_fake.last_pulses[YAW], 89, "E3 增量应为 89，实得 %d", (int)g_fake.last_pulses[YAW]);
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 178, "E3 累计应为 178");

  CHECK_EQ(AVE_Gimbal_MoveAbsDeg(YAW, 0.0f, 200.0f), AVE_GIMBAL_EOK, "E4 回到零点");
  /* want = 0；delta = 0 - 178 = -178 */
  CHECK_EQ(g_fake.last_pulses[YAW], -178, "E4 增量应为 -178，实得 %d", (int)g_fake.last_pulses[YAW]);
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "E4 累计应回到 0");

  /* 1000000 × 8.889 = 8889000 —— 在 int32 内（没触发换算的范围检查），
     但作为【增量】超过后端单次上限 2000000。这一条专测 gimbal_move 里
     那道"给 int32 形参兜底"的闸：少了它就会静默截断成一个看似合法的数。 */
  CHECK_EQ(AVE_Gimbal_MoveAbsDeg(YAW, 1000000.0f, 200.0f), AVE_GIMBAL_ERANGE,
           "E5 增量超后端上限应 ERANGE");
  CHECK_EQ(AVE_Gimbal_GetPulses(YAW), 0, "E5 被拒时累计不许动");

  /* 累计脉冲钳位。224900 × 8.889 = 1999136 脉冲/次（≤ 2000000，合法）。
     1080 次 × 1999136 = 2159066880 > 2147483647 ⇒ 必须钳住，不许绕回负数。 */
  reset_all();
  {
    int i;
    int all_ok = 1;
    for (i = 0; i < 1080; i++)
    {
      if (AVE_Gimbal_MoveRelDeg(PITCH, 224900.0f, 200.0f) != AVE_GIMBAL_EOK)
      {
        all_ok = 0;
        break;
      }
    }
    CHECK_EQ(all_ok, 1, "E6 1080 次移动都应被受理（第 %d 次失败）", i + 1);
    CHECK_EQ(AVE_Gimbal_GetPulses(PITCH), 2147483647,
             "E6 累计应钳在 2147483647，实得 %d", (int)AVE_Gimbal_GetPulses(PITCH));
  }
  AVE_Gimbal_ZeroPulses();
  CHECK_EQ(AVE_Gimbal_GetPulses(PITCH), 0, "E6 ZeroPulses 应清零");

  /* ================================================================
   * F 组 · 参数越界
   * ==============================================================*/
  group("F 轴号越界一律 EBADARG");

  reset_all();
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm((AVE_GimbalAxis)-1, 100.0f), AVE_GIMBAL_EBADARG, "F 轴 -1");
  CHECK_EQ(AVE_Gimbal_SetSpeedRpm((AVE_GimbalAxis)2, 100.0f), AVE_GIMBAL_EBADARG, "F 轴 2");
  CHECK_EQ(AVE_Gimbal_MoveRelDeg((AVE_GimbalAxis)-1, 10.0f, 200.0f), AVE_GIMBAL_EBADARG, "F 轴 -1");
  CHECK_EQ(AVE_Gimbal_MoveRelDeg((AVE_GimbalAxis)2, 10.0f, 200.0f), AVE_GIMBAL_EBADARG, "F 轴 2");
  CHECK_EQ(AVE_Gimbal_MoveAbsDeg((AVE_GimbalAxis)-1, 10.0f, 200.0f), AVE_GIMBAL_EBADARG, "F 轴 -1");
  CHECK_EQ(AVE_Gimbal_SetGate((AVE_GimbalAxis)2, 1), AVE_GIMBAL_EBADARG, "F 轴 2");
  CHECK_EQ(AVE_Gimbal_Stop((AVE_GimbalAxis)2), AVE_GIMBAL_EBADARG, "F 轴 2");
  /* 查询类接口的约定是"返回 0 表示无效"，而不是返回错误码 */
  CHECK_EQ(AVE_Gimbal_IsBusy((AVE_GimbalAxis)2), 0, "F 轴 2 查询应回 0");
  CHECK_EQ(AVE_Gimbal_IsGateOpen((AVE_GimbalAxis)2), 0, "F 轴 2 查询应回 0");
  CHECK_EQ(AVE_Gimbal_GetPulses((AVE_GimbalAxis)2), 0, "F 轴 2 查询应回 0");

  /* ================================================================
   * G 组 · 停止（**唯一**能打断定位的入口）
   * ==============================================================*/
  group("G 停止");

  reset_all();
  g_fake.busy[YAW] = 1;
  CHECK_EQ(AVE_Gimbal_Stop(YAW), AVE_GIMBAL_EOK, "G1 停止应 EOK");
  CHECK(g_fake.calls_stop[YAW] >= 1U, "G1 应真的调用了后端 Stop");
  CHECK_EQ(AVE_Gimbal_IsBusy(YAW), 0, "G1 停完应空闲");
  /* 停止【不是】关门：门还开着，下一条指令照样能走 */
  CHECK_EQ(AVE_Gimbal_IsGateOpen(YAW), 1, "G1 停止不该顺手把门关了");

  reset_all();
  g_fake.busy[YAW] = 1;
  g_fake.busy[PITCH] = 1;
  CHECK_EQ(AVE_Gimbal_StopAll(), AVE_GIMBAL_EOK, "G2 两轴停止应 EOK");
  CHECK_EQ(AVE_Gimbal_IsBusy(YAW), 0, "G2 水平轴应停");
  CHECK_EQ(AVE_Gimbal_IsBusy(PITCH), 0, "G2 俯仰轴应停");

  /* ================================================================
   * H 组 · SplitPulses（公开纯函数，全工程暂无调用者）
   * ==============================================================*/
  group("H SplitPulses");

  {
    uint32_t p = 999U;
    uint8_t  d = 9U;

    reset_all();
    CHECK_EQ(AVE_Gimbal_SplitPulses(10.0f, 8.889f, 1U, &p, &d), AVE_GIMBAL_EOK, "H1 应 EOK");
    CHECK_EQ(p, 89U, "H1 脉冲应为 89，实得 %u", p);
    CHECK_EQ(d, 1U, "H1 正角度应给 pos_dir_level 本身");

    p = 999U; d = 9U;
    CHECK_EQ(AVE_Gimbal_SplitPulses(-10.0f, 8.889f, 1U, &p, &d), AVE_GIMBAL_EOK, "H2 应 EOK");
    CHECK_EQ(p, 89U, "H2 脉冲取绝对值，应为 89");
    CHECK_EQ(d, 0U, "H2 负角度应给 pos_dir_level 取反");

    p = 999U; d = 9U;
    CHECK_EQ(AVE_Gimbal_SplitPulses(10.0f, 8.889f, 0U, &p, &d), AVE_GIMBAL_EOK, "H3 应 EOK");
    CHECK_EQ(d, 0U, "H3 正角度应给 0（该轴正方向就是低电平）");

    CHECK_EQ(AVE_Gimbal_SplitPulses(10.0f, 8.889f, 2U, &p, &d), AVE_GIMBAL_EBADARG,
             "H4 方向电平只能是 0/1");
    CHECK_EQ(AVE_Gimbal_SplitPulses(10.0f, 8.889f, 1U, NULL, &d), AVE_GIMBAL_EBADARG,
             "H5 空指针应 EBADARG");
    CHECK_EQ(AVE_Gimbal_SplitPulses(10.0f, 0.0f, 1U, &p, &d), AVE_GIMBAL_EBADARG,
             "H5 每度脉冲数为 0 应 EBADARG");
    CHECK_EQ(AVE_Gimbal_SplitPulses(10.0f, NAN, 1U, &p, &d), AVE_GIMBAL_EBADARG,
             "H5 每度脉冲数为 NaN 应 EBADARG");
  }

  /* ================================================================
   * 汇总
   * ==============================================================*/
  printf("\n================================================================\n");
  printf(" 通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  printf("================================================================\n");
  return (g_fail == 0) ? 0 : 1;
}
