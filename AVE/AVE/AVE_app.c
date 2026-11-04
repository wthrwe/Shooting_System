/* AVE_app.c —— 应用装配实现：板级/串口初始化 + 互斥量与四个任务 */
/* 负责初始化顺序、RTOS 对象创建与失败上报、四个任务骨架；视觉/串口/日志的具体实现在别处 */

#include <stdio.h>        /* 只为 [TIM] 行那句 snprintf，不靠 AVE_log.h 间接带进来 */
#include "AVE_app.h"
#include "AVE_tuning.h"   /* 必须显式引：下面用 `#if AVE_ENABLE_GIMBAL`。漏引时宏未
                           *    定义 ⇒ 按 0 处理 ⇒ 行为恰好一样，但没有任何提示。 */
#include "AVE_board.h"
#include "AVE_log.h"
#include "AVE_uart.h"
#include "AVE_vision.h"   /* 都写出来，不靠间接包含：哪天某个头文件不再包含另一个，
                           * 报错的位置会离真正的原因隔着两层。 */
#include "AVE_gimbal.h"   /* 本文件只负责把互斥量交给它。 */
#include "AVE_stepper.h"  /* [TIM] 自检行，只用到 Init 与 SelfTest 两个函数。 */

/* ============================================================================
 * 一、任务参数 —— 全部取自 AVE_tuning.h，这里不许出现魔数
 * ==========================================================================*/

/* static const 活在 flash 里，不占任务栈。优先级：视觉 High / 电机 AboveNormal /
   命令 Normal / 心跳 Low。心跳最低——它是可有可无的任务，绝不能跟控制抢 CPU */
static const osThreadAttr_t s_vision_attr =
{
  .name       = "VisionTask",
  .stack_size = AVE_TASK_VISION_STACK,
  .priority   = (osPriority_t)osPriorityHigh,
};

static const osThreadAttr_t s_motor_attr =
{
  .name       = "MotorTask",
  .stack_size = AVE_TASK_MOTOR_STACK,
  .priority   = (osPriority_t)osPriorityAboveNormal,
};

static const osThreadAttr_t s_cmd_attr =
{
  .name       = "CmdTask",
  .stack_size = AVE_TASK_CMD_STACK,
  .priority   = (osPriority_t)osPriorityNormal,
};

static const osThreadAttr_t s_heartbeat_attr =
{
  .name       = "HeartbeatTask",
  .stack_size = AVE_TASK_HEARTBEAT_STACK,
  .priority   = (osPriority_t)osPriorityLow,
};

/** 电机总线互斥量。🔴 `osMutexPrioInherit` 不是可选项 —— 见 AVE_app.h。 */
static const osMutexAttr_t s_motor_mutex_attr =
{
  .name      = "MotorMutex",
  .attr_bits = osMutexPrioInherit,
};

/* ============================================================================
 * 二、私有数据
 * ==========================================================================*/

/**
  * 各任务的循环计数。volatile 不是怕读到一半被改（对齐 32 位读写本身原子），
  * 而是禁止编译器把读取提签到循环外——否则心跳永远打同一数字，看着像任务全停了
  */
typedef struct
{
  volatile uint32_t vision;
  volatile uint32_t motor;
  volatile uint32_t cmd;
  volatile uint32_t heartbeat;
} AVE_AppCounters;

static AVE_AppCounters s_counters;

static osMutexId_t s_motor_mutex;

/** 上一次发 `[VS]` 的时刻。放文件作用域是为了让 boot 能清零。 */
static uint32_t s_vs_last_log_ms = 0U;

/** 栈溢出标志。钩子在 freertos.c 的 USER CODE 4 段置位，心跳负责报出来。 */
volatile uint32_t AVE_AppStackOverflow = 0U;

/**
  * 溢出的是哪个任务（FreeRTOS 传进钩子的任务名），未溢出为 NULL。
  * 存指针安全（任务名在 TCB 里、在堆上），但钩子里别访问它指向的字符串以外的东西——
  * 钩子运行时栈已坏，它自己随时可能再崩一次。
  */
const char *volatile AVE_AppStackOverflowTask = 0;

/** 第一个创建失败的 RTOS 对象名。调试器里看一眼就知道是谁没起来。 */
const char *AVE_App_FailName = 0;

/* ============================================================================
 * 三、私有工具
 * ==========================================================================*/

/** 把失败对象名记下来并返回 −1。 */
static int app_fail(const char *name)
{
  AVE_App_FailName = name;
  return -1;
}

/**
  * 创建一个任务；失败记名并返回 −1。
  * 名字从 attr 里读、不另传——传两遍早晚不一致，那样上报的名字查不到任何任务
  */
static int app_create_thread(osThreadFunc_t fn, const osThreadAttr_t *attr)
{
  if (osThreadNew(fn, 0, attr) == 0)
  {
    return app_fail(attr->name);
  }
  return 0;
}

/**
  * 到点发一条 `[VS]`，只在"奇数秒"那一半发，与 `[HB]` 错相。
  * 字段：`st` 断流 / `se` 通变断跳变次数 / `ok` 有效目标帧数 / `dr` 协议错 /
  *      `sp` 行超长丢弃数 / `f` 最新帧 cx,cy,kind。宽度预算见 AVE_Vision_Report()
  */
static void vision_log(uint32_t now)
{
  char body[AVE_LOG_LINE_MAX];
  int  n;

  if ((now % AVE_VISION_VS_LOG_PERIOD_MS) < (AVE_VISION_VS_LOG_PERIOD_MS / 2U))
  {
    return;   /* 落在偶数秒那一半：让给 [HB] */
  }

  /* 窗口有半个周期（1000 毫秒）宽而本任务 20 毫秒一轮，所以这个守卫必需 ——
     否则会连发 50 条。 */
  if ((now - s_vs_last_log_ms) < AVE_VISION_VS_LOG_PERIOD_MS)
  {
    return;
  }
  s_vs_last_log_ms = now;

  /*
   * 刻意不用 n 决定发不发：返回负值表示"宽度保证被破坏"，那正是最需要看到的。
   * snprintf 无论如何都补 '\0'，真截断时 AVE_Log() 会在行尾打 '~'
   */
  n = AVE_Vision_Report(body, (int)sizeof(body), now);
  (void)n;

  (void)AVE_Log("%s", body);
}

/**
  * 目标丢失/视觉断流时的 fail-safe：关激光并闩住。
  * 只关绝不开——目标回来时只解闩不点光，出光是开火逻辑的事
  */
static void vision_failsafe_laser(uint32_t now, uint8_t *held_off)
{
  AVE_VisionFrame f;
  int              lost;

  (void)AVE_Vision_Latest(&f);
  lost = (AVE_Vision_IsStale(now) != 0) || (f.kind != AVE_VISION_KIND_OK);

  if (lost == 0)
  {
    *held_off = 0U;   /* 目标回来：只解闩，不点光 */
    return;
  }
  if (*held_off != 0U)
  {
    return;           /* 已经闩住了：不重复写 GPIO */
  }

  AVE_Laser_Set(0);
  *held_off = 1U;
}

#if AVE_ENABLE_VISION_TRACK
/**
  * 函    数：像素死区（不敏感带）
  * 参    数：e 像素误差（可负）
  * 返 回 值：扣掉死区后的误差；|e| 不超过死区时为 0
  * 注意事项：不是"|e| ≤ TOL 就返回 0、否则返回 e"——那样在边界会产生一个
  *           宽度为 TOL 的输出台阶（8 px 处从 0 跳到 0.8 rpm），现象是云台
  *           在中心附近一顿一顿。这里把死区**减掉**，输出在边界连续。
  */
static int vision_deadzone(int e)
{
  if (e > AVE_VISION_TOL_PX)  { return e - AVE_VISION_TOL_PX; }
  if (e < -AVE_VISION_TOL_PX) { return e + AVE_VISION_TOL_PX; }
  return 0;
}

/** 把一拍转速夹进跟踪限速。两轴各自调用，不是共用同一个符号。 */
static float vision_clamp_rpm(float rpm)
{
  if (rpm > (float)AVE_VISION_TRACK_MAX_RPM)  { return  (float)AVE_VISION_TRACK_MAX_RPM; }
  if (rpm < -(float)AVE_VISION_TRACK_MAX_RPM) { return -(float)AVE_VISION_TRACK_MAX_RPM; }
  return rpm;
}

/**
  * 函    数：视觉闭环一拍 —— 像素误差 → 两轴转速（速度模式，纯 P）
  * 参    数：now 当前毫秒，必须与同一轮 AVE_Vision_Poll() 用的时间戳相同
  * 返 回 值：无
  * 注意事项：
  *   ① **必须在 VisionTask 里调用**。帧的写（publish，视觉任务）和读（Latest，
  *      这里）在同一个任务 ⇒ 不存在跨任务撕裂。把它挪到 MotorTask 会引入
  *      "读的结构体被视觉任务改了一半"的窗口，而那种错误只偶尔表现为云台抖一下，
  *      极难归因。
  *   ② 无目标 / 断流 ⇒ 两轴转速写 0。语义是"目标丢了就停"，**不是保持上一拍**。
  *      保持上一拍会让云台在丢帧期间继续匀速扫，这是最危险的行为。
  *   ③ 只用 P（Ki/Kd 暂不接）：增量式 PID 的输出量纲（rpm 还是脉冲增量）还没定，
  *      先用纯比例把**方向**验对，再谈积分。
  *   ④ 返回值刻意丢弃：EGATE（门没开）/ ERANGE（转速太小）都已经由
  *      AVE_GimbalStats 与 AVE_StepperStats 记账，这里再判一次只是重复。
  *      第一次试车如果看到"云台不动"，先查 gimbal 的 moves_rejected 与
  *      stepper 的 gate_rejects，而不是怀疑这段算术。
  */
/* ——— [TR] 行的数据源：闭环"这一拍想干什么" ———
   为什么必须有（2026-10-20 加）：查 YAW 方向查了三轮，[VS] 只能看到 cx/cy，
   看不到"算出多少 rpm、有没有被门控/量程/忙拒掉、电机实际在跑多少 Hz"。
   有 hz 这一项，方向问题就能一刀切开：
     · 有目标但 hz 恒 0            ⇒ 命令没到后端，看 rc（负值）与 rj 三个拒收计数
     · hz 非 0 而电机纹丝不动      ⇒ 电气/机械（驱动器 ENA、接线、皮带），与符号无关
     · hz 符号与目标位置反着来     ⇒ 符号宏错，改 AVE_tuning.h 那一行
   这几个量只读、只在 [TR] 行里用，不参与任何控制。 */
static int   s_tr_has_target;   /* 本拍是否有可用目标（1=有） */
static int   s_tr_ex;           /* 本拍 ex，像素 */
static int   s_tr_ey;           /* 本拍 ey，像素 */
static float s_tr_rpm_yaw;      /* 本拍**下发**的 YAW 转速（已夹限） */
static float s_tr_rpm_pitch;    /* 本拍下发的 PITCH 转速（已夹限） */
static int   s_tr_rc_yaw;       /* SetSpeedRpm(YAW) 的返回码 */
static int   s_tr_rc_pitch;     /* SetSpeedRpm(PITCH) 的返回码 */

static uint32_t s_tr_age_ms;    /* 本拍所用那一帧的年龄 = now - f.tick_ms，毫秒 */

static void vision_track_step(uint32_t now)
{
  AVE_VisionFrame f;
  int   ex = 0;
  int   ey = 0;
  int   have_frame;
  float rx;
  float ry;

  /* Latest 失败时 f 是栈上的垃圾，不能直接喂给 Deviation —— 否则"垃圾恰好长得像
     目标"时会按一个随机误差下发大转速，现象是"开机瞬间云台自己窜一下"。
     这一条是第七十二次顺手补的，之前是这样写的（(void) 丢掉返回值）：
       (void)AVE_Vision_Latest(&f);
       if ((AVE_Vision_Deviation(&f, &ex, &ey) == 0) || ...) */
  have_frame = AVE_Vision_Latest(&f);
  s_tr_age_ms = (have_frame != 0) ? (now - f.tick_ms) : 0U;

  if ((have_frame == 0) ||
      (AVE_Vision_Deviation(&f, &ex, &ey) == 0) || (AVE_Vision_IsStale(now) != 0))
  {
    s_tr_has_target = 0;
    s_tr_ex  = 0;
    s_tr_ey  = 0;
    s_tr_rpm_yaw   = 0.0f;
    s_tr_rpm_pitch = 0.0f;
    /* 返回码一并记下：无目标时若仍拿到 EBUSY，说明"停"没发出去 —— 那是真故障 */
    s_tr_rc_yaw   = AVE_Gimbal_SetSpeedRpm(AVE_GIMBAL_AXIS_YAW,   0.0f);
    s_tr_rc_pitch = AVE_Gimbal_SetSpeedRpm(AVE_GIMBAL_AXIS_PITCH, 0.0f);
    return;
  }

  /* 符号在这里只乘一次：符号宏是机械安装事实（**截至第七十二次仍未定案**，
     两轮实测见 AVE_tuning.h 处的注释），与 Kp 分开写，改方向不用动增益、改增益不用动方向。 */
  rx = (float)AVE_VISION_YAW_SIGN   * AVE_VISION_YAW_PID_KP   * (float)vision_deadzone(ex);
  ry = (float)AVE_VISION_PITCH_SIGN * AVE_VISION_PITCH_PID_KP * (float)vision_deadzone(ey);

  s_tr_has_target = 1;
  s_tr_ex = ex;
  s_tr_ey = ey;
  s_tr_rpm_yaw   = vision_clamp_rpm(rx);
  s_tr_rpm_pitch = vision_clamp_rpm(ry);

  s_tr_rc_yaw   = AVE_Gimbal_SetSpeedRpm(AVE_GIMBAL_AXIS_YAW,   s_tr_rpm_yaw);
  s_tr_rc_pitch = AVE_Gimbal_SetSpeedRpm(AVE_GIMBAL_AXIS_PITCH, s_tr_rpm_pitch);
}

/**
  * 函    数：诊断行 [TR] —— 闭环这一拍的输入/输出/实际频率/拒收计数
  * 参    数：now 当前毫秒
  * 返 回 值：无
  * 谁调用：VisionTask，紧跟在 vision_log() 之后（同属奇数秒那一半）
  * 注意事项：AVE_Log 是"忙就丢"，两行连着发时第二行可能被丢（drtx 会涨）。
  *           丢了只看下一轮，**不要**据此判断"没执行"。
  *           字段：nv=有无目标；ex/ey=像素误差；
  *           hz=后端**当前实际**带符号频率（÷53.3 就是 rpm，÷8.9 就是 °/秒）；
  *           rc=SetSpeedRpm 返回码；
  *           dc=换向成功累计次数；rj=量程/门控/忙 三种拒收累计；
  *           age=本拍所用那一帧的年龄（毫秒）。**这个字段比 ex 更该先看**：
  *               age 远大于一帧周期（>100 ms）⇒ 控制器在拿旧数据打新靶，
  *               这时候调 Kp 全是白调，先查断流/帧率。
  */
static void vision_track_log(uint32_t now)
{
  static uint32_t last_ms = 0U;
  const AVE_StepperStats *stp = AVE_Stepper_Stats();

  if ((now % AVE_VISION_VS_LOG_PERIOD_MS) < (AVE_VISION_VS_LOG_PERIOD_MS / 2U)) { return; }
  if ((now - last_ms) < AVE_VISION_TR_LOG_PERIOD_MS) { return; }
  last_ms = now;

  /* 🔴 字段顺序按"丢了也最不心疼"排：AVE_LOG_LINE_MAX 只有 100 字节，超了会被
     '~' 截断，截掉的永远是行尾。所以拒收计数（诊断用）放在 dc 之前。 */
  /* dir= 两项已在第七十二次撤掉：用户确认 PA5 接 DIR，且**改符号后物理方向确实跟着反**，
     这本身就证明 DIR 通路是好的，不必再占 8 个字节的行宽；空出来的位置给 age。 */
  (void)AVE_Log("[TR] t=%lu nv=%d ex=%d ey=%d hz=%ld,%ld rj=%lu,%lu,%lu rc=%d,%d dc=%lu age=%lu",
                (unsigned long)now,
                s_tr_has_target,
                s_tr_ex, s_tr_ey,
                (long)AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_YAW),
                (long)AVE_Stepper_GetFreqHz(AVE_STEPPER_AXIS_PITCH),
                (unsigned long)stp->range_rejects,
                (unsigned long)stp->gate_rejects,
                (unsigned long)stp->busy_rejects,
                s_tr_rc_yaw, s_tr_rc_pitch,
                (unsigned long)stp->dir_changes,
                (unsigned long)s_tr_age_ms);
}
#endif /* AVE_ENABLE_VISION_TRACK */


static void heartbeat_log(void)
{
  /* 两个通道都是编译期常量，`AVE_Uart_Stats()` 必然非空（只对越界通道号返回 NULL）。 */
  const AVE_UartStats *cam = AVE_Uart_Stats(AVE_UART_CAM);
  const AVE_UartStats *dbg = AVE_Uart_Stats(AVE_UART_DBG);
  /* 同样永不为 NULL（返回内部静态量地址）⇒ 直接解引用，不必判空 */
  const AVE_StepperStats *stp = AVE_Stepper_Stats();

  /* p 放在行尾：一是不动 `[HB]` 已有的字段顺序（那是验收标准），二是它是
     唯一一个“正常时恒为 0,0”的字段，扫一眼行尾就知道有没有出过脉冲。 */
  (void)AVE_Log("[HB] t=%lu v=%lu m=%lu c=%lu hb=%lu rx=%lu drrx=%lu drtx=%lu ovf=%lu bf=%lu p=%lu,%lu",
                (unsigned long)osKernelGetTickCount(),
                (unsigned long)s_counters.vision,
                (unsigned long)s_counters.motor,
                (unsigned long)s_counters.cmd,
                (unsigned long)s_counters.heartbeat,
                (unsigned long)cam->rx_bytes,
                (unsigned long)cam->drop_bytes,
                (unsigned long)dbg->tx_drop,
                (unsigned long)AVE_AppStackOverflow,
                (unsigned long)AVE_Board_BootFailCode(),
                (unsigned long)stp->pulses_total[AVE_STEPPER_AXIS_YAW],
                (unsigned long)stp->pulses_total[AVE_STEPPER_AXIS_PITCH]);
}

/* ============================================================================
 * 四、任务
 * ==========================================================================*/

/**
  * 视觉任务：解析 MaixCAM 目标坐标行并（可选）驱动两轴，周期 20 毫秒（50 赫兹）。
  * 排空/解析/校验/只留最新帧/更新断流/丢即关激光；
  * AVE_ENABLE_VISION_TRACK == 1 时再加一步：误差 → 转速（纯 P，见 vision_track_step）。
  * 🔴 闭环下发**故意留在这个任务**而不放 MotorTask：读写同一份帧必须在同一个任务，
  *    否则结构体可能被改了一半，现象只是偶尔抖一下，极难查。
  */
static void VisionTask(void *argument)
{
  uint32_t next;                /* 下一轮应该运行的绝对 tick */
  uint32_t now;                 /* 本轮所有判断统一使用的时间戳 */
  uint8_t  laser_held_off = 0U; /* 激光关断闩锁 */

  (void)argument;
  next = osKernelGetTickCount();

  for (;;)
  {
    s_counters.vision++;

    /* 本周期只取一次时刻：分三次深夜会得到三个略有差异的值，日志 `t` 就对不上了——这种
       "差不多对"的数字在排障时最费时间 */
    now = osKernelGetTickCount();

    AVE_Vision_Poll(now);                        /* 排空 → 只发布最新一帧 → 更新断流状态 */
#if AVE_ENABLE_VISION_TRACK
    vision_track_step(now);                      /* 误差 → 两轴转速；无目标/断流则停 */
    vision_track_log(now);                       /* 诊断：[TR] 行：奇数秒那一半，每 AVE_VISION_TR_LOG_PERIOD_MS 一行 */
#endif
    vision_log(now);                             /* 每 2 秒（奇数秒）报一行，与心跳错相 */
    vision_failsafe_laser(now, &laser_held_off); /* 目标丢失 → 关光并闩住 */

#if (AVE_DIAG_BLOCK_VISION_MS > 0)
    /*
     * 诊断探针——正常运行必须为 0（见 AVE_tuning.h 同名宏）。让视觉任务长时间阻塞，
     * 上板验证「一个任务阻塞时其余任务照常跑」
     */
    (void)osDelay(AVE_DIAG_BLOCK_VISION_MS);
#endif

    /* 累加绝对时刻，别写 `osDelayUntil(now + PERIOD)`：后者每轮误差累积，
       周期从 20 漂到 21，增益漂 5%，现象看着只是"PID 参数不太对" */
    next += AVE_TASK_VISION_PERIOD_MS;
    (void)osDelayUntil(next);
  }
}

/**
  * 电机任务：周期 10 毫秒。云台层已接线（AVE_ENABLE_GIMBAL == 1）但还没有活干 ——
 * 视觉下发已搬到 VisionTask（与解析同任务，避免跨任务读帧撕裂）；本任务仍只计数。
  * ⚠️ 别因为"它叫 MotorTask"就把闭环搬进来 —— 那会重新引入撕裂窗口。
  */
static void MotorTask(void *argument)
{
  uint32_t next;

  (void)argument;
  next = osKernelGetTickCount();

  for (;;)
  {
    s_counters.motor++;
    next += AVE_TASK_MOTOR_PERIOD_MS;
    (void)osDelayUntil(next);
  }
}

/** 命令任务：读上位机/云端的指令行，改参数、切模式。周期 10 毫秒。现在只计数。 */
static void CmdTask(void *argument)
{
  uint32_t next;

  (void)argument;
  next = osKernelGetTickCount();

  for (;;)
  {
    s_counters.cmd++;
    next += AVE_TASK_CMD_PERIOD_MS;
    (void)osDelayUntil(next);
  }
}

/** 心跳任务：每 2 秒吐一行 `[HB]`。优先级最低，它是这台机器上最不重要的东西。 */
static void HeartbeatTask(void *argument)
{
  uint32_t next;

  (void)argument;
  next = osKernelGetTickCount();

  for (;;)
  {
    s_counters.heartbeat++;
    heartbeat_log();

    next += AVE_TASK_HEARTBEAT_PERIOD_MS;
    (void)osDelayUntil(next);
  }
}

/* ============================================================================
 * 五、对外接口
 * ==========================================================================*/

/**
  * 函    数：上电第 1 步，板级初始化，按极性把激光置关
  * 参    数：无
  * 返 回 值：无
  * 注意事项：必须最先做——在任何可能出错的事情之前，先让激光是灭的
  */
static void app_boot_safety(void)
{
  AVE_Board_Init();
}

/**
  * 函    数：等调试口上一笔日志发完（有界自旋），专供开机阶段串行化日志
  * 参    数：无
  * 返 回 值：无
  */
static void boot_wait_uart_idle(void)
{
  uint32_t guard = AVE_LOG_BOOT_TXIDLE_SPIN;

  while ((AVE_Uart_TxBusy(AVE_UART_DBG) != 0) && (guard > 0U))
  {
    guard--;
  }
}

/**
  * 函    数：上电第 2 步，启动三路串口逐字节中断接收，并打一条开机 [RST]
  * 参    数：无
  * 返 回 值：无
  */
static void app_boot_uart_and_report(void)
{
  AVE_Uart_Init();
  /* 此刻通道必然是空的（本行是全工程第一条日志），这个等待一次都不会用满。
     写出来是为了让"开机日志一律先等再发"成为一条**不变的规矩**，
     而不是靠下一个人记住"哪几条要等"——第 28 次就是因为没这条规矩白烧了一版。 */
  boot_wait_uart_idle();
  (void)AVE_Log("[RST] cause=%lu", (unsigned long)AVE_Board_ResetCause());
}

/**
  * 函    数：上电第 3 步，视觉跟踪初始化，并把 [VS] 节流时刻一起清零
  * 参    数：无
  * 返 回 值：无
  * 注意事项：必须在调度器启动前调（本函数只清静态状态、不读时间，满足）。
  *           副作用：last_frame_ms 只剩 0，开机约 201 毫秒后进入断流态，直到第一帧到，
  *           正是要的那次"目标丢失安全动作"——视觉未上线时云台该静、激光该关
  */
static void app_boot_vision(void)
{
  AVE_Vision_Init();
  s_vs_last_log_ms = 0U;
}

/**
  * 函    数：上电第 4 步，启动脉冲后端，并打一条 `[TIM]` 把定时器配置读回来对账
  * 参    数：无
  * 返 回 值：无
  */
static void app_boot_stepper(void)
{
  AVE_StepperSelfTest st;
  char body[AVE_LOG_LINE_MAX];
  int  r;

  AVE_Stepper_Init();

  r = AVE_Stepper_SelfTest(&st);
  (void)r;   /* 返回值只用来分支；这里刻意不分支也不丢字段——详情都在 bad 里 */

  (void)snprintf(body, sizeof(body),
                 "[TIM] tb=%lu clk=%lu exppsc=%lu A=%lu,%04lX,%04lX B=%lu,%04lX,%04lX bad=%lX,%lX",
                 (unsigned long)st.timebase_hz,
                 (unsigned long)st.timer_clk_hz,
                 (unsigned long)st.expected_psc,
                 (unsigned long)st.psc[AVE_STEPPER_AXIS_YAW],
                 (unsigned long)st.ccmr2[AVE_STEPPER_AXIS_YAW],
                 (unsigned long)st.ccer[AVE_STEPPER_AXIS_YAW],
                 (unsigned long)st.psc[AVE_STEPPER_AXIS_PITCH],
                 (unsigned long)st.ccmr2[AVE_STEPPER_AXIS_PITCH],
                 (unsigned long)st.ccer[AVE_STEPPER_AXIS_PITCH],
                 (unsigned long)st.bad[AVE_STEPPER_AXIS_YAW],
                 (unsigned long)st.bad[AVE_STEPPER_AXIS_PITCH]);

  boot_wait_uart_idle();
  (void)AVE_Log("%s", body);
}

#if (AVE_DIAG_JOG_PULSES != 0)
static void app_boot_jog(void)
{
  const AVE_StepperAxis   axis = (AVE_StepperAxis)AVE_DIAG_JOG_AXIS;
  const AVE_StepperStats *st   = AVE_Stepper_Stats();
  char     body[AVE_LOG_LINE_MAX];
  uint32_t started0;
  uint32_t mag;
  uint32_t expect_ms;
  uint32_t spin;
  int      rc_gate, rc_move, rc_close;
  int32_t  hz_after;
  int      busy_left;

  /* move_started 是**累计**的（只增不减）。点动前后各取一次，差值才是这一枪的 ——
     直接打读数的话，以后有人加了别的动作，这一枪的日志会跟着变。
     只留这一个统计量：`mv=1` + `b=0` 已经说明"被接受且走完了"，
     `b=1` + `sp=0` 说明"没走完、被我们关门打断"，`mv=0` 说明"压根没被接受"——
     done/abt 两个数加进来只会把行撑宽，而超宽是**静默截断**（本行的宽度是实测卡过的）。 */
  started0 = st->move_started;

  rc_gate  = AVE_Stepper_SetGate(axis, 1U);        /* 坑①：不开门，下面必返回 EGATE */
  rc_move  = AVE_Stepper_MovePulses(axis, (int32_t)AVE_DIAG_JOG_PULSES, AVE_DIAG_JOG_HZ);
  hz_after = AVE_Stepper_GetFreqHz(axis);          /* 被接受才会变成请求值；被拒则保持旧值 */

  /* 有界自旋等它走完。上界 = 期望时长 × 余量系数（系数见 AVE_DIAG_JOG_SPIN_PER_MS）。
     20 步 / 200 赫兹 ⇒ 期望 100 毫秒 ⇒ 上界 2e7 次；走完就提前退出。 */
  mag       = (AVE_DIAG_JOG_PULSES > 0) ? (uint32_t)AVE_DIAG_JOG_PULSES
                                        : (uint32_t)(-AVE_DIAG_JOG_PULSES);
  expect_ms = (mag * 1000UL) / AVE_DIAG_JOG_HZ;
  spin      = expect_ms * AVE_DIAG_JOG_SPIN_PER_MS;
  while ((AVE_Stepper_IsBusy(axis) != 0) && (spin > 0U)) { spin--; }
  busy_left = AVE_Stepper_IsBusy(axis);

  rc_close = AVE_Stepper_SetGate(axis, 0U);        /* 坑④：关门；若还在走，Stop() 会打断它 */

  boot_wait_uart_idle();                           /* 理由同 [TIM] 那一行：不等就永远看不见 */
  (void)snprintf(body, sizeof(body),
                 "[JOG] ax=%lu n=%ld hz=%lu rc=%d,%d,%d f=%ld b=%d sp=%lu mv=%lu",
                 (unsigned long)AVE_DIAG_JOG_AXIS,
                 (long)AVE_DIAG_JOG_PULSES,
                 (unsigned long)AVE_DIAG_JOG_HZ,
                 rc_gate, rc_move, rc_close,
                 (long)hz_after,
                 busy_left, (unsigned long)spin,
                 (unsigned long)(st->move_started - started0));
  (void)AVE_Log("%s", body);
}
#endif

void AVE_App_Init(void)
{
  /* 顺序不能变：安全 → 串口 → 视觉 → 电机，四步全在 `osKernelStart()` 之前跑。 */
  app_boot_safety();
  app_boot_uart_and_report();

#if AVE_ENABLE_GIMBAL
  AVE_Gimbal_Init();
#endif

  app_boot_vision();
  app_boot_stepper();   /* 唯一的位置要求：在 main.c 的 MX_TIM2_Init()/MX_TIM5_Init() 之后 */
#if (AVE_DIAG_JOG_PULSES != 0)
  app_boot_jog();       /* 诊断：上电点动一次。AVE_DIAG_JOG_PULSES == 0 时本行整个不存在 */
#endif

#if AVE_ENABLE_VISION_TRACK
  /* 开两轴软件门控。不开门的现象是"闭环完全不动作"：每一条 SetSpeedRpm 都返回
     EGATE，而串口日志里**没有任何一行主动报错**（只有 stats 的 gate_rejects 在涨）。
     放在 jog 之后 —— jog 收尾会关门，这里保证跟踪模式下门最终是开的。 */
  (void)AVE_Gimbal_SetGateAll(1U);
#endif

  /*
   * 后续步骤随功能补进来：
   *   5. 上电点动 —— 未开始。⚠️ 现在两轴门控全关、一步不动，那是 AVE_Stepper_Init()
   *      的 fail-safe 设计，不是漏调了哪一句；
   *   6. 一次"目标丢失"安全动作 —— 当前只关了激光，两轴不停（脉冲后端刚接上，还没补）；
   *   7. 云端初始化
   */
}

int AVE_App_RtosInit(void)
{
  AVE_App_FailName = 0;

  s_motor_mutex = osMutexNew(&s_motor_mutex_attr);
  if (s_motor_mutex == 0)
  {
    return app_fail("MotorMutex");
  }

  /*
   * 电机互斥量：仍然创建、仍然检查，但【当前没有任何消费者】。
   * 2026-10-06 第 35 次：AVE_Gimbal_SetBusMutex() 连同这个接口一起删了 ——
   * 脉冲后端下两轴各占一个定时器，不存在抢外设这回事，交锁没有对象。
   * 留着创建的三条理由：
   *  ① osMutexNew 失败检查是独立有价值的自检（返回 NULL = FreeRTOS 堆不够，
   *     而它的报错方式是静默的，不查就永远发现不了）；
   *  ② AVE_App_MotorMutex() 将来给"两轴同步启停"这类原子操作用；
   *  ③ 删掉它要连带调 configTOTAL_HEAP_SIZE 的余量，收益为零。
   * 现状如实说：AVE_App_MotorMutex() 当前零调用者 —— 别以为它在保护什么。
   */

  /*
   * 四个任务逐个创建、逐个检查返回值。osThreadNew 失败返回 NULL 不报错，
   * 漏检=某功能永远不工作却看似正常。失败主因是 configTOTAL_HEAP_SIZE 不够，
   * 而报错方式是静默的，所以必须逐个 if
   */
  if (app_create_thread(VisionTask, &s_vision_attr) != 0)
  {
    return -1;
  }
  if (app_create_thread(MotorTask, &s_motor_attr) != 0)
  {
    return -1;
  }
  if (app_create_thread(CmdTask, &s_cmd_attr) != 0)
  {
    return -1;
  }
  if (app_create_thread(HeartbeatTask, &s_heartbeat_attr) != 0)
  {
    return -1;
  }

  return 0;
}

osMutexId_t AVE_App_MotorMutex(void)
{
  return s_motor_mutex;
}
