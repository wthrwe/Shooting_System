//云台应用层(角度，转速，定位)
//视觉/PID业务层AVE_vision -> 云台应用层AVE_gimbal -> 云台驱动层AVE_stepper -> 外设定时器 -> Emm42

#include "AVE_gimbal.h"
#include "AVE_stepper.h"   /* 唯一后端：波形、频率、DIR 时序全在那边 */
#include "AVE_tuning.h"    /* AVE_YAW/PITCH_STEPS_PER_DEG、AVE_GIMBAL_MAX_RPM */

/* ===== 一、编译期不变量 =====
 * 条件不成立则数组长度为 -1，编译直接失败；比 #error 强在能检查宏展开后的值。 */

#define AVE_GIMBAL_CHK(name, cond) typedef char ave_gimbal_chk_##name[((cond) ? 1 : -1)]

/** 本模块的两张表按轴数开，改轴数就必须回来改表。 */
AVE_GIMBAL_CHK(exactly_two_axes, (AVE_GIMBAL_AXIS_NUM == 2));

AVE_GIMBAL_CHK(axis_num_matches_backend,
               ((int)AVE_GIMBAL_AXIS_NUM == (int)AVE_STEPPER_AXIS_NUM));

/** 每度脉冲数必须 ≥ 1（截断后比）。真值 8.889 → 8，过。
 *  ⚠️ 局限：0.5 这类"小于 1 但未必非法"的值会被误判。本机 8.889/3200 脉冲，碰不到。 */
AVE_GIMBAL_CHK(yaw_steps_per_deg_sane,  ((uint32_t)AVE_YAW_STEPS_PER_DEG  >= 1U));
AVE_GIMBAL_CHK(pitch_steps_per_deg_sane, ((uint32_t)AVE_PITCH_STEPS_PER_DEG >= 1U));

/** 最大转速必须 ≥ 1 转/分（截断后比）。 */
AVE_GIMBAL_CHK(max_rpm_positive, ((uint32_t)AVE_GIMBAL_MAX_RPM >= 1U));

#define AVE_GIMBAL_HZ_CEIL(steps_per_deg) \
  ((uint32_t)AVE_GIMBAL_MAX_RPM * ((uint32_t)(steps_per_deg) + 1U) * 6U)

AVE_GIMBAL_CHK(max_rpm_fits_backend_hz,
               (AVE_GIMBAL_HZ_CEIL(AVE_YAW_STEPS_PER_DEG)  <= (uint32_t)AVE_STEPPER_MAX_HZ));
AVE_GIMBAL_CHK(max_rpm_fits_backend_hz_pitch,
               (AVE_GIMBAL_HZ_CEIL(AVE_PITCH_STEPS_PER_DEG) <= (uint32_t)AVE_STEPPER_MAX_HZ));

/* ===== 二、每轴静态配置 =====
 * 只有"每度脉冲数"一项。方向【不】在这里：
 * 方向由调用方传进来的 deg / rpm 的符号决定，后端按符号摆 DIR 电平，
 * 而"哪个电平算正方向"是机械安装事实，由 AVE_YAW/PITCH_POS_DIR_LEVEL 交给 AVE_stepper.c。 */

typedef struct
{
  float steps_per_deg;   /* 3200 脉冲/圈 ÷ 360 = 8.889 */
} AVE_GimbalAxisCfg;

/* static const 活在 flash 里不吃 RAM。顺序必须与 AVE_GimbalAxis 枚举一致。 */
static const AVE_GimbalAxisCfg s_cfg[AVE_GIMBAL_AXIS_NUM] =
{
  { AVE_YAW_STEPS_PER_DEG  },
  { AVE_PITCH_STEPS_PER_DEG }
};

/* ===== 三、可写状态 =====
 * 🔴 这里【只】存软件累计脉冲。
 * "门控开没开""忙不忙"一律去问后端（AVE_Stepper_IsGateOpen / IsBusy），不另存一份 ——
 * 两份状态迟早会分叉，而分叉之后软件认为的和硬件实际的发生的事对不上，
 * 排查时没有任何线索（本工程在"清状态把刚使能成功一起清掉"那件事上吃过这个亏）。 */

/** 软件累计脉冲的上限。钳位而不是报错：累计值只用于绝对移动的基准。 */
#define AVE_GIMBAL_PULSE_CLAMP   2147483647

static int64_t          s_pulses[AVE_GIMBAL_AXIS_NUM];
static AVE_GimbalStats  s_stats;

/* ===== 四、私有小工具 ===== */

static int axis_valid(AVE_GimbalAxis axis)
{
  return ((int)axis >= 0) && ((int)axis < (int)AVE_GIMBAL_AXIS_NUM);
}

/** 后端的返回码 → 本层的返回码。五条一一对应，未列出的（如 ECONFIG，只有 SelfTest 会返回）
 *  一律归到 EBADARG：真发生的话说明调用路径不对，不该被当成"正常但没做事"。 */
static int map_stepper(int rc)
{
  switch (rc)
  {
    case AVE_STEPPER_EOK:     return AVE_GIMBAL_EOK;
    case AVE_STEPPER_EBADARG: return AVE_GIMBAL_EBADARG;
    case AVE_STEPPER_ERANGE:  return AVE_GIMBAL_ERANGE;
    case AVE_STEPPER_EGATE:   return AVE_GIMBAL_EGATE;
    case AVE_STEPPER_EBUSY:   return AVE_GIMBAL_EBUSY;
    default:                  return AVE_GIMBAL_EBADARG;
  }
}

/**
  * 函    数：角度 → 带符号的脉冲数
  * 参    数：deg 角度；steps_per_deg 每度脉冲数；out 输出带符号脉冲
  * 返 回 值：AVE_GIMBAL_EOK / EBADARG / ERANGE；失败时 *out 不写
  * 注意事项：先做范围比较再转整数。(int32_t) 转换超范围/NaN 的浮点是未定义行为，
  *           armcc 与 gcc 可以给不同结果（本工程两边都要用）；NaN 与任何数比较皆假，
  *           会自然落到 ERANGE。
  */
static int gimbal_round_pulses(float deg, float steps_per_deg, int64_t *out)
{
  float f;

  if (out == 0)
  {
    return AVE_GIMBAL_EBADARG;
  }
  if (!(steps_per_deg > 0.0f))
  {
    return AVE_GIMBAL_EBADARG;
  }

  f = deg * steps_per_deg;         //角度 * 每度脉冲数 = 脉冲总数

  /* 上限取 2147483000 而非 2147483647：给"+0.5 后仍能安全转换"留余量。 */
  if (!((f > -2147483000.0f) && (f < 2147483000.0f)))
  {
    return AVE_GIMBAL_ERANGE;
  }

  if (f < 0.0f)
  {
    *out = (int64_t)(int32_t)(f - 0.5f);
  }
  else
  {
    *out = (int64_t)(int32_t)(f + 0.5f);
  }
  return AVE_GIMBAL_EOK;
}

/**
  * 函    数：转速（转/分）→ 步进频率（Hz）
  * 参    数：rpm 带符号转速；steps_per_deg 每度脉冲数；out 输出带符号频率
  * 返 回 值：AVE_GIMBAL_EOK / EBADARG / ERANGE；失败时 *out 不写
  * 注意事项：换算式 hz = rpm × 6 × steps_per_deg。
  *           那个 6 = 每分钟 60 秒取倒数(1/60) × 一圈 360 度 = 360/60 ——
  *           写成 ×6 是把"转/分 → 度/秒"和"度 → 脉冲"两步合并，少乘一个 6 就是慢 6 倍。
  *           🔴 rpm != 0 但四舍五入后 hz == 0 时返回 ERANGE 而不是 0：
  *           后端把 hz == 0 【当成"停"】（AVE_stepper.c:408），
  *           悄悄返回 0 会让"速度太小"变成"把轴停下来"这种完全不相干的行为。
  *           最小可表示转速 ≈ 1 ÷ (8.889 × 6) ≈ 0.019 转/分。
  */
static int gimbal_rpm_to_hz(float rpm, float steps_per_deg, int32_t *out)
{
  float   f;
  int32_t v;

  if (out == 0)
  {
    return AVE_GIMBAL_EBADARG;
  }
  if (!(steps_per_deg > 0.0f))
  {
    return AVE_GIMBAL_EBADARG;
  }
  if (!((rpm >= -AVE_GIMBAL_MAX_RPM) && (rpm <= AVE_GIMBAL_MAX_RPM)))
  {
    return AVE_GIMBAL_ERANGE;
  }

  f = rpm * 6.0f * steps_per_deg;     //频率 = rpm * 6 * 每度脉冲数

  if (!((f > -2147483000.0f) && (f < 2147483000.0f)))
  {
    return AVE_GIMBAL_ERANGE;
  }

  v = (f < 0.0f) ? (int32_t)(f - 0.5f) : (int32_t)(f + 0.5f);

  if ((v == 0) && (rpm != 0.0f))
  {
    return AVE_GIMBAL_ERANGE;
  }

  *out = v;
  return AVE_GIMBAL_EOK;
}

/** 累计脉冲累加 + 钳位。只在确认后端已受理之后调用。 */
static void pulses_add(int axis, int64_t delta)
{
  s_pulses[axis] += delta;

  if (s_pulses[axis] > AVE_GIMBAL_PULSE_CLAMP)
  {
    s_pulses[axis] = AVE_GIMBAL_PULSE_CLAMP;
  }
  else if (s_pulses[axis] < -AVE_GIMBAL_PULSE_CLAMP)
  {
    s_pulses[axis] = -AVE_GIMBAL_PULSE_CLAMP;
  }
}

/**
  * 函    数：绝对值定位的公共骨架（MoveRelDeg / MoveAbsDeg 都走它）
  * 参    数：axis 轴号（调用方已校验）；delta 带符号脉冲增量；rpm 转速
  * 返 回 值：AVE_GIMBAL_EOK / ERANGE / EGATE / EBUSY
  * 注意事项：三条不显然但必须保持的顺序，见各段注释。
  */
static int gimbal_move(AVE_GimbalAxis axis, int64_t delta, float rpm)
{
  AVE_StepperAxis ax = (AVE_StepperAxis)axis;
  int32_t         hz = 0;
  uint32_t        mag;
  int             rc;

  /* ① 先判断电机是否忙 */
  if (AVE_Stepper_IsBusy(ax) != 0)
  {
    s_stats.moves_bypassed++;
    return AVE_GIMBAL_EBUSY;
  }

  /* ② 再判脉冲数上限。
   *    🔴 必须在这里判，不能只靠后端：后端的形参是 int32_t，
   *    而 MoveAbsDeg 算出来的 delta 可以到 ±43 亿（want − 累计），
   *    直接强制转换会先【截断】成一个看似合法的数 —— 又是一次静默错误。
   *    判据用后端自己的上限，兼容性最好。 */
  if ((delta > (int64_t)AVE_STEPPER_MAX_MOVE_PULSES) ||
      (delta < -(int64_t)AVE_STEPPER_MAX_MOVE_PULSES))
  {
    s_stats.moves_rejected++;
    return AVE_GIMBAL_ERANGE;
  }

  rc = gimbal_rpm_to_hz(rpm, s_cfg[axis].steps_per_deg, &hz);
  if (rc != AVE_GIMBAL_EOK)
  {
    s_stats.moves_rejected++;
    return rc;
  }

  /* ③ rpm 换算成 hz；校验 hz */
  if (delta == 0)
  {
    return AVE_GIMBAL_EOK;
  }
  if (hz == 0)
  {
    s_stats.moves_rejected++;
    return AVE_GIMBAL_ERANGE;
  }

  /* 后端形参 hz 是【无符号】，方向由 pulses 的符号给 —— 取绝对值。 */
  mag = (uint32_t)((hz < 0) ? -hz : hz);

  rc = map_stepper(AVE_Stepper_MovePulses(ax, (int32_t)delta, mag));
  if (rc != AVE_GIMBAL_EOK)
  {
    /* ④ 没受理 ⇒ 驱动器的目标位置没变 ⇒ 累计值一个字都不许动。 */
    s_stats.moves_rejected++;
    return rc;
  }

  //⑤ 调用底层`AVE_Stepper_MovePulses`执行脉冲运动
  pulses_add((int)axis, delta);
  s_stats.moves_accepted++;
  return AVE_GIMBAL_EOK;
}

/* ===== 五、角度拆成脉冲数量 + DIR 方向电平，给底层驱动使用 ===== */

int AVE_Gimbal_SplitPulses(float deg, float steps_per_deg, uint8_t pos_dir_level,
                           uint32_t *pulses, uint8_t *dir)
{
  int64_t n = 0;
  int     rc;

  /* 失败时两个输出一个都不写 —— 与本模块其它接口同一套约定。 */
  if ((pulses == 0) || (dir == 0) || (pos_dir_level > 1U))
  {
    return AVE_GIMBAL_EBADARG;
  }

  rc = gimbal_round_pulses(deg, steps_per_deg, &n);
  if (rc != AVE_GIMBAL_EOK)
  {
    return rc;
  }

  if (n < 0)
  {
    *pulses = (uint32_t)(-n);
    *dir    = (uint8_t)(pos_dir_level ^ 1U);
  }
  else
  {
    *pulses = (uint32_t)n;
    *dir    = pos_dir_level;
  }
  return AVE_GIMBAL_EOK;
}

/* ===== 六、清零脉冲、统计计数器 ===== */

void AVE_Gimbal_Init(void)
{
  int i;

  for (i = 0; i < (int)AVE_GIMBAL_AXIS_NUM; i++)
  {
    s_pulses[i] = 0;
  }

  /* 逐字段清零而不是 memset：加字段时这里会编译报错提醒，
   * 比"新字段悄悄带着上一轮的垃圾值"安全。 */
  s_stats.moves_accepted = 0U;
  s_stats.moves_bypassed = 0U;
  s_stats.moves_rejected = 0U;
  s_stats.gate_changes   = 0U;
}

/* ===== 七、门控（电机使能 / 脱机） ===== */
//单轴使能 / 失能
int AVE_Gimbal_SetGate(AVE_GimbalAxis axis, uint8_t on)
{
  AVE_StepperAxis ax = (AVE_StepperAxis)axis;
  int             before;
  int             after;
  int             rc;

  if (!axis_valid(axis) || (on > 1U))
  {
    return AVE_GIMBAL_EBADARG;
  }

  /* 前后各问一次后端，而不是拿本地镜像比较 —— 状态只有一份，不会分叉。
   * 代价是两次调用，但这不在一秒钟几千次的路径上。 */
  before = AVE_Stepper_IsGateOpen(ax);
  rc     = map_stepper(AVE_Stepper_SetGate(ax, on));
  after  = AVE_Stepper_IsGateOpen(ax);

  if (rc == AVE_GIMBAL_EOK)
  {
    if ((before >= 0) && (after >= 0) && (before != after))
    {
      s_stats.gate_changes++;
    }
  }
  return rc;
}

int AVE_Gimbal_IsGateOpen(AVE_GimbalAxis axis)
{
  int v;

  if (!axis_valid(axis))
  {
    return 0;
  }

  v = AVE_Stepper_IsGateOpen((AVE_StepperAxis)axis);
  return (v > 0) ? 1 : 0;
}

//双轴一起使能 / 失能
int AVE_Gimbal_SetGateAll(uint8_t on)
{
  int i;
  int bad = 0;

  if (on > 1U)
  {
    return AVE_GIMBAL_EBADARG;
  }

  for (i = 0; i < (int)AVE_GIMBAL_AXIS_NUM; i++)
  {
    if (AVE_Gimbal_SetGate((AVE_GimbalAxis)i, on) != AVE_GIMBAL_EOK)
    {
      /* 不回滚：补发只会让"到底做没做"更说不清，而重复设同一个门控值本身安全。 */
      bad = 1;
    }
  }

  return (bad != 0) ? AVE_GIMBAL_EBADARG : AVE_GIMBAL_EOK;
}

/* ===== 八、运动 ===== */
//速度模式
int AVE_Gimbal_SetSpeedRpm(AVE_GimbalAxis axis, float rpm)
{
  AVE_StepperAxis ax = (AVE_StepperAxis)axis;
  int32_t         hz = 0;
  int             rc;

  if (!axis_valid(axis))
  {
    return AVE_GIMBAL_EBADARG;
  }

  /* 🔴 先让路，理由同 gimbal_move 的 ①：rpm == 0 也会走到这里，
   * 而后端的 hz==0 分支排在忙碌检查之前 ⇒ 不先挡就会打断定位。 */
  if (AVE_Stepper_IsBusy(ax) != 0)
  {
    s_stats.moves_bypassed++;
    return AVE_GIMBAL_EBUSY;
  }

  rc = gimbal_rpm_to_hz(rpm, s_cfg[axis].steps_per_deg, &hz);
  if (rc != AVE_GIMBAL_EOK)
  {
    s_stats.moves_rejected++;
    return rc;
  }

  /* hz == 0（即 rpm == 0）在这里是合法且有意义的：后端把 0 解释成"停"。 */
  rc = map_stepper(AVE_Stepper_SetFreqHz(ax, hz));
  if (rc != AVE_GIMBAL_EOK)
  {
    s_stats.moves_rejected++;
  }
  return rc;
}

//相对运动
int AVE_Gimbal_MoveRelDeg(AVE_GimbalAxis axis, float deg, float rpm)
{
  int64_t delta = 0;
  int     rc;

  if (!axis_valid(axis))
  {
    return AVE_GIMBAL_EBADARG;
  }

  rc = gimbal_round_pulses(deg, s_cfg[axis].steps_per_deg, &delta);
  if (rc != AVE_GIMBAL_EOK)
  {
    s_stats.moves_rejected++;
    return rc;
  }

  return gimbal_move(axis, delta, rpm);
}

//绝对定位
int AVE_Gimbal_MoveAbsDeg(AVE_GimbalAxis axis, float deg, float rpm)
{
  int64_t want  = 0;
  int64_t delta;
  int     rc;

  if (!axis_valid(axis))
  {
    return AVE_GIMBAL_EBADARG;
  }

  rc = gimbal_round_pulses(deg, s_cfg[axis].steps_per_deg, &want);
  if (rc != AVE_GIMBAL_EOK)
  {
    s_stats.moves_rejected++;
    return rc;
  }

  /* int64 相减：want 与累计各自都在 int32 范围内，差值可能到 ±43 亿，
   * 用 int32 会在这里就溢出（UB）。上限在 gimbal_move 里判。 */
  delta = want - s_pulses[axis];

  return gimbal_move(axis, delta, rpm);
}

//停止单轴
int AVE_Gimbal_Stop(AVE_GimbalAxis axis)
{
  if (!axis_valid(axis))
  {
    return AVE_GIMBAL_EBADARG;
  }

  return map_stepper(AVE_Stepper_Stop((AVE_StepperAxis)axis));
}

//停止双轴
int AVE_Gimbal_StopAll(void)
{
  int i;
  int bad = 0;

  for (i = 0; i < (int)AVE_GIMBAL_AXIS_NUM; i++)
  {
    if (AVE_Gimbal_Stop((AVE_GimbalAxis)i) != AVE_GIMBAL_EOK)
    {
      bad = 1;
    }
  }

  return (bad != 0) ? AVE_GIMBAL_EBADARG : AVE_GIMBAL_EOK;
}

/* ===== 九、状态查询 ===== */
//查询电机是否正在运动
int AVE_Gimbal_IsBusy(AVE_GimbalAxis axis)
{
  int v;

  if (!axis_valid(axis))
  {
    return 0;
  }

  v = AVE_Stepper_IsBusy((AVE_StepperAxis)axis);
  return (v > 0) ? 1 : 0;
}

//读取软件累计脉冲
int32_t AVE_Gimbal_GetPulses(AVE_GimbalAxis axis)
{
  if (!axis_valid(axis))
  {
    return 0;
  }
  return (int32_t)s_pulses[axis];   /* 已由 pulses_add 钳在 int32 范围内 */
}

//脉冲清零
void AVE_Gimbal_ZeroPulses(void)
{
  int i;

  for (i = 0; i < (int)AVE_GIMBAL_AXIS_NUM; i++)
  {
    s_pulses[i] = 0;
  }
}

//返回统计信息只读指针，串口打印调试
const AVE_GimbalStats *AVE_Gimbal_Stats(void)
{
  return &s_stats;
}
