/**
 * 仿真 CMSIS-RTOS2 实现 —— 只实现本工程真正调用的那四个接口。
 *
 * 未实现的接口（`osKernelStart`、`osSemaphoreNew`、…）**不提供定义**：
 * 一旦产品代码里有人用了它们，链接会直接报 undefined reference ——
 * 这正是我们想要的行为（说明它不再是"只用标准门面"了）。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "fake_os2.h"
#include "fake_hal.h"      /* host_uart_tx_progress —— 时间流逝会让在途发送完成 */
#include "host_trace.h"

#include <setjmp.h>
#include <string.h>

/* ============================================================================
 * 一、记录表
 * ==========================================================================*/

static host_os2_thread_t s_thread[HOST_OS2_MAX_THREADS];
static int               s_thread_n;

static host_os2_mutex_t  s_mutex[HOST_OS2_MAX_THREADS];
static int               s_mutex_n;

/*
 * 注入计数用**调用次数**，不是"表里已有的个数"。
 *
 * 🔴 差别是实的：`AVE_App_RtosInit()` 可能被调用多次（自检里就会），
 *    用表下标的话，第二次调用时 `s_thread_n` 已经是 4 —— 注入"第 1 次失败"
 *    就永远命中不了，用例会**假通过**（看起来"注入了也照样成功"，
 *    于是误判成"产品代码没检查返回值也没关系"）。
 */
static int s_fail_thread_at;
static int s_thread_calls;
static int s_fail_mutex_at;
static int s_mutex_calls;

/* ============================================================================
 * 二、时间与"跑几轮就回来"
 * ==========================================================================*/

#define HOST_OS2_DEADLINE_MAX 256

static jmp_buf  s_jb;
static int      s_jb_armed;      /**< 非 0 表示当前处在 host_os2_run_task 的保护区里 */
static int      s_cycles_left;

static uint32_t s_tick;
static uint32_t s_deadline[HOST_OS2_DEADLINE_MAX];
static int      s_deadline_n;

static int        s_late_n;
static osStatus_t s_last_status;

/**
 * 醒来时刻的抖动（毫秒）。
 *
 * 🔴 **这个量不是装饰，它是"周期漂移"这个 bug 唯一能被观测到的原因。**
 *
 *    假如任务被唤醒的那一刻 tick 恰好等于它申请的绝对时刻，那么
 *    「累加写法」`next += P` 和「每轮现算」`now + P` **结果完全相同** ——
 *    因为 `now` 正好就是 `next`。两者的差别被彻底掩盖，测试全绿。
 *
 *    真实内核不是这样：`vTaskDelayUntil()` 只保证"**不早于**该时刻唤醒"。
 *    唤醒之后还要等调度器切进来，低优先级任务可能被压后一个甚至几个 tick。
 *    于是 `now` 通常等于 `deadline + 若干毫秒`，每轮多出来的那点误差
 *    在「每轮现算」的写法里**逐轮累积** —— 100 轮后周期就从 20 变成 21 毫秒。
 *
 *    所以替身按最保守的方式取 1 毫秒。取 0 也能跑（`host_os2_set_wake_jitter(0)`），
 *    但那时漂移用例会"假通过"——正好可以用来说明这一条。
 */
static uint32_t s_wake_jitter = 1U;

static void copy_name(char *dst, const char *src)
{
  int i;

  if (src == NULL)
  {
    dst[0] = '\0';
    return;
  }
  for (i = 0; (i < (HOST_OS2_NAME_MAX - 1)) && (src[i] != '\0'); i++)
  {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

/* ============================================================================
 * 三、CMSIS-RTOS2 接口
 * ==========================================================================*/

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
  host_os2_thread_t *t;

  if ((func == NULL) || (s_thread_n >= HOST_OS2_MAX_THREADS))
  {
    return NULL;
  }

  s_thread_calls++;

  /* 注入：模拟"堆不够了，这一笔创建失败" —— 真实内核此时只是返回 NULL */
  if ((s_fail_thread_at != 0) && (s_thread_calls == s_fail_thread_at))
  {
    return NULL;
  }

  t        = &s_thread[s_thread_n++];
  t->func  = func;
  t->arg   = argument;

  if (attr == NULL)
  {
    /*
     * 真实 CMSIS-RTOS2 在 attr == NULL 时用一套默认值
     * （栈大小 = 由 `osStackSize` 配置决定、优先级 = osPriorityNormal）。
     * 本工程四个任务都显式给了 attr，所以这个分支只是"照实记录"，
     * 不做猜测 —— 猜错了会让"忘了传 attr"这种 bug 看起来是好的。
     */
    t->attributes_null = 1;
    copy_name(t->name, "");
    t->stack_size = 0U;
    t->priority   = osPriorityNormal;
  }
  else
  {
    t->attributes_null = 0;
    copy_name(t->name, attr->name);
    t->stack_size = attr->stack_size;
    t->priority   = attr->priority;
  }

  return (osThreadId_t)t;
}

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
  host_os2_mutex_t *m;

  if (s_mutex_n >= HOST_OS2_MAX_THREADS)
  {
    return NULL;
  }

  s_mutex_calls++;

  if ((s_fail_mutex_at != 0) && (s_mutex_calls == s_fail_mutex_at))
  {
    return NULL;
  }

  m = &s_mutex[s_mutex_n++];

  if (attr == NULL)
  {
    m->attributes_null = 1;
    copy_name(m->name, "");
    m->attr_bits = 0U;
  }
  else
  {
    m->attributes_null = 0;
    copy_name(m->name, attr->name);
    m->attr_bits = attr->attr_bits;
  }

  return (osMutexId_t)m;
}

uint32_t osKernelGetTickCount(void)
{
  return s_tick;
}

osStatus_t osDelayUntil(uint32_t ticks)
{
  if (s_deadline_n < HOST_OS2_DEADLINE_MAX)
  {
    s_deadline[s_deadline_n] = ticks;
  }
  s_deadline_n++;

  /*
   * 真实内核的语义：绝对时刻已经过去 → 立刻返回（不延时），并报参数错。
   * 现象是任务开始全速空转追赶，计数器疯涨。
   */
  if (ticks <= s_tick)
  {
    s_late_n++;
    s_last_status = osErrorParameter;
  }
  else
  {
    /*
     * 醒来时刻 = 申请的绝对时刻 + 唤醒抖动。
     * 抖动是刻意加的 —— 它是"周期漂移"能被测出来的唯一原因，见 s_wake_jitter 的说明。
     */
    s_tick        = ticks + s_wake_jitter;
    s_last_status = osOK;
  }

  /*
   * 任务体是 `for(;;)`，唯一的出口就在这里。
   * 第 `cycles` 次延时调用时跳回 `host_os2_run_task()`。
   */
  if (s_jb_armed)
  {
    s_cycles_left--;
    if (s_cycles_left <= 0)
    {
      s_jb_armed = 0;
      longjmp(s_jb, 1);
    }
  }

  return s_last_status;
}

/* ============================================================================
 * 四、互斥量与延时（M2.4 起才需要）
 *
 * 这一节存在的理由只有一个：`AVE_gimbal.h` 有一条硬约束叫
 * 「**持锁期间不许睡眠**」。要能判它，就必须同时看得见"锁"和"延时"两件事，
 * 而且要看得出**顺序** —— 所以这里每一下都往 `host_trace` 里记一笔。
 * ==========================================================================*/

/*
 * 默认 **`osKernelInactive`**，不是 `osKernelRunning`。
 *
 * 理由不是"保守"，是**如实**：本替身根本不模拟调度（见 fake_os2.h 顶部），
 * 它没有"调度器已经启动"这回事。谎报一个 running 会让产品代码里
 * 「问内核在不在跑」那段逻辑走进一个**在这个替身上不存在的世界** ——
 * 而它恰好是 `AVE_gimbal.c` 用来决定"睡 osDelay 还是 HAL_Delay"的判据。
 *
 * 顺带一个实际好处：默认状态下上电使能走 `HAL_Delay`，**不动 tick** ——
 * M1.5 那批"周期不漂"的用例因此不会因为 App_Init 多出 120 毫秒而错位。
 * 要测任务路径的用例自己 `host_os2_set_kernel_state(osKernelRunning)`。
 */
static osKernelState_t s_kernel_state = osKernelInactive;
static int             s_lock_calls;
static int             s_unlock_calls;
static int             s_held;          /**< 当前持有深度；正常应当 0/1 */
static int             s_fail_lock_at;

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
  (void)mutex_id;
  (void)timeout;

  s_lock_calls++;

  if ((s_fail_lock_at != 0) && (s_lock_calls == s_fail_lock_at))
  {
    host_trace("lock-timeout");
    /*
     * 真实内核在超时后返回 osErrorTimeout，**而且没有把锁给调用方**。
     * 于是调用方若照样发帧，两条帧就可能交错 ——
     * 这正是 `AVE_gimbal.c` 里 `gimbal_bus_lock()` 返回 −1 时必须放弃发送的理由。
     */
    return osErrorTimeout;
  }

  s_held++;
  host_trace("lock");
  return osOK;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
  (void)mutex_id;

  s_unlock_calls++;
  if (s_held > 0)
  {
    s_held--;
  }
  host_trace("unlock");
  return osOK;
}

osStatus_t osDelay(uint32_t ticks)
{
  host_trace_ms("osdelay", ticks);

  /*
   * 时间往前走两件事：tick 前进，以及**在途的中断发送自己完成**。
   * 后者是保真度而不是方便 —— 见 fake_hal.c 的 `host_uart_tx_progress()`。
   */
  host_uart_tx_progress(ticks);
  s_tick += ticks;

  return osOK;
}

osKernelState_t osKernelGetState(void)
{
  return s_kernel_state;
}

/* ============================================================================
 * 五、查询
 * ==========================================================================*/

int host_os2_thread_count(void) { return s_thread_n; }
int host_os2_mutex_count(void)  { return s_mutex_n; }

const host_os2_thread_t *host_os2_thread(int i)
{
  if ((i < 0) || (i >= s_thread_n))
  {
    return NULL;
  }
  return &s_thread[i];
}

const host_os2_mutex_t *host_os2_mutex(int i)
{
  if ((i < 0) || (i >= s_mutex_n))
  {
    return NULL;
  }
  return &s_mutex[i];
}

const host_os2_thread_t *host_os2_find(const char *name)
{
  int i;
  for (i = 0; i < s_thread_n; i++)
  {
    if (strcmp(s_thread[i].name, name) == 0)
    {
      return &s_thread[i];
    }
  }
  return NULL;
}

/* ============================================================================
 * 六、注入与推进
 * ==========================================================================*/

void host_os2_fail_thread_at(int n) { s_fail_thread_at = n; }
void host_os2_fail_mutex_at(int n)  { s_fail_mutex_at  = n; }

void host_os2_set_wake_jitter(uint32_t ms) { s_wake_jitter = ms; }

int host_os2_run_task(osThreadFunc_t func, int cycles)
{
  int before;
  int after;

  if ((func == NULL) || (cycles <= 0))
  {
    return 0;
  }

  before = s_deadline_n;

  s_cycles_left = cycles;
  s_jb_armed    = 1;

  /*
   * `setjmp` 返回 0 = 第一次到这里，进任务函数；
   * 返回非 0 = 任务里某个 `osDelayUntil` 把我们跳回来了。
   */
  if (setjmp(s_jb) == 0)
  {
    func(NULL);
  }

  s_jb_armed = 0;

  after = s_deadline_n;
  return (after >= before) ? (after - before) : (HOST_OS2_DEADLINE_MAX - before + after);
}

uint32_t host_os2_tick(void)
{
  return s_tick;
}

void host_os2_advance(uint32_t ms)
{
  s_tick += ms;
}

uint32_t host_os2_deadline(int i)
{
  if ((i < 0) || (i >= s_deadline_n))
  {
    return 0U;
  }
  return s_deadline[i];
}

int host_os2_deadline_count(void) { return s_deadline_n; }
int host_os2_late_count(void)     { return s_late_n; }

int host_os2_lock_count(void)   { return s_lock_calls; }
int host_os2_unlock_count(void) { return s_unlock_calls; }
int host_os2_held_count(void)   { return s_held; }

void host_os2_set_kernel_state(osKernelState_t st) { s_kernel_state = st; }
void host_os2_fail_lock_at(int n)                  { s_fail_lock_at = n; }

osStatus_t host_os2_last_status(void) { return s_last_status; }

/* ============================================================================
 * 七、复位
 * ==========================================================================*/

void host_os2_reset(void)
{
  s_thread_n = 0;
  s_mutex_n  = 0;

  s_fail_thread_at = 0;
  s_fail_mutex_at  = 0;
  s_thread_calls   = 0;
  s_mutex_calls    = 0;

  s_tick        = 0U;
  s_deadline_n  = 0;
  s_late_n      = 0;
  s_last_status = osOK;
  s_jb_armed    = 0;
  s_cycles_left = 0;
  s_wake_jitter = 1U;   /* 恢复默认；要改的用例请在复位之后再设 */

  s_kernel_state = osKernelInactive;
  s_lock_calls   = 0;
  s_unlock_calls = 0;
  s_held         = 0;
  s_fail_lock_at = 0;
}
