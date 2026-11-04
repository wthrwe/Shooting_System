/**
 * M1.5 运行时自检 —— 装配层（任务创建 + 非阻塞日志）在宿主机上真跑一遍。
 *
 * ── 被测对象是产品代码本身 ──────────────────────────────────────────────────
 *   链接进来的是工程里那几份 **`AVE/AVE_app.c`、`AVE/AVE_log.c`、`AVE/AVE_uart.c`、
 *   `AVE/AVE_board.c`** —— 不复制、不改写。
 *
 *   被替身的是两样东西：
 *     HAL              host/fake_hal.c
 *     CMSIS-RTOS2      host/fake_os2.c
 *
 *   ⚠️ **CMSIS-RTOS2 的类型与常量不是抄的** —— 用的是工程里那份真头文件
 *      （`Middlewares/.../CMSIS_RTOS_V2/cmsis_os2.h`），由 `-I` 指过去。
 *      所以 `osThreadAttr_t` 的字段、`osPriorityHigh` 的数值、
 *      `osMutexPrioInherit` 的位都是目标机上的真值。见 host/fake_os2.h 的说明。
 *
 * ── 覆盖的是 M1.5 验收标准里**不依赖硬件**的部分 ─────────────────────────────
 *   验收①  四个任务真的被创建，参数与 plan.md §4.2 一致          → A1 A2 A3
 *   验收②  创建失败必须能上报（osThreadNew 返回 NULL 不报错）      → A4 A5 A6
 *   验收③  每个任务的周期**精确、不漂**                            → P1 P2
 *   验收④  计数确实在涨，心跳行能报出它们                          → P3 P4
 *   验收⑤  日志不阻塞：发不出去就丢并计数                          → T1 T2
 *   验收⑥  发送缓冲的所有权归本模块（回调数据不被调用方覆盖）      → T3 T4 T5
 *
 *   ⚠️ **测不到**（必须上板）：
 *      · "视觉任务被 osDelay(5000) 卡住时其他任务照常跑" —— 那是 FreeRTOS
 *        调度器的行为，在替身上验证它等于在验证替身自己；
 *      · 心跳真的每 2 秒一行（本文件只验周期参数与任务体逻辑）；
 *      · 栈够不够（512 字节的心跳栈 + vsnprintf 只能在板上见真章）；
 *      · 串口线、波特率、MaixCAM 是否真的在发。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "AVE_config.h"
#include "AVE_app.h"
#include "AVE_log.h"
#include "AVE_uart.h"
#include "AVE_vision.h"
#include "fake_hal.h"
#include "fake_os2.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * 零、编译期检查 —— 与 plan.md §4.2 的任务表对表
 *
 * 写成 typedef 而不是 expect()：**不符合就是编译不过**，
 * 比"跑起来才报 FAIL"早一步，也比人的记性可靠。
 * 右边这些字面量逐条抄自 plan.md §4.2 的任务表。
 * ==========================================================================*/

typedef char chk_plan_vision_period[((AVE_TASK_VISION_PERIOD_MS    ==   20) ? 1 : -1)];
typedef char chk_plan_motor_period [((AVE_TASK_MOTOR_PERIOD_MS     ==   10) ? 1 : -1)];
typedef char chk_plan_cmd_period   [((AVE_TASK_CMD_PERIOD_MS       ==   10) ? 1 : -1)];
typedef char chk_plan_hb_period    [((AVE_TASK_HEARTBEAT_PERIOD_MS == 2000) ? 1 : -1)];

typedef char chk_plan_vision_stack[((AVE_TASK_VISION_STACK    == 1024) ? 1 : -1)];
typedef char chk_plan_motor_stack [((AVE_TASK_MOTOR_STACK     ==  768) ? 1 : -1)];
typedef char chk_plan_cmd_stack   [((AVE_TASK_CMD_STACK       == 1024) ? 1 : -1)];
typedef char chk_plan_hb_stack    [((AVE_TASK_HEARTBEAT_STACK ==  512) ? 1 : -1)];

/** 日志行必须装得进发送缓冲 —— 否则每一条日志都会被 `AVE_Uart_WriteIT` 拒收。 */
typedef char chk_log_fits_tx[((AVE_LOG_LINE_MAX <= AVE_UART_TX_BUF_SIZE) ? 1 : -1)];

/**
 * 🔴 任务栈必须**大于**日志行缓冲。
 *    `AVE_Log()` 在调用者栈上开 `AVE_LOG_LINE_MAX` 字节，外加 vsnprintf 自己的开销。
 *    栈比行缓冲还小 = 一打日志就溢出。
 */
typedef char chk_hb_stack_gt_line[((AVE_TASK_HEARTBEAT_STACK > AVE_LOG_LINE_MAX) ? 1 : -1)];

/* ============================================================================
 * 极简测试框架（与 ave_m13_runtime.c 同款，保持输出格式一致）
 * ==========================================================================*/

static const char *g_id          = "?";
static int         g_test_failed = 0;
static int         g_pass        = 0;
static int         g_fail        = 0;
static int         g_checks      = 0;

static void begin(const char *id)
{
  g_id          = id;
  g_test_failed = 0;
}

static void end(void)
{
  g_checks++;
  if (g_test_failed)
  {
    g_fail++;
    printf("  FAIL [%s]\n", g_id);
  }
  else
  {
    g_pass++;
    printf("  ok   [%s]\n", g_id);
  }
}

static int is_true(const char *what, int cond)
{
  if (!cond)
  {
    printf("       ✗ %s\n", what);
    g_test_failed = 1;
  }
  return cond;
}

static int eq_u32(const char *what, unsigned long got, unsigned long want)
{
  if (got != want)
  {
    printf("       ✗ %s: 实际 %lu，期望 %lu\n", what, got, want);
    g_test_failed = 1;
    return 0;
  }
  return 1;
}

static int eq_i32(const char *what, long got, long want)
{
  if (got != want)
  {
    printf("       ✗ %s: 实际 %ld，期望 %ld\n", what, got, want);
    g_test_failed = 1;
    return 0;
  }
  return 1;
}

static int eq_str(const char *what, const char *got, const char *want)
{
  if ((got == NULL) || (strcmp(got, want) != 0))
  {
    printf("       ✗ %s: 实际 \"%s\"，期望 \"%s\"\n", what,
           (got == NULL) ? "(NULL)" : got, want);
    g_test_failed = 1;
    return 0;
  }
  return 1;
}

/* ============================================================================
 * 公共夹具
 * ==========================================================================*/

/** 复位一切，并完成串口武装 —— 每个用例开头都调它。 */
static void fixture(void)
{
  host_reset_all();
  host_os2_reset();
  AVE_Uart_Init();
}

/** 建好四个任务，并把它们的函数指针取出来。返回 0 表示装配成功。 */
static int build_app(void)
{
  if (AVE_App_RtosInit() != 0)
  {
    printf("       ✗ 装配失败，FailName=%s\n",
           (AVE_App_FailName == NULL) ? "(NULL)" : AVE_App_FailName);
    return -1;
  }
  return 0;
}

/* ============================================================================
 * 一、装配 —— 任务与互斥量的参数
 * ==========================================================================*/

/** 任务表：名字、优先级、栈大小。顺序必须与 AVE_app.c 里 osThreadNew 的顺序一致。 */
typedef struct
{
  const char  *name;
  long         priority;   /* CMSIS-RTOS2 的数值，不是宏 —— 见下面的说明 */
  unsigned long stack;
  unsigned long period;
} expect_task_t;

/*
 * 🔴 优先级写的是**数值**（40 / 32 / 24 / 8），不是 `osPriorityHigh` 之类的宏。
 *
 *    写宏就变成了同义反复：产品代码里写 `osPriorityHigh`，用例里也写 `osPriorityHigh`，
 *    两边一起错也不会失败。写数值才能测出"用错了哪个优先级"——
 *    比如把视觉任务写成 `osPriorityAboveNormal`，这里就会得到 32 ≠ 40。
 *
 *    这些数值取自 CMSIS-RTOS2 官方定义：
 *    osPriorityLow = 8、osPriorityNormal = 24、osPriorityAboveNormal = 32、osPriorityHigh = 40。
 */
static const expect_task_t k_expect[4] =
{
  { "VisionTask",     40, 1024,    20 },
  { "MotorTask",      32,  768,    10 },
  { "CmdTask",        24, 1024,    10 },
  { "HeartbeatTask",   8,  512,  2000 },
};

static void test_A1_tasks_created(void)
{
  int i;

  begin("A1 四个任务按 plan.md §4.2 的参数创建");

  fixture();
  if (build_app() != 0)
  {
    end();
    return;
  }

  eq_i32("创建的任务数", host_os2_thread_count(), 4);

  for (i = 0; i < 4; i++)
  {
    const host_os2_thread_t *t = host_os2_thread(i);

    if (!is_true("任务存在", t != NULL))
    {
      continue;
    }
    eq_str("任务名", t->name, k_expect[i].name);
    eq_u32("栈大小", (unsigned long)t->stack_size, k_expect[i].stack);
    eq_u32("优先级", (unsigned long)t->priority, (unsigned long)k_expect[i].priority);
    is_true("任务函数非空", t->func != NULL);
    is_true("显式传了 attr（没退回默认值）", t->attributes_null == 0);
  }

  end();
}

static void test_A2_priority_order(void)
{
  int i;

  begin("A2 优先级次序正确（视觉 > 电机 > 命令 > 心跳）");

  fixture();
  if (build_app() != 0)
  {
    end();
    return;
  }

  /*
   * 这一条单独测，是因为它比"每个值都对"更要紧：
   * 心跳任务一旦被排到控制任务之前，它打日志就会把控制挤掉 ——
   * 那是"观测行为干扰被观测对象"的经典事故。
   */
  for (i = 0; i + 1 < 4; i++)
  {
    const host_os2_thread_t *a = host_os2_thread(i);
    const host_os2_thread_t *b = host_os2_thread(i + 1);

    if ((a != NULL) && (b != NULL))
    {
      is_true("前一个任务优先级严格高于后一个", a->priority > b->priority);
    }
  }

  end();
}

static void test_A3_mutex_prio_inherit(void)
{
  begin("A3 电机总线互斥量带优先级继承");

  fixture();
  if (build_app() != 0)
  {
    end();
    return;
  }

  eq_i32("创建的互斥量数", host_os2_mutex_count(), 1);

  {
    const host_os2_mutex_t *m = host_os2_mutex(0);
    if (is_true("互斥量存在", m != NULL))
    {
      /*
       * 数值 0x02 抄自 CMSIS-RTOS2 官方头文件（`Middlewares/.../CMSIS_RTOS_V2/cmsis_os2.h`）：
       *   #define osMutexPrioInherit  0x00000002U
       * 写数值而不是宏，理由同 A1 的优先级 —— 否则用例和产品代码一起错也测不出来。
       */
      eq_u32("attr_bits 含 osMutexPrioInherit(0x02)",
             (unsigned long)(m->attr_bits & 0x02U), 0x02U);
      eq_str("互斥量名", m->name, "MotorMutex");
      is_true("显式传了 attr", m->attributes_null == 0);
    }
  }

  is_true("没有调用者时也要能取到它", AVE_App_MotorMutex() != NULL);

  end();
}

static void test_A4_thread_new_failure(void)
{
  int k;

  begin("A4 任一任务创建失败都必须返回 -1 并记下名字");

  /*
   * 🔴 这是本套用例里**唯一**能测出"有没有检查 osThreadNew 返回值"的手段。
   *    `osThreadNew()` 失败时不报错、不死机，任务就是不存在 ——
   *    没有这个注入，`if (... == 0) return app_fail(...)` 那几行永远是死代码。
   */
  for (k = 1; k <= 4; k++)
  {
    int r;

    fixture();
    host_os2_fail_thread_at(k);

    r = AVE_App_RtosInit();

    eq_i32("注入第 k 个任务失败时返回值", (long)r, -1);
    eq_str("记下的失败对象名", AVE_App_FailName, k_expect[k - 1].name);
    eq_i32("失败的任务数", host_os2_thread_count(), k - 1);
  }

  end();
}

static void test_A5_mutex_new_failure(void)
{
  begin("A5 互斥量创建失败必须返回 -1");

  fixture();
  host_os2_fail_mutex_at(1);

  eq_i32("返回值", (long)AVE_App_RtosInit(), -1);
  eq_str("记下的失败对象名", AVE_App_FailName, "MotorMutex");

  /*
   * 互斥量都建不出来就不该再建任务 —— 那只会把问题藏得更深。
   * 所以这里断言"一个任务也没建"。
   */
  eq_i32("失败后不再创建任务", host_os2_thread_count(), 0);

  end();
}

static void test_A6_success_clears_fail_name(void)
{
  begin("A6 装配成功时失败名必须为 NULL");

  fixture();
  eq_i32("返回值", (long)AVE_App_RtosInit(), 0);
  is_true("FailName 被清空（上一次的残留不能留着骗人）",
          AVE_App_FailName == NULL);

  /*
   * 再失败一次，确认它确实会被重新写上（而不是永远停在 NULL）。
   * 🔴 这里必须**先复位**：注入是按"第几次调用"计数的，
   *    上面那次成功的装配已经用掉了 4 次调用。不复位的话注入永远不命中，
   *    用例会假通过 —— 而"假通过"比"没测"更危险。
   */
  host_os2_reset();
  host_os2_fail_thread_at(1);
  eq_i32("第二次装配失败", (long)AVE_App_RtosInit(), -1);
  eq_str("重新失败后又被写上", AVE_App_FailName, "VisionTask");

  end();
}

/* ============================================================================
 * 二、周期 —— 绝对时刻必须精确，不能逐轮漂
 * ==========================================================================*/

static void test_P1_period_no_drift(void)
{
  int i;

  begin("P1 四个任务的唤醒时刻正好是周期的整数倍（不漂）");

  /* 注入一个零开销的假任务？不需要 —— 直接用真任务的函数指针跑 */
  for (i = 0; i < 4; i++)
  {
    const host_os2_thread_t *t;
    int                      n;
    int                      k;
    int                      drift_at = -1;

    fixture();
    host_os2_set_wake_jitter(1);
    if (build_app() != 0)
    {
      end();
      return;
    }

    t = host_os2_find(k_expect[i].name);
    if (!is_true("找得到任务函数", t != NULL))
    {
      continue;
    }

    n = host_os2_run_task(t->func, 20);
    eq_i32("跑满 20 轮", (long)n, 20);

    for (k = 0; k < 20; k++)
    {
      unsigned long want = (unsigned long)(k + 1) * k_expect[i].period;
      unsigned long got  = (unsigned long)host_os2_deadline(k);

      if ((got != want) && (drift_at < 0))
      {
        drift_at = k;
        printf("       ✗ %s 第 %d 轮的唤醒时刻是 %lu，应为 %lu（每轮偏移 %ld）\n",
               k_expect[i].name, k + 1, got, want,
               (long)got - (long)want);
        g_test_failed = 1;
      }
    }
    eq_i32("没有一轮迟到（迟到=任务干不完，会全速空转）", (long)host_os2_late_count(), 0);
  }

  end();
}

static void test_P2_late_start_still_exact_intervals(void)
{
  int i;

  begin("P2 任务起步晚了 5 毫秒时，相邻唤醒间隔仍精确等于周期");

  /*
   * P1 验的是"从 0 开始跑，绝对时刻 = N × 周期"；
   * 本用例把**起点挪到 5 毫秒**，再去验**间隔**。
   *
   * 这两条为什么都值得测：周期漂移的两种表现不一样 ——
   *   起点对齐时，漂移表现为"绝对时刻不再是周期的整数倍"（P1）；
   *   起点不对齐时，漂移表现为"间隔一轮比一轮大"（P2）。
   * 只测一种的话，另一种就被漏掉了。
   *
   * ⚠️ 若替身的唤醒抖动被设成 0，两种写法会变得完全一样，
   *    本用例和 P1 都会假通过。见 host/fake_os2.h 里 `host_os2_set_wake_jitter`。
   */
  for (i = 0; i < 4; i++)
  {
    const host_os2_thread_t *t;
    int                      k;
    int                      n;

    fixture();
    host_os2_set_wake_jitter(1);
    if (build_app() != 0)
    {
      end();
      return;
    }

    t = host_os2_find(k_expect[i].name);
    if (!is_true("找得到任务函数", t != NULL))
    {
      continue;
    }

    /* 任务还没开始，tick 先走掉 5 毫秒 —— 等价于它起步就晚了 */
    host_os2_advance(5U);

    n = host_os2_run_task(t->func, 10);
    eq_i32("跑满 10 轮", (long)n, 10);

    for (k = 1; k < 10; k++)
    {
      unsigned long a = (unsigned long)host_os2_deadline(k - 1);
      unsigned long b = (unsigned long)host_os2_deadline(k);

      if (!eq_u32("相邻唤醒间隔", b - a, k_expect[i].period))
      {
        printf("       （%s 的第 %d 轮：%lu → %lu）\n",
               k_expect[i].name, k + 1, a, b);
        break;
      }
    }
  }

  end();
}

/* ============================================================================
 * 三、计数器与心跳行
 * ==========================================================================*/

static void test_P3_counters_advance(void)
{
  begin("P3 每个任务每轮把计数加一");

  fixture();
  if (build_app() != 0)
  {
    end();
    return;
  }

  {
    const host_os2_thread_t *v = host_os2_find("VisionTask");
    const host_os2_thread_t *m = host_os2_find("MotorTask");
    const host_os2_thread_t *c = host_os2_find("CmdTask");
    const host_os2_thread_t *h = host_os2_find("HeartbeatTask");

    if (!is_true("四个任务齐了", (v && m && c && h)))
    {
      end();
      return;
    }

    /*
     * 心跳任务会调 AVE_Log，而发送是"启动即返回"的 ——
     * 所以每跑一段必须把上一笔发送"完成"掉，否则下一段的日志会因忙被丢。
     * 这里就用 host_uart_tx_finish 模拟硬件发完。
     *
     * 🔴 视觉任务现在**也会发日志**（`[VS]` 行），所以这三句不是可有可无的：
     *    少了它们，最后那句 `AVE_Uart_TxBusy(AVE_UART_DBG) == 1` 有可能是
     *    因为**视觉任务留下的**在途发送而通过的 —— 结论对、原因错。
     *    本用例只跑到几百毫秒，而视觉的日志窗口 t=3000 才打开，所以这一版
     *    其实还撞不上；但"撞不上"是当前周期参数的巧合，不是这条用例保证的
     *    性质 —— 周期一改就变成假绿。
     */
    eq_i32("视觉跑 7 轮", (long)host_os2_run_task(v->func, 7), 7);
    (void)host_uart_tx_finish(&huart3);
    eq_i32("电机跑 13 轮", (long)host_os2_run_task(m->func, 13), 13);
    (void)host_uart_tx_finish(&huart3);
    eq_i32("命令跑 5 轮", (long)host_os2_run_task(c->func, 5), 5);
    (void)host_uart_tx_finish(&huart3);
    eq_i32("心跳跑 1 轮", (long)host_os2_run_task(h->func, 1), 1);

    /* 数值本身在 P4 里从心跳行的文本中反读回来 —— 这里只验"确实在发" */
    is_true("心跳确实启动了一笔发送", AVE_Uart_TxBusy(AVE_UART_DBG) == 1);
  }

  end();
}

/** 跑一轮心跳，并把发出去的那一行取回来。返回行长；-1 表示没发出去。 */
static int run_heartbeat_capture(char *out, int cap)
{
  const host_os2_thread_t *h = host_os2_find("HeartbeatTask");
  const uint8_t           *data;
  int                      len = 0;

  if (h == NULL)
  {
    return -1;
  }

  host_tx_reset(&huart3);
  if (host_os2_run_task(h->func, 1) != 1)
  {
    return -1;
  }
  if (host_uart_tx_finish(&huart3) != 0)
  {
    return -1;      /* 没有在途发送 —— 说明心跳根本没发东西 */
  }

  data = host_tx_data(&huart3, &len);
  if ((data == NULL) || (len <= 0) || (len >= cap))
  {
    return -1;
  }

  memcpy(out, data, (size_t)len);
  out[len] = '\0';
  return len;
}

/** 从心跳行里反读出来的字段。 */
typedef struct
{
  unsigned long t, v, m, c, hb, rx, drrx, drtx, ovf;
} hb_line_t;

/** 跑一轮心跳，把那一行取回来并解析。返回 0 成功。 */
static int heartbeat_take(hb_line_t *o)
{
  char line[256];
  int  len = run_heartbeat_capture(line, (int)sizeof(line));

  if (len <= 0)
  {
    if (len >= (int)sizeof(line))
    {
      printf("       （行太长，%d 字节，超过本用例的 %d）\n", len, (int)sizeof(line));
    }
    return -1;
  }

  if (!is_true("行以 \\r\\n 结尾",
               (len >= 2) && (line[len - 2] == '\r') && (line[len - 1] == '\n')))
  {
    return -1;
  }
  if (!is_true("行内没有截断标记 '~'", (len >= 3) && (line[len - 3] != '~')))
  {
    return -1;
  }

  /* 逐字段反读 —— 这就是串口助手里会看到的那一行 */
  if (!is_true("九个字段都能解析",
               sscanf(line,
                      "[HB] t=%lu v=%lu m=%lu c=%lu hb=%lu rx=%lu drrx=%lu drtx=%lu ovf=%lu",
                      &o->t, &o->v, &o->m, &o->c, &o->hb,
                      &o->rx, &o->drrx, &o->drtx, &o->ovf) == 9))
  {
    printf("       （实际内容：%s）\n", line);
    return -1;
  }

  return 0;
}

static void test_P4_heartbeat_line(void)
{
  begin("P4 心跳行：格式正确，且三个任务的计数都在涨");

  fixture();
  if (build_app() != 0)
  {
    end();
    return;
  }

  {
    const host_os2_thread_t *v = host_os2_find("VisionTask");
    const host_os2_thread_t *m = host_os2_find("MotorTask");
    const host_os2_thread_t *c = host_os2_find("CmdTask");
    hb_line_t                a;
    hb_line_t                b;

    if (!is_true("四个任务齐了", (v && m && c)))
    {
      end();
      return;
    }

    /*
     * 🔴 **先取一条基线，再比增量。**
     *
     *    计数是 AVE_app.c 里的静态变量，**不会在用例之间复位** ——
     *    直接断言"v == 7"会依赖本用例前面跑过多少轮，脆得一碰就碎。
     *    而"计数在涨"本来就是**增量**命题（ROADMAP M1.5 的原话是
     *    "三个任务的计数**都在涨**"），所以用增量判据才是对准了目标。
     */
    if (heartbeat_take(&a) != 0)
    {
      end();
      return;
    }

    /* 给视觉口灌 8 个字节 —— 验证 rx 也真的在涨，而不是恒为 0 */
    eq_i32("灌进视觉口的字节数", (long)host_uart_inject_str(&huart1, "100,200\n", 8), 8);

    (void)host_os2_run_task(v->func, 7);
    (void)host_os2_run_task(m->func, 13);
    (void)host_os2_run_task(c->func, 5);

    if (heartbeat_take(&b) != 0)
    {
      end();
      return;
    }

    eq_u32("视觉计数涨了 7",  b.v - a.v,   7UL);
    eq_u32("电机计数涨了 13", b.m - a.m,  13UL);
    eq_u32("命令计数涨了 5",  b.c - a.c,   5UL);
    eq_u32("心跳计数涨了 1",  b.hb - a.hb, 1UL);
    eq_u32("视觉口收到字节涨了 8", b.rx - a.rx, 8UL);

    eq_u32("视觉口没有丢字节 drrx", b.drrx, 0UL);
    eq_u32("日志没有被丢 drtx",     b.drtx, 0UL);
    eq_u32("没有栈溢出 ovf",        b.ovf,  0UL);
    is_true("tick 在前进", b.t > a.t);

    /*
     * tick 前进量应该正好是这一段的周期开销：
     * 基线那一轮心跳醒来 → 心跳 2000 毫秒 → 后又跑了一轮心跳。
     * 只要求它 >= 2000 就够 —— 写死具体值会把用例绑死在替身的抖动参数上。
     */
    is_true("tick 至少前进了 2000（心跳周期）", (b.t - a.t) >= 2000UL);
  }

  end();
}

/**
 * 跑到下一行 `[VS]` 发出来并把它整行取回（不含结尾零）。返回行长；-1 表示失败。
 *
 * 🔴 **刻意不写死"跑多少轮"。** `[VS]` 行有两道门槛：必须落在
 *    `now % 2000 >= 1000` 那个窗口（奇数秒）里，且距上次发送 ≥ 2000 毫秒。
 *    写成"跑 151 轮"就把用例绑死在周期参数上了，周期一改就报假警。
 *    这里改成"跑到发出来为止"，判据始终是"发出来了没有"。
 */
static int vs_capture(char *out, int cap, const host_os2_thread_t *v)
{
  const uint8_t *data;
  int            len = 0;
  int            k;

  host_tx_reset(&huart3);
  for (k = 0; k < 400; k++)
  {
    (void)host_os2_run_task(v->func, 1);

    if (AVE_Uart_TxBusy(AVE_UART_DBG) != 1)
    {
      continue;         /* 这一轮没发东西，接着跑 */
    }

    if (host_uart_tx_finish(&huart3) != 0)
    {
      return -1;
    }
    data = host_tx_data(&huart3, &len);
    if ((data == NULL) || (len <= 0) || (len >= cap))
    {
      return -1;
    }

    memcpy(out, data, (size_t)len);
    out[len] = '\0';
    return len;
  }

  return -1;            /* 400 轮（默认周期下 8 秒）都没等到 —— 窗口根本不打开 */
}

/** 从 `[VS]` 行里反读出来的字段。 */
typedef struct
{
  unsigned long t, st, se, ok, dr, sp;
  int           cx, cy, kind;   /* 与产品代码里的 `%d` 对齐，别图省事写成 long */
} vs_line_t;

/** 跑到下一行 `[VS]` 并解析。返回 0 成功。 */
static int vs_take(vs_line_t *o, const host_os2_thread_t *v)
{
  char line[256];
  int  len = vs_capture(line, (int)sizeof(line), v);

  if (len <= 0)
  {
    return -1;
  }
  if (!is_true("行以 \\r\\n 结尾",
               (len >= 2) && (line[len - 2] == '\r') && (line[len - 1] == '\n')))
  {
    return -1;
  }
  if (!is_true("行内没有截断标记 '~'", (len >= 3) && (line[len - 3] != '~')))
  {
    return -1;
  }
  if (!is_true("开头是 '[VS] '", strncmp(line, "[VS] ", 5) == 0))
  {
    return -1;
  }
  if (!is_true("九个字段都能解析",
               sscanf(line,
                      "[VS] t=%lu st=%lu se=%lu ok=%lu dr=%lu sp=%lu f=%d,%d,%d",
                      &o->t, &o->st, &o->se, &o->ok, &o->dr, &o->sp,
                      &o->cx, &o->cy, &o->kind) == 9))
  {
    printf("       （实际内容：%s）\n", line);
    return -1;
  }

  return 0;
}

/**
 * P5 视觉层**接线**：`VisionTask` 真的在消费视觉口，且把 `[VS]` 行发出来。
 *
 * ── 这条用例在防什么（它和 m13 的 V 组不重复）────────────────────────────────
 *   `AVE_vision.c` 的语义在 m13 里已经由 V1~V12 逐条验过，这里**不重复**验解析。
 *   这里验的是**接线**：任务里那句 `AVE_Vision_Poll(now)` 还在不在。
 *
 *   把那一行删掉，m13 的 V 组**依然全过** —— 那是纯模块测试，不经过任务。
 *   只有这条用例能发现"模块写好了但没人调用"这个失败模式，
 *   而它恰恰是最像"成功"的失败：代码全在、编译无警告、用例全绿、板子上什么都不发生。
 *
 * ── 为什么全程比**增量**（和 P4 同一个理由）──────────────────────────────────
 *   本替身**没有**调用 `AVE_App_Init()`（那是 `main.c` 在调度器启动前调的，
 *   而宿主机不启动调度器），所以 `AVE_Vision_Init()` 从未运行 ——
 *   视觉层的计数器是**跨用例累积**的，`ok`/`dr` 的绝对值取决于前面跑过什么。
 *   断言增量才是对准了命题，也才不随用例顺序碎掉。
 */
static void test_P5_vision_line(void)
{
  begin("P5 [VS] 行：VisionTask 真的在消费视觉口");

  fixture();
  if (build_app() != 0)
  {
    end();
    return;
  }

  {
    const host_os2_thread_t *v = host_os2_find("VisionTask");
    vs_line_t                a;
    vs_line_t                b;

    if (!is_true("VisionTask 在", v != NULL))
    {
      end();
      return;
    }

    /* 基线：先让第一条 [VS] 行发出来 —— 此时视觉口还什么都没灌 */
    if (vs_take(&a, v) != 0)
    {
      is_true("400 轮之内取到基线 [VS] 行（错相窗口真的存在）", 0);
      end();
      return;
    }

    /*
     * 灌一帧 MaixCAM 真正会发的字节。**三个字段，缺一不可** ——
     * 文法要求 `cx,cy,flag`；少写 flag 的话这一行是**协议错**，
     * 而"协议错"和"没人消费"在计数上只差一个数字，很容易看错。
     */
    eq_i32("灌进视觉口 10 字节",
           (long)host_uart_inject_str(&huart1, "400,100,1\n", 10), 10);

    if (vs_take(&b, v) != 0)
    {
      end();
      return;
    }

    eq_u32("视觉口消费到 1 帧（说明 Poll 真的被调用了）", b.ok - a.ok, 1UL);
    eq_u32("没有多出协议错", b.dr - a.dr, 0UL);
    eq_u32("没有超长行", b.sp - a.sp, 0UL);
    eq_i32("横坐标就是灌进去的那一帧", b.cx, 400L);
    eq_i32("纵坐标", b.cy, 100L);
    eq_i32("kind 是 OK", b.kind, (long)AVE_VISION_KIND_OK);
    is_true("两行的 t 都落在奇数秒窗口内（与 [HB] 错相）",
            ((a.t % AVE_VISION_VS_LOG_PERIOD_MS) >= (AVE_VISION_VS_LOG_PERIOD_MS / 2U)) &&
            ((b.t % AVE_VISION_VS_LOG_PERIOD_MS) >= (AVE_VISION_VS_LOG_PERIOD_MS / 2U)));
    is_true("第二行确实晚了一整个周期（不是同一行读了两遍）", b.t >= (a.t + 2000UL));
  }

  end();
}

/* ============================================================================
 * 四、非阻塞日志
 * ==========================================================================*/

static void test_T1_busy_drops_but_counts(void)
{
  begin("T1 上一笔没发完 → 拒绝并计数（不阻塞、不排队）");

  fixture();

  {
    int r1 = AVE_Log("first");
    int r2 = AVE_Log("second");

    is_true("第一笔被受理", r1 > 0);
    eq_i32("受理后处于忙", (long)AVE_Uart_TxBusy(AVE_UART_DBG), 1);
    eq_i32("第二笔被拒", (long)r2, (long)AVE_UART_EBUSY);
    eq_u32("tx_drop 记了一笔", (unsigned long)AVE_Uart_Stats(AVE_UART_DBG)->tx_drop, 1UL);

    /* 被拒的那一笔**一个字节都不能出去** —— 半条日志比没有日志更误导 */
    host_uart_tx_finish(&huart3);
    {
      int            len  = 0;
      const uint8_t *data = host_tx_data(&huart3, &len);
      is_true("线上只有第一条", (data != NULL) && (len == 7) && (memcmp(data, "first\r\n", 7) == 0));
    }
  }

  end();
}

static void test_T2_completion_clears_busy(void)
{
  begin("T2 发送完成回调必须清掉忙标志（否则日志永久沉默）");

  fixture();

  {
    int r1 = AVE_Log("a");
    int r3;

    is_true("第一笔被受理", r1 > 0);
    eq_i32("完成后忙标志归零", (long)host_uart_tx_finish(&huart3), 0);
    eq_i32("TxBusy 已清", (long)AVE_Uart_TxBusy(AVE_UART_DBG), 0);

    r3 = AVE_Log("b");
    is_true("发完之后能再发下一条", r3 > 0);
  }

  end();
}

static void test_T3_caller_buffer_ownership(void)
{
  begin("T3 数据先拷进模块自己的缓冲 —— 调用方可以马上改自己的内存");

  fixture();

  {
    char mine[32];
    int  r;

    strcpy(mine, "hello");
    r = AVE_Uart_WriteIT(AVE_UART_DBG, (const uint8_t *)mine, 5);
    is_true("受理", r == 5);

    /*
     * 🔴 **启动发送之后立刻覆盖调用方的缓冲。**
     *    `HAL_UART_Transmit_IT()` 只记指针，真正的搬运发生在之后的"中断"里。
     *    如果产品代码没有先拷一份（直接把 `buf` 交给 HAL），
     *    这里覆盖掉之后，线上发出去的就是 "XXXXX"。
     *
     *    这个 bug 在真机上的现象是"偶尔收到一条内容错乱的上报"，
     *    极难联想到发送缓冲 —— 所以它值得一条专门的用例。
     */
    strcpy(mine, "XXXXX");

    eq_i32("完成一笔发送", (long)host_uart_tx_finish(&huart3), 0);

    {
      int            len  = 0;
      const uint8_t *data = host_tx_data(&huart3, &len);

      is_true("线上内容仍是启动发送时的内容",
              (data != NULL) && (len == 5) && (memcmp(data, "hello", 5) == 0));
    }
  }

  end();
}

static void test_T4_start_failure_clears_busy(void)
{
  begin("T4 发送启动失败也要把忙标志清回去（否则这一路永久报废）");

  fixture();

  {
    /* 手工把 HAL 侧搞成"忙"：等价于 TxState 卡在 BUSY_TX 的真实故障 */
    const uint8_t dummy = 0x55U;
    int           r;

    is_true("先让 HAL 侧处于忙",
            HAL_UART_Transmit_IT(&huart3, &dummy, 1U) == HAL_OK);

    r = AVE_Log("stuck");
    eq_i32("启动失败返回超时", (long)r, (long)AVE_UART_ETIMEOUT);
    eq_i32("忙标志必须被清回去（不能永远卡在忙）",
           (long)AVE_Uart_TxBusy(AVE_UART_DBG), 0);
    eq_u32("tx_err 记了一笔", (unsigned long)AVE_Uart_Stats(AVE_UART_DBG)->tx_err, 1UL);

    /* 把手工那笔收尾，确认之后能正常发 */
    (void)host_uart_tx_finish(&huart3);
    is_true("恢复后能正常发", AVE_Log("ok") > 0);
  }

  end();
}

static void test_T5_error_callback_clears_tx(void)
{
  begin("T5 接收错误时顺手清发送方向（错误回调是收发共用的）");

  fixture();

  {
    is_true("先启动一笔发送", AVE_Log("inflight") > 0);
    eq_i32("在途", (long)AVE_Uart_TxBusy(AVE_UART_DBG), 1);

    /*
     * 模拟一次**接收**错误（过载）。错误回调是收发共用的 ——
     * 如果它只清接收侧，HAL 的 gState 会永远停在 BUSY_TX，
     * 之后每一条日志都撞 HAL_BUSY，表现为"日志永久沉默"。
     */
    host_raise_error(&huart3, HAL_UART_ERROR_ORE, 1);

    eq_i32("错误后忙标志被清", (long)AVE_Uart_TxBusy(AVE_UART_DBG), 0);
    eq_u32("HAL 侧没有卡在忙（本该被 AbortTransmit 清掉）",
           host_tx_busy_count, 0UL);
    is_true("错误之后日志还能恢复", AVE_Log("recovered") > 0);
  }

  end();
}

static void test_T6_line_truncation(void)
{
  begin("T6 超长日志被截断并标记，且不越界、不超过发送缓冲");

  fixture();

  {
    char filler[400];
    int  r;
    int  i;

    for (i = 0; i < 399; i++)
    {
      filler[i] = (char)('a' + (i % 26));
    }
    filler[399] = '\0';

    r = AVE_Log("X%s", filler);
    is_true("被受理（截断后仍放得进发送缓冲）", r > 0);

    eq_i32("完成发送", (long)host_uart_tx_finish(&huart3), 0);

    {
      int            len  = 0;
      const uint8_t *data = host_tx_data(&huart3, &len);

      if (is_true("抓到了内容", (data != NULL) && (len > 0)))
      {
        eq_i32("行长正好是 AVE_LOG_LINE_MAX", (long)len, (long)AVE_LOG_LINE_MAX);
        is_true("以 \\r\\n 结尾",
                (len >= 2) && (data[len - 2] == '\r') && (data[len - 1] == '\n'));
        /*
         * 截断必须**看得见**。悄悄切掉一半的日志比没有日志更危险：
         * `tick=200` 其实是 `tick=2000`，据此得出的结论全是错的。
         */
        eq_i32("倒数第三个字节是截断标记 '~'", (long)data[len - 3], (long)'~');
      }
    }
  }

  end();
}

static void test_T7_argument_validation(void)
{
  begin("T7 参数校验：空格式串与空指针不得发出任何字节");

  fixture();

  {
    const char *nullfmt = NULL;
    int         r;

    r = AVE_Log(nullfmt);
    eq_i32("空格式串被拒", (long)r, (long)AVE_UART_EBADCH);
    eq_i32("没有启动任何发送", (long)AVE_Uart_TxBusy(AVE_UART_DBG), 0);

    host_uart_tx_finish(&huart3);
    {
      int len = 0;
      (void)host_tx_data(&huart3, &len);
      eq_i32("线上一个字节都没有", (long)len, 0);
    }

    /* 空字符串是合法的（打一个空行），不该被拒 */
    r = AVE_Log("");
    is_true("空字符串是合法的空行", r > 0);
  }

  end();
}

/* ============================================================================
 * 五、入口
 * ==========================================================================*/

int main(void)
{
  printf("=== M1.5 运行时自检（宿主机 / 仿真 HAL + 仿真 CMSIS-RTOS2）===\n");
  printf("编译期配置：LOG_LINE_MAX=%d  TX_BUF_SIZE=%d  RX_BUF_SIZE=%d\n",
         (int)AVE_LOG_LINE_MAX, (int)AVE_UART_TX_BUF_SIZE, (int)AVE_UART_RX_BUF_SIZE);
  printf("任务周期：%d / %d / %d / %d 毫秒\n\n",
         (int)AVE_TASK_VISION_PERIOD_MS, (int)AVE_TASK_MOTOR_PERIOD_MS,
         (int)AVE_TASK_CMD_PERIOD_MS, (int)AVE_TASK_HEARTBEAT_PERIOD_MS);

  printf("-- 装配 --\n");
  test_A1_tasks_created();
  test_A2_priority_order();
  test_A3_mutex_prio_inherit();
  test_A4_thread_new_failure();
  test_A5_mutex_new_failure();
  test_A6_success_clears_fail_name();

  printf("-- 周期 --\n");
  test_P1_period_no_drift();
  test_P2_late_start_still_exact_intervals();

  printf("-- 计数与心跳 --\n");
  test_P3_counters_advance();
  test_P4_heartbeat_line();
  test_P5_vision_line();

  printf("-- 非阻塞日志 --\n");
  test_T1_busy_drops_but_counts();
  test_T2_completion_clears_busy();
  test_T3_caller_buffer_ownership();
  test_T4_start_failure_clears_busy();
  test_T5_error_callback_clears_tx();
  test_T6_line_truncation();
  test_T7_argument_validation();

  printf("\n=== 用例 %d 通过 / %d 失败 ===\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
