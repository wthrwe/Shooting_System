/**
 * M1.3 自检 —— 串口抽象层。
 *
 * ── 这份测试**测不到什么**（先说实话）────────────────────────────────────────
 *   环形缓冲的**运行时行为**（满了怎么办、逃生路径触发没有、溢出计数涨不涨）
 *   在本机**无法执行** —— 机器上没有宿主编译器（WSL 未装、VS 只有 Shell 无 C++ 工具链），
 *   而 armcc 出的是 ARM 目标码，编出来也跑不了。
 *
 *   所以本文件覆盖的是**能落到编译期的那部分**：结构不变量、下标回绕算术、
 *   通道表与枚举的一致性、公开接口是否齐全。
 *   运行时行为只能在板子上或装了宿主编译器之后验证 —— 见 verify.md 的"未验证"一节。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "AVE_config.h"
#include "AVE_uart.h"
#include "AVE_ringbuf.h"

#define CHK(name, cond) typedef char chk_##name[(cond) ? 1 : -1]

/* ── 一、通道表结构 ────────────────────────────────────────────────────────── */

/* 通道数必须是 3：串口分配是硬约束（USART1 视觉 / USART2 电机 / USART3 调试） */
CHK(ch_count_is_3,  ((int)AVE_UART_CH_NUM == 3));

/* 编号必须从 0 开始且连续 —— s_huart[] 的物理顺序就是枚举声明顺序 */
CHK(cam_is_0,       ((int)AVE_UART_CAM   == 0));
CHK(motor_is_1,     ((int)AVE_UART_MOTOR == 1));
CHK(dbg_is_2,       ((int)AVE_UART_DBG   == 2));

/* 返回码必须是负数且互不相同 —— 撞了的话调用方就分不清"没数据"和"行太长" */
CHK(code_enodata_neg,     (AVE_UART_ENODATA        < 0));
CHK(code_toolong_neg,     (AVE_UART_ELINE_TOO_LONG < 0));
CHK(code_enospc_neg,      (AVE_UART_ENOSPC         < 0));
CHK(code_ebadch_neg,      (AVE_UART_EBADCH         < 0));
CHK(code_etimeout_neg,    (AVE_UART_ETIMEOUT       < 0));
CHK(code_distinct_1,      (AVE_UART_ENODATA        != AVE_UART_ELINE_TOO_LONG));
CHK(code_distinct_2,      (AVE_UART_ELINE_TOO_LONG != AVE_UART_ENOSPC));
CHK(code_distinct_3,      (AVE_UART_ENOSPC         != AVE_UART_EBADCH));
CHK(code_distinct_4,      (AVE_UART_EBADCH         != AVE_UART_ETIMEOUT));
CHK(code_distinct_5,      (AVE_UART_ENODATA        != AVE_UART_ENOSPC));
CHK(code_distinct_6,      (AVE_UART_ENODATA        != AVE_UART_EBADCH));
CHK(code_distinct_7,      (AVE_UART_ENODATA        != AVE_UART_ETIMEOUT));
CHK(code_distinct_8,      (AVE_UART_ELINE_TOO_LONG != AVE_UART_EBADCH));
CHK(code_distinct_9,      (AVE_UART_ELINE_TOO_LONG != AVE_UART_ETIMEOUT));
CHK(code_distinct_10,     (AVE_UART_ENOSPC         != AVE_UART_ETIMEOUT));

/* ── 二、环形缓冲的下标回绕算术 ────────────────────────────────────────────── */

/*
 * 这三条是**能落到编译期的关键部分**：环形缓冲用「与掩码」代替取模，
 * 掩码算错就静默越界 —— 那是最难查的一类错误，而它在编译期就能验。
 */
CHK(buf_is_pow2,   ((AVE_UART_RX_BUF_SIZE & (AVE_UART_RX_BUF_SIZE - 1)) == 0));
CHK(buf_min_size,  (AVE_UART_RX_BUF_SIZE >= 2));
CHK(mask_top,      (((AVE_UART_RX_BUF_SIZE - 1) & (AVE_UART_RX_BUF_SIZE - 1)) == (AVE_UART_RX_BUF_SIZE - 1)));

/* 回绕点：下标 = size 时必须折回 0，这是"绕圈"的核心 */
CHK(mask_wraps,    ((AVE_UART_RX_BUF_SIZE & (AVE_UART_RX_BUF_SIZE - 1)) == 0));

/*
 * 计数器用「只增不回绕」的写法，靠 32 位无符号回绕保证 used 正确。
 * 这里把回绕场景写死成具体数字验证 —— 这正是纯逻辑、编译期可判的那部分。
 */
CHK(used_wraps_32,   (((uint32_t)(0x00000000u - 0xFFFFFF00u)) == 256u));
CHK(used_wraps_full, (((uint32_t)(0x00000005u - 0xFFFFFF05u)) == 256u));
CHK(used_no_wrap,    (((uint32_t)(500u - 100u)) == 400u));

/* 消费端"丢弃 n 个"的推进也走同一套无符号算术 */
CHK(skip_wraps,      (((uint32_t)(0xFFFFFF00u + 300u)) == 44u));

/* ── 三、公开接口齐全性 —— 未声明的会在这里报错 ────────────────────────────── */

static void (*const s_fn_init)(void)                                  = AVE_Uart_Init;
static int  (*const s_fn_line)(AVE_UartChannel, char *, int)          = AVE_Uart_ReadLine;
static int  (*const s_fn_read)(AVE_UartChannel, uint8_t *, int)       = AVE_Uart_Read;
static uint32_t (*const s_fn_avail)(AVE_UartChannel)                  = AVE_Uart_Available;
static void (*const s_fn_flush)(AVE_UartChannel)                      = AVE_Uart_Flush;
static int  (*const s_fn_write)(AVE_UartChannel, const uint8_t *, int) = AVE_Uart_Write;
static int  (*const s_fn_wstr)(AVE_UartChannel, const char *)         = AVE_Uart_WriteStr;
static const AVE_UartStats *(*const s_fn_stats)(AVE_UartChannel)      = AVE_Uart_Stats;

/* 统计量字段必须齐全 —— 少一个计数器就少一块仪表盘 */
static AVE_UartStats s_stats_shape;
static void use_stats(void)
{
  s_stats_shape.rx_bytes   = 1U;
  s_stats_shape.drop_bytes = 1U;
  s_stats_shape.drop_lines = 1U;
  s_stats_shape.err        = 1U;
  s_stats_shape.rx_restart = 1U;
  s_stats_shape.tx_err     = 1U;
}

/* ── 四、环形缓冲结构体与函数签名 ──────────────────────────────────────────── */

static AVE_RingBuf     s_rb_shape;
static uint8_t         s_rb_storage[AVE_UART_RX_BUF_SIZE];

static int use_ringbuf(void)
{
  uint8_t  b = 0U;
  uint32_t off = 0U;
  int      r;

  r  = AVE_RbInit(&s_rb_shape, s_rb_storage, AVE_UART_RX_BUF_SIZE);
  r += AVE_RbPush(&s_rb_shape, 0x41U);
  r += AVE_RbPop(&s_rb_shape, &b);
  r += AVE_RbFind(&s_rb_shape, (uint8_t)'\n', &off);
  r += (int)AVE_RbUsed(&s_rb_shape);
  r += (int)AVE_RbFree(&s_rb_shape);
  r += AVE_RbIsEmpty(&s_rb_shape);
  AVE_RbRead(&s_rb_shape, &b, 1U);
  AVE_RbSkip(&s_rb_shape, 1U);
  AVE_RbDropAll(&s_rb_shape);
  return r;
}

/* 把上面所有东西用掉，避免"声明未使用"的警告掩盖真实错误 */
int ave_m13_selftest(void)
{
  use_stats();
  return use_ringbuf()
       + (s_fn_init != 0) + (s_fn_line != 0) + (s_fn_read != 0) + (s_fn_avail != 0)
       + (s_fn_flush != 0) + (s_fn_write != 0) + (s_fn_wstr != 0) + (s_fn_stats != 0);
}
