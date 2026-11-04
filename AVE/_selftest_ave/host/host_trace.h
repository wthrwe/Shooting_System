/**
 * 事件轨迹 —— 记下"谁先谁后"，给**顺序类断言**用。
 *
 * ── 为什么计数器不够，非要一条轨迹 ──────────────────────────────────────────
 *   `AVE_gimbal.c` 有一条硬约束是「**持锁期间不许睡眠**」，即延时必须在锁外。
 *   用计数器看，这件事是这样的：
 *
 *     锁  : acquire 1 次、release 1 次      ← 完全正常
 *     延时: 10 毫秒一次                     ← 完全正常
 *
 *   **两个计数都对，而约束被违反了。** 只有把事件按发生顺序记下来，
 *   「lock → txit → unlock → delay」和「lock → txit → delay → unlock」
 *   才区分得开 —— 后者会让等锁的高优先级任务整整白等 10 毫秒。
 *
 *   这和 `verify.md` 里那条老结论是同一条：**判据必须能把"坏"和"好"分开**，
 *   分不开的判据不是判据，哪怕它一直在通过。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#ifndef HOST_TRACE_H
#define HOST_TRACE_H

#include <stdint.h>

/** 最多记多少条。超了**停止记录**（不覆盖旧的）—— 覆盖会把"开头"丢掉，
 *  而出问题的往往正是开头几步。 */
#define HOST_TRACE_MAX 512
#define HOST_TRACE_TAG_MAX 24

/** 记一个事件（如 `"lock"` / `"txit"`）。 */
void host_trace(const char *tag);

/** 记一个带毫秒数的事件，存成 `"tag:123"`。 */
void host_trace_ms(const char *tag, uint32_t ms);

/** 已记录的事件条数。 */
int host_trace_count(void);

/** 第 i 条事件的文本（越界返回 `NULL`）。 */
const char *host_trace_at(int i);

/** 因缓冲写满而**没能记下**的事件数。非 0 说明轨迹不完整，别拿它当证据。 */
int host_trace_dropped(void);

void host_trace_reset(void);

/**
 * 把 `[from, to)` 区间的事件拼成 `"a,b,c"` 写进 `out`，便于失败时打印。
 *
 * 为什么要有它：断言失败时只报"第 3 条不是 unlock"是没法排查的 ——
 * 需要看到**整段序列**才知道它到底走到哪一步了。
 *
 * @return 写进 `out` 的字符数（不含结尾 `'\0'`）
 */
int host_trace_join(int from, int to, char *out, int cap);

#endif /* HOST_TRACE_H */
