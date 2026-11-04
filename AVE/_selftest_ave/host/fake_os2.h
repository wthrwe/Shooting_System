/**
 * 仿真 CMSIS-RTOS2 的**注入与探针接口** —— 只有测试主程序用它，产品代码不包含本文件。
 *
 * ── 这里**没有** CMSIS 类型定义，那是刻意的 ──────────────────────────────────
 *    cmsis_os2.h 用的是**工程里那一份真头文件**
 *    （`Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.h`），
 *    由 `-I` 指过去。所以 `osThreadAttr_t` 的字段顺序、`osPriorityHigh` 的数值、
 *    `osMutexPrioInherit` 的位，全都是**目标机上的真值**。
 *
 *    自己抄一份枚举的坏处很具体：抄错一位（比如把 `osMutexPrioInherit` 写成 0x04）
 *    测试照样全绿 —— 因为它测的是"产品代码和我的抄写一致"，而不是"和真值一致"。
 *
 * ── 这个替身**不模拟调度**，只模拟"对象的创建" ───────────────────────────────
 *    它记录每次 `osThreadNew()` 收到的参数供断言，并用 `longjmp` 让任务函数
 *    跑有限的几轮就交回控制权（任务体是死循环，没法正常返回）。
 *
 *    ⚠️ **"视觉任务被阻塞时其他任务照常运行"这条性质测不了** ——
 *       那是 FreeRTOS 调度器的行为，不是本工程代码的行为。
 *       在替身上验证它等于在验证替身自己。这条只能上板看串口助手。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#ifndef HOST_FAKE_OS2_H
#define HOST_FAKE_OS2_H

#include <stdint.h>
#include "cmsis_os2.h"

/* ============================================================================
 * 一、被创建对象的记录
 * ==========================================================================*/

#define HOST_OS2_MAX_THREADS 8
#define HOST_OS2_NAME_MAX    24

typedef struct
{
  char         name[HOST_OS2_NAME_MAX];
  uint32_t     stack_size;
  osPriority_t priority;
  osThreadFunc_t func;      /**< 指向产品代码里的任务函数 —— 测试直接拿它来跑 */
  void        *arg;
  int          attributes_null;   /**< 非 0 表示调用时 attr 传的是 NULL（用了默认值） */
} host_os2_thread_t;

typedef struct
{
  char     name[HOST_OS2_NAME_MAX];
  uint32_t attr_bits;
  int      attributes_null;
} host_os2_mutex_t;

/** 已创建的任务数 / 互斥量数。 */
int host_os2_thread_count(void);
int host_os2_mutex_count(void);

/** 取第 i 个被创建的任务（越界返回 NULL）。 */
const host_os2_thread_t *host_os2_thread(int i);

/** 取第 i 个被创建的互斥量（越界返回 NULL）。 */
const host_os2_mutex_t *host_os2_mutex(int i);

/** 按名字找一个任务，找不到返回 NULL。 */
const host_os2_thread_t *host_os2_find(const char *name);

/* ============================================================================
 * 二、注入失败 —— 模拟 32 KB 堆不够用的那一刻
 * ==========================================================================*/

/**
 * 让**第 n 次**（从 1 数起）`osThreadNew()` 返回 NULL。传 0 取消注入。
 *
 * 这是本套用例里唯一能测出"有没有检查返回值"的手段：
 * `osThreadNew()` 失败时**不报错、不死机**，任务就是不存在 ——
 * 没有这个注入，`if (... == 0) return app_fail(...)` 那几行永远是死代码。
 */
void host_os2_fail_thread_at(int n);

/** 让第 n 次 `osMutexNew()` 返回 NULL。传 0 取消注入。 */
void host_os2_fail_mutex_at(int n);

/**
 * 设定"醒来时刻的抖动"（毫秒），默认 1。
 *
 * 🔴 **这是"周期漂移"这个 bug 能被观测到的前提。** 抖动为 0 时，
 *    「累加绝对时刻」和「每轮用当前 tick 现算」两种写法**结果完全一样**
 *    （因为唤醒时 tick 恰好等于申请值，现算出来就是同一个数），
 *    漂移用例会假通过。真实内核只保证"不早于"该时刻唤醒，所以默认取 1。
 *    见 fake_os2.c 里 `s_wake_jitter` 的完整说明。
 *
 * ⚠️ `host_os2_reset()` 会把它恢复成 1，所以要在复位**之后**再设。
 */
void host_os2_set_wake_jitter(uint32_t ms);

/* ============================================================================
 * 三、跑任务 —— 死循环怎么才能"跑几轮就回来"
 * ==========================================================================*/

/**
 * 让任务函数跑 `cycles` 轮然后返回。
 *
 * 原理：任务体是 `for(;;)`，唯一会调用的阻塞接口是 `osDelayUntil()`。
 * 替身在那里的第 `cycles` 次调用上 `longjmp` 回本函数 —— 于是
 * **"一轮"的定义就是"任务从上次延时走到本次延时"**，正是我们要断言的单位。
 *
 * 返回实际跑完的轮数（正常情况下等于 cycles）。
 *
 * ⚠️ 每轮结尾的 `osDelayUntil()` 参数是**绝对时刻**，替身会把它记下来 ——
 *    用它来验"周期不漂"：累加写法应当得到 PERIOD 的精确整数倍，
 *    而 `osKernelGetTickCount() + PERIOD` 那种写法会因为每轮开销而逐渐漂移。
 */
int host_os2_run_task(osThreadFunc_t func, int cycles);

/** 当前 tick（毫秒）。每次 `osDelayUntil` 会把它推到该时刻。 */
uint32_t host_os2_tick(void);

/**
 * 把 tick 往前推 `ms` —— 模拟"这一轮里任务干了 `ms` 毫秒的活"。
 *
 * 在两次 `host_os2_run_task(func, 1)` 之间调用它，就能构造出
 * "每轮都有固定开销"的工况，专门用来验周期累加写法的抗漂移能力。
 */
void host_os2_advance(uint32_t ms);

/** 记录到的第 i 次 `osDelayUntil` 的绝对时刻参数（越界返回 0）。 */
uint32_t host_os2_deadline(int i);

/** `osDelayUntil` 被调用的总次数。 */
int host_os2_deadline_count(void);

/**
 * `osDelayUntil()` 收到**已经过去**的绝对时刻的次数。
 *
 * 真实内核对这种情况会立刻返回（不延时）—— 表现为任务开始全速空转追赶，
 * 而计数器会疯涨。它是"周期定小了好比任务干不完"的信号。
 */
int host_os2_late_count(void);

/** 每次 `osDelayUntil` 返回的 `osStatus_t` 序列，用于断言"没迟到"。 */
osStatus_t host_os2_last_status(void);

/* ============================================================================
 * 四、互斥量 / 延时 / 内核状态（M2.4 起才需要）
 *
 * 每个动作都会往 `host_trace` 记一笔（`lock` / `unlock` / `osdelay:NN`），
 * 因为 `AVE_gimbal.h` 有一条硬约束是「持锁期间不许睡眠」——
 * **只看计数是判不出它的**：锁 1 次、延时 1 次，两个数都正常，
 * 而延时可能正好发生在锁里面。必须看得见顺序。
 * ==========================================================================*/

/**
 * 设定 `osKernelGetState()` 的返回值。默认 `osKernelInactive`（见 fake_os2.c 的说明）。
 *
 * `AVE_gimbal.c` 用它决定"延时睡 osDelay 还是 HAL_Delay"，两种都要测：
 *   `osKernelInactive` → 上电使能那条路（调度器还没起）
 *   `osKernelRunning`  → 任务里那条路
 */
void host_os2_set_kernel_state(osKernelState_t st);

/**
 * 让**第 n 次** `osMutexAcquire()` 返回 `osErrorTimeout`。传 0 取消注入。
 *
 * 测的是"拿不到锁时**放弃发送**"：不放弃的话两条帧会交错，
 * 而驱动器收到垃圾帧**不会回错误码**（它连帧头都对不上）——
 * 现象是"两轴同时动时偶尔一条指令没反应"，且没有任何计数会涨。
 */
void host_os2_fail_lock_at(int n);

int host_os2_lock_count(void);
int host_os2_unlock_count(void);

/** 当前持有深度。正常只该是 0 或 1；**大于 1 = 有人没放锁**。 */
int host_os2_held_count(void);

/* ============================================================================
 * 五、复位 —— 每个用例开头调一次
 * ==========================================================================*/

void host_os2_reset(void);

#endif /* HOST_FAKE_OS2_H */
