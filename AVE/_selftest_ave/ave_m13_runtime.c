/**
 * M1.3 运行时自检 —— **真正跑起来**的那一半。
 *
 * ── 这份测试和 ave_m13_selftest.c 的分工 ─────────────────────────────────────
 *   ave_m13_selftest.c  编译期：结构不变量、下标回绕算术、通道表一致性
 *   本文件              运行时：**把测试的字节真的灌进去，看行为**
 *
 * ── 被测对象是产品代码本身 ──────────────────────────────────────────────────
 *   链接进来的是工程里那份 **`AVE/AVE_uart.c`、`AVE/AVE_board.c`、
 *   `AVE/AVE_ringbuf.h`** —— 不复制、不改写。
 *   被替身的只有 HAL（host/ 目录下的 shim 与 fake_hal.c）。
 *
 * ── 覆盖的是 M1.3 验收标准里**不依赖硬件**的部分 ─────────────────────────────
 *   验收①  一次 800 字节 → 内容完整、顺序不乱          → U3
 *   验收②  分三次发带 '\n' 的行 → 三次都完整返回        → U4
 *   验收③  overflow 与 err 计数保持 0                  → U5
 *   验收④  连发 > 1024 字节 → overflow 增长，**系统不死** → U6（逃生路径）
 *
 *   ⚠️ **测不到**：HAL 是否真的把 NVIC 使能了、USART3 的线是否真的接对了、
 *      115200 的实际误码率。那些必须上板。本文件证明的是**逻辑**，不是硬件。
 *
 * ── 🔴 本文件的一条硬约束（第六十一次加）──────────────────────────────────
 *   V 组的所有**面内**坐标必须**派生**自 `AVE_VISION_SCREEN_W/H`
 *   （`v_in_screen()` 做前提断言），**不许再写死像素值**。
 *   写死的那一刻就把用例绑死在某一次分辨率上：2026-10-08 画面
 *   `640×480 → 224×224`（`AVE_tuning.h:73`）之后，写死的 `320`/`240` 全部越界，
 *   V 组九条用例静默变红，而这套用例此后没被跑过 ⇒ 连带让 M9/M10/M11
 *   三项变异证据**假绿**了整整 5 天（详见 V 组开头那段注释）。
 *   改 V 组时的第一反应应当是"这个数能不能从宏推出来"，而**不是**"把 320 改成 112"。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "AVE_config.h"
#include "AVE_uart.h"
#include "AVE_ringbuf.h"
#include "AVE_vision.h"
#include "fake_hal.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * 极简测试框架
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

static int eq_bytes(const char *what, const uint8_t *got, const uint8_t *want, int n)
{
  if (memcmp(got, want, (size_t)n) != 0)
  {
    int i;
    for (i = 0; i < n; i++)
    {
      if (got[i] != want[i])
      {
        printf("       ✗ %s: 第 %d 个字节起不符（实际 0x%02X，期望 0x%02X）\n",
               what, i, got[i], want[i]);
        break;
      }
    }
    g_test_failed = 1;
    return 0;
  }
  return 1;
}

/* ============================================================================
 * 公用素材
 * ==========================================================================*/

#define BIG_N 1500

static uint8_t g_pat[BIG_N];     /* 测试图案：**不含 '\n' 也不含 '\r'** */
static char    g_out[BIG_N + 8];

/**
 * 填充测试图案。取值范围 0x41~0x74（'A' 起 52 个），
 * 刻意避开 0x0A/0x0D —— 否则图案自己就会被当成行尾，测的就不是缓冲了。
 * 用 52 而不是 2 或 10，是为了让"错位一个字节"立刻在后面每一位都暴露出来。
 */
static void pattern_fill(uint8_t *p, int n)
{
  int i;
  for (i = 0; i < n; i++)
  {
    p[i] = (uint8_t)(0x41 + (i % 52));
  }
}

/**
 * 每个用例开头的统一起点。
 *
 * ⚠️ `AVE_Vision_Init()` 也在这里面，理由和当初给 `AVE_Uart_Init()` 写的一样：
 *    视觉层的计数器与"最后一次有效帧"是**静态状态**，不随 `host_reset_all()`
 *    复位。漏掉它，V 组用例之间就会互相污染 —— 而污染的表现是"某个用例
 *    单独跑会过、连起来跑就挂"，正是 `ave_m15_runtime.c` 留痕过的那个脆弱点。
 */
static void fresh(void)
{
  host_reset_all();
  AVE_Uart_Init();
  AVE_Vision_Init();
}

/* ============================================================================
 * 一、环形缓冲本体（纯逻辑，与串口无关）
 * ==========================================================================*/

/** R1 填满、拒绝第 size+1 个、且**不覆盖最旧数据**。 */
static void test_R1_fill_no_overwrite(void)
{
  AVE_RingBuf rb;
  uint8_t     store[8];
  uint8_t     b;
  int         i;

  begin("R1 填满/拒收/不覆盖");
  memset(store, 0, sizeof(store));
  is_true("AVE_RbInit(8) 成功", AVE_RbInit(&rb, store, 8U) == 0);

  for (i = 0; i < 8; i++)
  {
    is_true("前 8 个都应写入成功", AVE_RbPush(&rb, (uint8_t)(0x10 + i)) == 0);
  }
  eq_u32("填满后 used", AVE_RbUsed(&rb), 8U);
  eq_u32("填满后 free", AVE_RbFree(&rb), 0U);

  is_true("第 9 个必须被拒绝", AVE_RbPush(&rb, 0x99U) == -1);

  /* 关键：被拒绝的那个字节绝不能挤掉最旧数据 */
  b = 0;
  (void)AVE_RbPop(&rb, &b);
  eq_u32("最旧数据未被覆盖", b, 0x10U);
  end();
}

/** R2 反复 push/pop 强制下标回绕，逐字节校验顺序。 */
static void test_R2_wraparound_order(void)
{
  AVE_RingBuf rb;
  uint8_t     store[8];
  int         i;
  int         ok = 1;

  begin("R2 回绕后顺序不乱");
  memset(store, 0, sizeof(store));
  ok &= (AVE_RbInit(&rb, store, 8U) == 0);

  /*
   * 每轮写 5 个、读 5 个 —— 净进度为 0，但 head/tail 会一路涨上去，
   * 下标 (head & mask) 反复绕圈。40 轮 = 下标绕了好几圈。
   */
  for (i = 0; i < 40; i++)
  {
    uint8_t  w[5];
    uint8_t  r[5];
    int      k;

    for (k = 0; k < 5; k++)
    {
      w[k] = (uint8_t)(i * 5 + k);
    }
    for (k = 0; k < 5; k++)
    {
      if (AVE_RbPush(&rb, w[k]) != 0)
      {
        ok = 0;
      }
    }
    memset(r, 0, sizeof(r));
    AVE_RbRead(&rb, r, 5U);
    if (memcmp(w, r, 5) != 0)
    {
      printf("       第 %d 轮数据错乱\n", i);
      ok = 0;
      break;
    }
  }
  is_true("40 轮 push/read 后数据始终正确", ok);
  eq_u32("净进度为 0", AVE_RbUsed(&rb), 0U);
  end();
}

/** R3 32 位计数器回绕（head/tail 越过 0xFFFFFFFF）。真实设备连续跑几天就会到这。 */
static void test_R3_counter_overflow(void)
{
  AVE_RingBuf rb;
  uint8_t     store[16];
  uint8_t     b;
  int         i;
  int         ok = 1;

  begin("R3 32 位计数器回绕");
  memset(store, 0, sizeof(store));
  ok &= (AVE_RbInit(&rb, store, 16U) == 0);

  /* 手工把两个计数器推到 32 位边界附近，很快就要翻零 */
  rb.head = 0xFFFFFFF8UL;
  rb.tail = 0xFFFFFFF8UL;
  eq_u32("设好后 used 应为 0", AVE_RbUsed(&rb), 0U);

  /* 写 12 个 —— 中途 head 会从 0xFFFFFFFF 翻回 0 */
  for (i = 0; i < 12; i++)
  {
    if (AVE_RbPush(&rb, (uint8_t)(0xA0 + i)) != 0)
    {
      ok = 0;
    }
  }
  is_true("跨 32 位边界写入 12 个都成功", ok);
  eq_u32("跨边界后 used 仍是 12", AVE_RbUsed(&rb), 12U);
  is_true("head 已翻过零", rb.head < 0xFFFFFFF8UL);

  /* 读回来必须是 A0..AB —— 差一个就说明"减法天然正确"这个前提不成立 */
  ok = 1;
  for (i = 0; i < 12; i++)
  {
    b = 0;
    (void)AVE_RbPop(&rb, &b);
    if (b != (uint8_t)(0xA0 + i))
    {
      printf("       第 %d 个读回 0x%02X，期望 0x%02X\n", i, b, (uint8_t)(0xA0 + i));
      ok = 0;
      break;
    }
  }
  is_true("跨边界数据顺序正确", ok);
  eq_u32("读空后 used", AVE_RbUsed(&rb), 0U);
  end();
}

/** R4 AVE_RbFind 的 offset 语义：偏移、不消费、找不到。 */
static void test_R4_find_semantics(void)
{
  AVE_RingBuf rb;
  uint8_t     store[16];
  uint32_t    off = 0xDEADBEEFUL;
  const char *s   = "ab\ncd";

  begin("R4 Find 的偏移与不消费");
  memset(store, 0, sizeof(store));
  is_true("AVE_RbInit(16) 成功", AVE_RbInit(&rb, store, 16U) == 0);
  (void)AVE_RbPush(&rb, (uint8_t)s[0]);
  (void)AVE_RbPush(&rb, (uint8_t)s[1]);
  (void)AVE_RbPush(&rb, (uint8_t)s[2]);
  (void)AVE_RbPush(&rb, (uint8_t)s[3]);
  (void)AVE_RbPush(&rb, (uint8_t)s[4]);

  is_true("能找到 '\\n'", AVE_RbFind(&rb, (uint8_t)'\n', &off) == 0);
  eq_u32("偏移应为 2", off, 2U);
  eq_u32("Find 不应消费数据", AVE_RbUsed(&rb), 5U);

  is_true("找不到的字节应返回 -1", AVE_RbFind(&rb, (uint8_t)'z', &off) == -1);
  end();
}

/** R5 Read/Skip 的边界：n == used 时正好清空、不越界。 */
static void test_R5_read_skip_bounds(void)
{
  AVE_RingBuf rb;
  uint8_t     store[8];
  uint8_t     out[8];
  struct { uint8_t b[8]; uint8_t canary[4]; } g;
  int         i;

  begin("R5 Read/Skip 边界");
  memset(store, 0, sizeof(store));
  is_true("AVE_RbInit(8) 成功", AVE_RbInit(&rb, store, 8U) == 0);
  for (i = 0; i < 8; i++)
  {
    (void)AVE_RbPush(&rb, (uint8_t)i);
  }

  memset(&g, 0, sizeof(g));
  memset(g.canary, 0x7F, sizeof(g.canary));
  AVE_RbRead(&rb, g.b, 8U);                       /* 恰好读满，一个不多 */
  is_true("Read(8) 未越界写", g.canary[0] == 0x7F && g.canary[3] == 0x7F);
  eq_u32("Read 后清空", AVE_RbUsed(&rb), 0U);

  for (i = 0; i < 8; i++)
  {
    (void)AVE_RbPush(&rb, (uint8_t)(0x80 + i));
  }
  AVE_RbSkip(&rb, 3U);
  eq_u32("Skip(3) 后剩余", AVE_RbUsed(&rb), 5U);
  memset(out, 0, sizeof(out));
  AVE_RbRead(&rb, out, 5U);
  eq_u32("Skip 后第一个应是 0x83", out[0], 0x83U);
  end();
}

/* ============================================================================
 * 二、串口层：中断路径 → 环形缓冲 → 按行读取
 * ==========================================================================*/

/** U1 一条最简的行。 */
static void test_U1_basic_line(void)
{
  int n;

  begin("U1 单行基本收发");
  fresh();

  is_true("注入 4 字节都成功",
           host_uart_inject_str(AVE_HUART_ESP, "abc\n", 4) == 4);
  eq_u32("无人死亡（重新武装正常）", host_dead_rx_count, 0UL);
  eq_u32("有数据可读", AVE_Uart_Available(AVE_UART_DBG), 4U);

  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("返回行长", n, 3);
  is_true("内容是 abc", (n == 3) && (strcmp(g_out, "abc") == 0));
  eq_u32("缓冲已清空", AVE_Uart_Available(AVE_UART_DBG), 0U);

  eq_i32("再读应报无数据", AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out)),
         AVE_UART_ENODATA);
  end();
}

/** U2 CRLF 行尾 —— 串口助手的默认行尾，'\r' 必须被吃掉。 */
static void test_U2_crlf(void)
{
  int n;

  begin("U2 CRLF 行尾");
  fresh();

  (void)host_uart_inject_str(AVE_HUART_ESP, "abc\r\n", 5);
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("返回行长（不含 \\r）", n, 3);
  is_true("内容是 abc 且不带 \\r", (n == 3) && (memcmp(g_out, "abc", 4) == 0));
  end();
}

/**
 * U3 验收①：一次 800 字节，内容完整、顺序不乱。
 *
 * 刻意先推进 600 字节再发 —— 这样 800 字节会**跨过环形缓冲的物理边界回绕**，
 * 而不是从头顺排。不回绕的测试测不出回绕的 bug。
 */
static void test_U3_accept_800_bytes(void)
{
  int n;
  int drained;

  begin("U3 验收① 一次 800 字节（含回绕）");
  fresh();

  /* 先灌 600 字节再读掉，把 head/tail 各推进 600，制造回绕 */
  pattern_fill(g_pat, 600);
  (void)host_uart_inject_str(AVE_HUART_ESP, (const char *)g_pat, 600);
  drained = AVE_Uart_Read(AVE_UART_DBG, (uint8_t *)g_out, 600);
  eq_i32("预推进读到 600 字节", drained, 600);
  eq_bytes("预推进内容正确", (const uint8_t *)g_out, g_pat, 600);
  eq_u32("预推进后缓冲为空", AVE_Uart_Available(AVE_UART_DBG), 0U);

  /* 正式：800 字节 + '\n' */
  pattern_fill(g_pat, 800);
  (void)host_uart_inject_str(AVE_HUART_ESP, (const char *)g_pat, 800);
  (void)host_uart_inject(AVE_HUART_ESP, (uint8_t)'\n');

  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("返回长度 800", n, 800);
  eq_bytes("800 字节逐字节一致、顺序不乱", (const uint8_t *)g_out, g_pat, 800);
  eq_u32("缓冲已清空", AVE_Uart_Available(AVE_UART_DBG), 0U);
  end();
}

/** U4 验收②：分三次发带 '\n' 的行，三次都完整返回。 */
static void test_U4_three_lines(void)
{
  static const char *lines[3] = { "LINE1", "LINE2", "LINE3" };
  int i;

  begin("U4 验收② 三次分行");
  fresh();

  for (i = 0; i < 3; i++)
  {
    char sent[16];
    int  n;

    sprintf(sent, "%s\n", lines[i]);
    (void)host_uart_inject_str(AVE_HUART_ESP, sent, (int)strlen(sent));

    n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
    if (!eq_i32("返回行长", n, (long)strlen(lines[i])))
    {
      continue;
    }
    is_true("内容一致", strcmp(g_out, lines[i]) == 0);
  }
  end();
}

/** U5 验收③：以上正常收发过程中，三个计数必须保持 0。 */
static void test_U5_counters_stay_zero(void)
{
  const AVE_UartStats *st;

  begin("U5 验收③ 正常收发不涨错误计数");
  fresh();

  (void)host_uart_inject_str(AVE_HUART_ESP, "hello world\n", 12);
  (void)AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));

  pattern_fill(g_pat, 800);
  (void)host_uart_inject_str(AVE_HUART_ESP, (const char *)g_pat, 800);
  (void)host_uart_inject(AVE_HUART_ESP, (uint8_t)'\n');
  (void)AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));

  st = AVE_Uart_Stats(AVE_UART_DBG);
  is_true("统计指针非空", st != NULL);
  if (st != NULL)
  {
    /* 12（"hello world\n"）+ 801（800 字节图案 + '\n'）= 813 */
    eq_u32("接收字节总数 = 813", st->rx_bytes, 813U);
    eq_u32("drop_bytes = 0", st->drop_bytes, 0U);
    eq_u32("drop_lines = 0", st->drop_lines, 0U);
    eq_u32("err = 0", st->err, 0U);
    eq_u32("tx_err = 0", st->tx_err, 0U);
  }
  eq_u32("仿真层无死亡", host_dead_rx_count, 0UL);
  eq_u32("仿真层无 HAL_BUSY", host_rearm_busy_count, 0UL);
  end();
}

/**
 * U6 验收④ + 本模块最重要的那条坑：**无换行的逃生路径**。
 *
 * 连发 1500 字节且一个 '\n' 都没有 → 缓冲区写满。
 * 此时若"按行读取"仍傻等 '\n'，就是**永久卡死**（读不到行 → 不消费 → 更满）。
 * 判据：必须触发一次整段丢弃并上报，且**之后系统仍然活着**。
 */
static void test_U6_escape_path_no_deadlock(void)
{
  const AVE_UartStats *st;
  int n;

  begin("U6 验收④ 满且无换行 → 逃生路径");
  fresh();

  pattern_fill(g_pat, BIG_N);
  (void)host_uart_inject_str(AVE_HUART_ESP, (const char *)g_pat, BIG_N);

  eq_u32("缓冲区被填满", AVE_Uart_Available(AVE_UART_DBG), AVE_UART_RX_BUF_SIZE);

  /* 第 1 次：满且无换行 → 必须逃生，而不是返回 ENODATA */
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("满且无换行必须报 ELINE_TOO_LONG", n, AVE_UART_ELINE_TOO_LONG);
  eq_u32("逃生后缓冲区被清空", AVE_Uart_Available(AVE_UART_DBG), 0U);

  st = AVE_Uart_Stats(AVE_UART_DBG);
  if (st != NULL)
  {
    eq_u32("drop_lines 计数 +1", st->drop_lines, 1U);
    eq_u32("err 仍为 0（丢弃不是错误）", st->err, 0U);
  }

  /* 逃生之后系统必须还活着 —— 这才是"不死"的定义 */
  (void)host_uart_inject_str(AVE_HUART_ESP, "ok\n", 3);
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("逃生后仍能正常收行", n, 2);
  is_true("内容为 ok", (n == 2) && (strcmp(g_out, "ok") == 0));

  eq_u32("全程无人死亡", host_dead_rx_count, 0UL);
  end();
}

/** U7 overflow 计数：写满后新来的字节被丢弃并计数，且**不覆盖旧数据**。 */
static void test_U7_overflow_counter(void)
{
  const AVE_UartStats *st;

  begin("U7 overflow 计数");
  fresh();

  pattern_fill(g_pat, AVE_UART_RX_BUF_SIZE);
  (void)host_uart_inject_str(AVE_HUART_ESP, (const char *)g_pat, (int)AVE_UART_RX_BUF_SIZE);
  st = AVE_Uart_Stats(AVE_UART_DBG);
  if (st != NULL)
  {
    eq_u32("装满时 drop_bytes 仍为 0", st->drop_bytes, 0U);
  }

  /* 再灌 10 个 —— 这 10 个全部进不去 */
  (void)host_uart_inject_str(AVE_HUART_ESP, "0123456789", 10);

  st = AVE_Uart_Stats(AVE_UART_DBG);
  if (st != NULL)
  {
    eq_u32("drop_bytes = 10", st->drop_bytes, 10U);
    eq_u32("rx_bytes = 1024+10（收到的都记，丢的是新来的）",
           st->rx_bytes, AVE_UART_RX_BUF_SIZE + 10U);
  }
  eq_u32("缓冲区仍满且是原来那 1024 个", AVE_Uart_Available(AVE_UART_DBG),
         AVE_UART_RX_BUF_SIZE);
  is_true("最旧数据未被覆盖（首字节仍是图案第 0 个）",
          AVE_Uart_Read(AVE_UART_DBG, (uint8_t *)g_out, 1) == 1 &&
          (uint8_t)g_out[0] == g_pat[0]);
  eq_u32("全程无人死亡", host_dead_rx_count, 0UL);
  end();
}

/**
 * U8 行的长度超过调用方缓冲区 → 报 ENOSPC、丢掉该行、**绝不越界写**。
 *
 * 这里最容易写错的是"用 Read 去丢比 cap 还多的字节" —— 那会直接踩烂调用方的栈。
 * 所以判据里放了 canary 哨兵字节。
 */
static void test_U8_line_too_long_for_caller(void)
{
  struct { char b[8]; uint8_t canary[4]; } g;
  const AVE_UartStats *st;
  int n;

  begin("U8 行超出 cap → ENOSPC 且不越界");
  fresh();

  /* 边界先试：cap=8 时 7 个字符刚好放得下 */
  (void)host_uart_inject_str(AVE_HUART_ESP, "1234567\n", 8);
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, 8);
  eq_i32("7 字符放进 cap=8 应成功", n, 7);
  is_true("边界内容正确", (n == 7) && (memcmp(g_out, "1234567", 8) == 0));

  /* 再来 8 个字符 —— 现在就放不下了 */
  memset(&g, 0, sizeof(g));
  memset(g.canary, 0x7F, sizeof(g.canary));
  (void)host_uart_inject_str(AVE_HUART_ESP, "12345678\n", 9);

  n = AVE_Uart_ReadLine(AVE_UART_DBG, g.b, 8);
  eq_i32("超长行应报 ENOSPC", n, AVE_UART_ENOSPC);
  is_true("未越界写调用方缓冲区（哨兵完好）",
          g.canary[0] == 0x7F && g.canary[1] == 0x7F &&
          g.canary[2] == 0x7F && g.canary[3] == 0x7F);

  /* 关键：那一行必须**已经被丢掉**，否则调用方每次读到同一行 → 死循环 */
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g.b, 8);
  eq_i32("超长行已被丢弃，不重复返回", n, AVE_UART_ENODATA);

  st = AVE_Uart_Stats(AVE_UART_DBG);
  if (st != NULL)
  {
    eq_u32("drop_lines 计数 +1", st->drop_lines, 1U);
  }

  /* 丢弃之后不该失去同步 */
  (void)host_uart_inject_str(AVE_HUART_ESP, "next\n", 5);
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("丢弃后仍能正常收下一行", n, 4);
  is_true("内容为 next", (n == 4) && (strcmp(g_out, "next") == 0));
  end();
}

/** U9 参数校验 —— 非法输入必须被挡住，不能越界。 */
static void test_U9_argument_validation(void)
{
  uint8_t buf[8];

  begin("U9 参数校验");
  fresh();

  eq_i32("非法通道号 99", AVE_Uart_ReadLine((AVE_UartChannel)99, g_out, 16),
         AVE_UART_EBADCH);
  eq_i32("负通道号", AVE_Uart_ReadLine((AVE_UartChannel)(-1), g_out, 16),
         AVE_UART_EBADCH);
  eq_i32("空 out 指针", AVE_Uart_ReadLine(AVE_UART_DBG, NULL, 16), AVE_UART_EBADCH);
  eq_i32("cap 太小（<2）", AVE_Uart_ReadLine(AVE_UART_DBG, g_out, 1), AVE_UART_EBADCH);

  eq_i32("Read 非法通道", AVE_Uart_Read((AVE_UartChannel)99, buf, 8), AVE_UART_EBADCH);
  eq_i32("Read 空指针", AVE_Uart_Read(AVE_UART_DBG, NULL, 8), AVE_UART_EBADCH);
  eq_i32("Read cap<=0", AVE_Uart_Read(AVE_UART_DBG, buf, 0), AVE_UART_EBADCH);

  eq_u32("Available 非法通道返回 0", AVE_Uart_Available((AVE_UartChannel)99), 0U);
  is_true("Stats 非法通道返回 NULL", AVE_Uart_Stats((AVE_UartChannel)99) == NULL);

  eq_i32("Write 空指针", AVE_Uart_Write(AVE_UART_DBG, NULL, 4), AVE_UART_EBADCH);
  eq_i32("Write 长度 0", AVE_Uart_Write(AVE_UART_DBG, buf, 0), AVE_UART_EBADCH);
  eq_i32("Write 超过 HAL 的 uint16 上限", AVE_Uart_Write(AVE_UART_DBG, buf, 0x10000),
         AVE_UART_EBADCH);
  eq_i32("WriteStr 空指针", AVE_Uart_WriteStr(AVE_UART_DBG, NULL), AVE_UART_EBADCH);
  eq_i32("WriteStr 空串返回 0", AVE_Uart_WriteStr(AVE_UART_DBG, ""), 0);

  AVE_Uart_Flush((AVE_UartChannel)99);   /* 不应崩 */
  end();
}

/**
 * U10 出错后必须把接收**救回来**。
 *
 * 最坏情形是 HAL 出错后 `RxState` 卡在 BUSY_RX —— 此时直接重新武装会返回 HAL_BUSY，
 * 于是"我明明重新武装了"却依然收不到数据，且**不报错、完全静默**。
 * AVE_uart.c 的对策是"先 Abort 再武装"，本用例正是验它。
 */
static void test_U10_error_recovery(void)
{
  const AVE_UartStats *st;
  int n;

  begin("U10 出错后重新武装");
  fresh();

  (void)host_uart_inject_str(AVE_HUART_ESP, "AB\n", 3);
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("出错前能正常收", n, 2);

  /* 造一次过载错误，并**故意把状态留在 BUSY_RX**（最坏情形） */
  host_raise_error(AVE_HUART_ESP, HAL_UART_ERROR_ORE, 1);

  st = AVE_Uart_Stats(AVE_UART_DBG);
  if (st != NULL)
  {
    eq_u32("err 计数 +1", st->err, 1U);
    eq_u32("rx_restart 计数 +1（确实救回来了）", st->rx_restart, 1U);
  }
  eq_u32("Abort 生效：没有撞上 HAL_BUSY", host_rearm_busy_count, 0UL);

  /* 真正的判据：错误之后串口必须**还能收** */
  (void)host_uart_inject_str(AVE_HUART_ESP, "CD\n", 3);
  n = AVE_Uart_ReadLine(AVE_UART_DBG, g_out, (int)sizeof(g_out));
  eq_i32("出错后仍能收到下一行", n, 2);
  is_true("内容是 CD", (n == 2) && (strcmp(g_out, "CD") == 0));
  eq_u32("出错后无静默死亡", host_dead_rx_count, 0UL);
  end();
}

/** U11 发送与抓包，两路互不串台（原"三路"，见函数内说明）。 */
static void test_U11_write_and_isolation(void)
{
  const uint8_t *p;
  int n;

  begin("U11 发送抓包 + 两路不串台");
  fresh();

  eq_i32("WriteStr 返回长度", AVE_Uart_WriteStr(AVE_UART_DBG, "hello"), 5);
  p = host_tx_data(AVE_HUART_ESP, &n);
  eq_i32("抓到 5 字节", n, 5);
  is_true("抓到的内容是 hello", (n == 5) && (memcmp(p, "hello", 5) == 0));

  /*
   * 🔴 2026-09-28（第二十六次）**用例被削过，这里交代削了什么。**
   *
   * 原版叫「三路隔离」，往第三路 `AVE_UART_MOTOR` 再发一笔 `"MOT"`，
   * 验的是"**两路发送**互不串台"。第 22 次电机后端整体改成 STEP/DIR 之后
   * 那条通道被删了，而现在的两路里 **视觉口是只收的**
   * （见 `AVE_uart.h:52` 的注释）—— 于是"多路发送隔离"这个性质
   * 在今天的硬件上**根本不存在**，不能靠改个名字硬凑：
   * 那会得到一条永远为真的判据，而永远为真的判据等于没测。
   *
   * 所以改为验**仍然成立**的那一条：两路的**接收**缓冲互不影响，
   * 且接收活动不会污染另一路的发送抓包。
   * ⬜ 代价：**"两路同时发送时字节不串台"这条覆盖面没有了** ——
   *    当前硬件上无人可测，等真的出现第二个发送通道时再补。
   */
  (void)host_uart_inject_str(AVE_HUART_CAM, "cam\n", 4);
  eq_u32("相机口有数据", AVE_Uart_Available(AVE_UART_CAM), 4U);
  eq_u32("调试口不受影响", AVE_Uart_Available(AVE_UART_DBG), 0U);

  /* 视觉口收字节这件事，不能动到调试口**发送**方向的抓包 */
  p = host_tx_data(AVE_HUART_ESP, &n);
  eq_i32("接收活动不污染发送抓包", n, 5);
  is_true("内容仍是 hello", (n == 5) && (memcmp(p, "hello", 5) == 0));
  end();
}

/** U12 持续接收：连收 200 字节，一个都不能少，且全程无静默死亡。 */
static void test_U12_sustained_rearm(void)
{
  int i;
  int n;

  begin("U12 持续接收 200 字节");
  fresh();

  for (i = 0; i < 200; i++)
  {
    if (!is_true("每个字节都能注入", host_uart_inject(AVE_HUART_ESP, (uint8_t)'x') == 0))
    {
      break;
    }
  }
  n = (int)AVE_Uart_Available(AVE_UART_DBG);
  eq_i32("200 个字节一个不少", n, 200);
  eq_u32("全程零静默死亡", host_dead_rx_count, 0UL);
  eq_u32("确实走了 200 次回调", host_delivered_count, 200UL);
  end();
}

/* ============================================================================
 * 三、板级：激光极性（安全攸关）
 * ==========================================================================*/

/**
 * L1 激光极性。
 *
 * 🔴 这是全工程**唯一安全攸关**的映射：写反了，上电就对着人眼出光。
 *    判据按当前极性宏推导，所以一旦有人把 AVE_board.h 里两个分支对调，
 *    本用例立刻失败 —— 而"对调两个分支"正是最可能发生的改错方式。
 */
static void test_L1_laser_polarity(void)
{
  int off_level;
  int on_level;

  begin("L1 激光极性");
  host_reset_all();

#if AVE_LASER_ACTIVE_HIGH
  off_level = GPIO_PIN_RESET;   /* 高电平点亮 → 「关」是低 */
  on_level  = GPIO_PIN_SET;
#else
  off_level = GPIO_PIN_SET;     /* 低电平点亮 → 「关」是高 */
  on_level  = GPIO_PIN_RESET;
#endif

  /* 宏本身：两个分支必须互为反相 —— 否则就是"两个分支都写成同一个值" */
  is_true("极性宏的两支互为反相",
          AVE_LASER_LEVEL(0) != AVE_LASER_LEVEL(1));
  eq_i32("极性宏：关 = 期望电平", (long)AVE_LASER_LEVEL(0), off_level);

  /* 行为：初始化必须把激光置到「关」 */
  AVE_Board_Init();
  eq_i32("Board_Init 后激光为关", host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN),
         off_level);

  AVE_Laser_Set(1);
  eq_i32("Set(1) 后为点亮电平", host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN),
         on_level);

  AVE_Laser_Set(0);
  eq_i32("Set(0) 后回到关闭电平", host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN),
         off_level);

  /* 非 0 即点亮：Set(2) 也应点亮，注意别写成 ==1 判断 */
  AVE_Laser_Set(2);
  eq_i32("Set(2) 也应点亮（非 0 即开）",
         host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN), on_level);
  end();
}

/* ============================================================================
 * 三之二、板级：启动故障记录 + 激光联锁（2026-09-26 补，配合 Error_Handler 改造）
 *
 * 🔴 **本组必须排在 L1 之后** —— main() 里的调用顺序本身就是判据的一部分。
 *    它们会给故障码**置位**，而故障码一旦记下就没有清除接口（故意的：
 *    启动失败是既成事实，中途"看起来好了"不代表坏掉的部件好了）。
 *    L1 要验的是"没故障时激光能点亮"，所以 L1 必须跑在置位之前。
 *
 *    这条顺序依赖是**真依赖**，不是随便排的：
 *    若把 B1 挪到 L1 前面，L1 的 `Set(1)` 会被联锁按回"关"电平而失败 ——
 *    那是判据在正确报警，不是判据坏了。
 *
 * ── 被测的是什么 ────────────────────────────────────────────────────────────
 *   `main.c` 的 `Error_Handler()` 由"静默死循环"改成"记码 + 返回（降级运行）"。
 *   降级运行本身**在桌面上造不出来**（要真把时钟配坏），但"记码 + 联锁"这一段
 *   是**纯内存逻辑**，可以在仿真 HAL 上验全。
 *
 *   这四条对应四个最容易写错、且写错了不会有任何症状的地方：
 *     B1 `NoteBootFail(0)` 被拒 —— 否则第一个码成了 0，"这块板子看起来是健康的"，
 *        联锁被解开，而真正的故障码再也进不来（先到先得）。
 *     B2 先到先得 —— 第二个码不许覆盖第一个（留根因，不留症状）。
 *     B3 计数照常累加 —— "先到先得"不能顺手把计数也吞了。
 *     B4 联锁生效 **且拒绝时仍强制写关电平** —— 只拒绝不纠正，
 *        引脚就可能停在"开"的电平上，等于没联锁。
 * ==========================================================================*/

/** B1 故障码 0 必须被拒收。 */
static void test_B1_zero_code_rejected(void)
{
  int on_level;

  begin("B1 故障码 0 被拒收");
  host_reset_all();

  eq_u32("起始：无故障", AVE_Board_BootFailCode(), 0UL);
  eq_u32("起始：计数为 0", AVE_Board_BootFailCount(), 0UL);

  /* 这就是"想清故障"的写法 —— 必须被当成空操作 */
  AVE_Board_NoteBootFail((uint32_t)AVE_BOARD_BOOT_FAIL_NONE);

  eq_u32("传 0 之后仍然无故障", AVE_Board_BootFailCode(), 0UL);
  eq_u32("传 0 也不该计入次数", AVE_Board_BootFailCount(), 0UL);

  /*
   * 连带验一句"健康时联锁不误触发"。
   * 这一条 L1 已经验过，此处再验是因为**变异的落点不同**：
   * L1 拦的是"极性宏被对调"，这里拦的是"联锁条件写反了"（健康时反而锁死）。
   */
#if AVE_LASER_ACTIVE_HIGH
  on_level = GPIO_PIN_SET;
#else
  on_level = GPIO_PIN_RESET;
#endif
  AVE_Laser_Set(1);
  eq_i32("健康状态下激光仍能点亮（联锁没被误触发）",
         host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN), on_level);
  end();
}

/** B2 先到先得：第一个码被留存，后续不覆盖。 */
static void test_B2_first_code_wins(void)
{
  begin("B2 先到先得（留根因）");
  host_reset_all();

  AVE_Board_NoteBootFail((uint32_t)AVE_BOARD_BOOT_FAIL_HAL);
  eq_u32("第一个码被记下",
         AVE_Board_BootFailCode(), (unsigned long)AVE_BOARD_BOOT_FAIL_HAL);

  /*
   * 模拟"时钟没起来 → 后面一串初始化跟着失败"。
   * 用一个明显的哨兵值，确保断言失败时一眼能看出**是谁覆盖了谁**。
   */
  AVE_Board_NoteBootFail(0xA5A5u);
  eq_u32("第二个码**不得覆盖**第一个（否则根因被症状顶掉）",
         AVE_Board_BootFailCode(), (unsigned long)AVE_BOARD_BOOT_FAIL_HAL);
  end();
}

/** B3 计数不被"先到先得"吞掉。 */
static void test_B3_count_still_counts(void)
{
  unsigned long before;

  begin("B3 计数照常累加");
  host_reset_all();

  before = AVE_Board_BootFailCount();
  is_true("此刻应已处于故障态（前面的用例记过码）", before >= 1UL);

  AVE_Board_NoteBootFail(0xA5A5u);
  eq_u32("再记一次 → 计数 +1", AVE_Board_BootFailCount(), before + 1UL);

  /* 顺手再确认一次：计数在涨，码却没动 —— "次数"和"根因"是两件事 */
  eq_u32("计数在涨，码仍是第一个",
         AVE_Board_BootFailCode(), (unsigned long)AVE_BOARD_BOOT_FAIL_HAL);
  end();
}

/** B4 启动故障后激光点不亮，且拒绝时把引脚按回"关"。 */
static void test_B4_laser_blocked_after_boot_fail(void)
{
  int           off_level;
  int           on_level;
  unsigned long writes;

  begin("B4 启动故障后激光点不亮");
  host_reset_all();

  is_true("前提：故障码已置位", AVE_Board_BootFailCode() != 0UL);

#if AVE_LASER_ACTIVE_HIGH
  off_level = GPIO_PIN_RESET;
  on_level  = GPIO_PIN_SET;
#else
  off_level = GPIO_PIN_SET;
  on_level  = GPIO_PIN_RESET;
#endif

  /*
   * 先把引脚**手动**写成"开"电平，模拟"上电后有人写过 / 联锁生效前就已经是开的"。
   * 这样一来，"拒绝时仍然写一次关电平" 和 "拒绝时直接 return 什么都不做"
   * 就有了可区分的现象 —— 后者会让 host_gpio_last 停在 on_level 上。
   */
  HAL_GPIO_WritePin(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN, on_level);
  writes = host_gpio_writes(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN);

  AVE_Laser_Set(1);
  eq_i32("Set(1) 被按回**关闭**电平（而不是停在原来的开电平）",
         host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN), off_level);
  eq_u32("确实写了一次引脚（不是直接 return 什么都不干）",
         host_gpio_writes(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN), writes + 1UL);

  AVE_Laser_Set(2);   /* 非 0 即开；联锁下同样点不亮 */
  eq_i32("Set(2) 也点不亮",
         host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN), off_level);

  AVE_Laser_Set(0);
  eq_i32("Set(0) 仍是关闭电平",
         host_gpio_last(AVE_LASER_GPIO_PORT, AVE_LASER_GPIO_PIN), off_level);
  end();
}

/* ============================================================================
 * 三之三、视觉层：行解析 → 校验 → 只取最新 → 偏差 → 断流
 *        （M3.2 前半；让 `AVE_Uart_ReadLine()` 第一次有真正的消费者）
 * ==========================================================================*/

/** 只灌字节，不排空。返回灌进去的字节数。 */
static int vs_inject(const char *line_without_eol)
{
  char buf[128];
  int  n;
  int  got;

  n = sprintf(buf, "%s\n", line_without_eol);
  if ((n <= 0) || (n >= (int)sizeof(buf)))
  {
    is_true("用例自己写错了：这一行装不进 vs_inject 的缓冲", 0);
    return 0;
  }

  got = host_uart_inject_str(AVE_HUART_CAM, buf, n);
  is_true("字节全部灌入视觉口", got == n);
  return got;
}

/** 灌一行（不含换行符）→ 排空一次。 */
static void vs_feed(const char *line_without_eol, uint32_t now_ms)
{
  (void)vs_inject(line_without_eol);
  AVE_Vision_Poll(now_ms);
}

/* ============================================================================
 * V 组的面内坐标一律**派生**自 AVE_VISION_SCREEN_W/H，禁止再写死像素值。
 *
 * 🔴 为什么（第六十一次实测发现，这是本工程第三次栽在"写死字面量"上）：
 *    本组原先写死 320 / 240 / 400 / 100 —— 那是 **640×480 时代**的画面中心
 *    与偏心点。2026-10-08 画面改成 **224×224**（`AVE_tuning.h:73`，有实测依据）后，
 *    `320` 与 `240` 全部越界。解析器**正确地**把它们判成 `PARSE_RANGE`，
 *    于是 V1/V2/V3/V5/V6/V7/V8/V10/V12 **九条用例齐红**，而这套用例
 *    从那天起再没被跑过 ⇒
 *      ① 文档里「M3.2 前半视觉层已通过运行时验证」这条结论**当场失效**；
 *      ② 更坏的是**变异测试变成假绿**：M9/M10/M11 期望拦住的正是 V3/V5/V6，
 *         基线本来就红 ⇒ 不论变异有没有生效，脚本都会报「✓ 被预期用例拦住」。
 *         现象上它与真绿**一字不差**，这是比"用例太弱"更难发现的一类错误。
 *    修法**不是**把 320 换成 112（那只是把下一次失效往后推），而是派生：
 *    画面尺寸再变，坐标跟着变，这一组不会第二次静默失效。
 *
 * 另外每条用例加一道 `v_in_screen()` **前提断言**：万一派生出来的坐标真落到
 * 画面外，失败会指名道姓报在"用例前提"上，而不是伪装成产品解析器的锅。
 * ==========================================================================*/

/** 坐标是否落在画面内 —— 给"派生坐标"的用例做前提断言用。 */
static int v_in_screen(int x, int y)
{
  return (x >= 0) && (x < AVE_VISION_SCREEN_W) &&
         (y >= 0) && (y < AVE_VISION_SCREEN_H);
}

/** 组装一行 `cx,cy,flag` 再喂进去。V 组用它，避免再出现裸像素字面量。 */
static void vs_feed_xyz(int cx, int cy, int flag, uint32_t now_ms)
{
  char s[32];

  (void)sprintf(s, "%d,%d,%d", cx, cy, flag);
  vs_feed(s, now_ms);
}

/* 偏心量 40 / 70：小到在 224 画面里宽松够用，且**两值不相等** ——
   V2 要的就是"两轴偏差非零且不等"，相等时一个轴写反了也看不出来。 */
#define V_OFF_X   40
#define V_OFF_Y   70

/* 明显越界的坐标：**由画面尺寸推出来**，画面变大也不会突然变成合法值。 */
#define V_OOR_X   (AVE_VISION_SCREEN_W + 100)
#define V_OOR_Y   (AVE_VISION_SCREEN_H + 100)

/* V5 / V12 那一串连续帧的 cx 起点，cy 恒为画面中心。 */
#define V_SEQ_CX0 (AVE_VISION_CENTER_X - 5)

/** V1 一帧落在画面正中心：偏差必须是 0，且状态机认它是合法目标帧。 */
static void test_V1_basic_frame(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;
  int                    ex = 999;
  int                    ey = 999;

  begin("V1 基本帧（画面正中心）（M3.2 ①）");
  fresh();

  is_true("用例前提：中心坐标在画面内",
          v_in_screen(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y));
  vs_feed_xyz(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y, 1, 1000U);
  st = AVE_Vision_State();

  eq_u32("ok 计数", st->ok, 1UL);
  eq_u32("没有协议错", st->proto_err, 0UL);
  eq_u32("没有范围错", st->range_err, 0UL);
  eq_u32("无目标帧为 0", st->no_target, 0UL);
  eq_u32("没有被判成断流", st->stale, 0UL);

  is_true("有帧可读", AVE_Vision_Latest(&f) == 1);
  eq_i32("kind 是 OK", (long)f.kind, (long)AVE_VISION_KIND_OK);
  eq_i32("cx", (long)f.cx, (long)AVE_VISION_CENTER_X);
  eq_i32("cy", (long)f.cy, (long)AVE_VISION_CENTER_Y);
  eq_u32("本帧时刻", f.tick_ms, 1000UL);
  eq_u32("序号从 1 起", f.seq, 1UL);
  eq_u32("last_frame_ms 被刷新", st->last_frame_ms, 1000UL);

  is_true("偏差可用", AVE_Vision_Deviation(&f, &ex, &ey) == 1);
  eq_i32("正中心：水平偏差 0", (long)ex, 0L);
  eq_i32("正中心：俯仰偏差 0", (long)ey, 0L);
  end();
}

/**
 * V2 §4.3 纵轴取反 —— **照字面写"画面正中心"是恒真弱判据**：
 *    居中时两个偏差都是 0，符号写反照样通过。所以这里必须用偏心帧，
 *    而且两个方向的差都要是**非零且不等**，一个轴写错就露。
 *
 * ⚠️ 输入坐标由宏**派生**（跟着画面中心走），但**期望值仍写字面量 40 / 70**：
 *    若期望值也从宏推（例如写成 `CENTER_Y - cy`），就等于把产品公式抄进判据，
 *    符号写反照样通过 —— 那正是本用例存在的理由。
 */
static void test_V2_deviation_signs(void)
{
  AVE_VisionFrame f;
  int              ex;
  int              ey;

  begin("V2 偏心帧 → err_x=+40 且 err_y=+70（§4.3 纵轴取反）");
  fresh();

  is_true("用例前提：两个偏移量不相等（相等时一个轴写反也看不出来）",
          V_OFF_X != V_OFF_Y);
  is_true("用例前提：第一帧在画面内",
          v_in_screen(AVE_VISION_CENTER_X + V_OFF_X, AVE_VISION_CENTER_Y - V_OFF_Y));

  vs_feed_xyz(AVE_VISION_CENTER_X + V_OFF_X, AVE_VISION_CENTER_Y - V_OFF_Y, 1, 1000U);
  is_true("有帧", AVE_Vision_Latest(&f) == 1);
  is_true("偏差可用", AVE_Vision_Deviation(&f, &ex, &ey) == 1);
  /* x: (中心+40) - 中心 = +40。y: 中心 - (中心-70) = +70 —— 屏幕 y 向下、云台俯仰向上，取反 */
  eq_i32("水平偏差 = cx - 中心", (long)ex, (long)V_OFF_X);
  eq_i32("俯仰偏差 = 中心 - cy（取反）", (long)ey, (long)V_OFF_Y);

  /* 反方向再来一帧：把符号在两个方向上都钉住，而不是只钉一个象限 */
  is_true("用例前提：第二帧在画面内",
          v_in_screen(AVE_VISION_CENTER_X - V_OFF_X, AVE_VISION_CENTER_Y + V_OFF_Y));
  vs_feed_xyz(AVE_VISION_CENTER_X - V_OFF_X, AVE_VISION_CENTER_Y + V_OFF_Y, 1, 2000U);
  is_true("有帧", AVE_Vision_Latest(&f) == 1);
  is_true("偏差可用", AVE_Vision_Deviation(&f, &ex, &ey) == 1);
  eq_i32("水平偏差为负", (long)ex, (long)(-V_OFF_X));
  eq_i32("俯仰偏差为负", (long)ey, (long)(-V_OFF_Y));
  end();
}

/** V3 越界坐标（M3.2 ③ / §5.6）：判错、不发布、**不刷新**断流计时。 */
static void test_V3_out_of_range(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;

  begin("V3 越界坐标（flag=1）→ 协议错且不刷新（M3.2 ③）");
  fresh();

  vs_feed_xyz(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y, 1, 1000U);  /* 先立一个"上一帧合法"的基准 */
  is_true("用例前提：越界坐标确实越界", !v_in_screen(V_OOR_X, V_OOR_Y));
  vs_feed_xyz(V_OOR_X, V_OOR_Y, 1, 2000U);                          /* 再来一帧越界的 */

  st = AVE_Vision_State();
  eq_u32("协议错 +1", st->proto_err, 1UL);
  eq_u32("范围错 +1（细分计数）", st->range_err, 1UL);
  eq_u32("ok 不涨", st->ok, 1UL);

  is_true("发布的仍是上一帧合法的",
          (AVE_Vision_Latest(&f) == 1) &&
          (f.cx == AVE_VISION_CENTER_X) && (f.cy == AVE_VISION_CENTER_Y));
  eq_u32("last_frame_ms 停在上一帧合法的时刻", st->last_frame_ms, 1000UL);

  /*
   * 越界帧不刷新计时的**可观测后果**：这一轮排空时 now=2000，
   * 距上一帧合法时刻已 1000 毫秒 > 200 毫秒超时 → 应当转入断流态，
   * 而且 stale_events 恰好 +1。若有人把越界分支改成"也刷新了"，
   * 这两个数会变成 0 / 0 —— 这就是那条"不刷新"真正的守卫。
   */
  eq_u32("越界帧没能骗过断流检测：已转断流", st->stale, 1UL);
  eq_u32("断流跳变恰好 +1", st->stale_events, 1UL);
  end();
}

/**
 * V4 §6.3 硬约束 3 的**唯一守卫** —— 无目标帧必须同样刷新断流计时。
 *
 * 单发一次 `-1,-1,0` 是测不出这条的（那一轮时间差为 0，怎么都不会算断流）。
 * 必须**连发几十帧、累计跨过 200 毫秒**，才逼出"不刷新"这个错。
 */
static void test_V4_no_target_keeps_alive(void)
{
  const AVE_VisionState *st;
  int                    i;

  begin("V4 连发 60 帧 -1,-1,0 跨过 200ms → 不判断流（§6.3 硬约束 3）");
  fresh();

  /* 每 20 毫秒一帧，共 60 帧 = 1200 毫秒，是 200 毫秒超时的 6 倍 */
  for (i = 0; i < 60; i++)
  {
    vs_feed("-1,-1,0", 20U * (uint32_t)(i + 1));
  }

  st = AVE_Vision_State();
  eq_u32("无目标帧 60 个", st->no_target, 60UL);
  eq_u32("ok 不涨（它们不是目标帧）", st->ok, 0UL);
  eq_u32("一个协议错都没有", st->proto_err, 0UL);
  eq_u32("**没有**被判成断流", st->stale, 0UL);
  eq_u32("一次断流跳变都没有", st->stale_events, 0UL);
  eq_u32("last_frame_ms 被刷到了最后一帧", st->last_frame_ms, 1200UL);
  end();
}

/** V5 M3.2 ⑤：一次排空里 10 行，**每行都计数**，但只有最后一行用于控制。 */
static void test_V5_only_latest_published(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;
  char                   s[16];
  int                    i;

  begin("V5 一次排空 10 行 → 全计数、只发布最后一行（M3.2 ⑤）");
  fresh();

  /*
   * 🔴 关键：10 行必须在**同一次 Poll**里被排掉。
   *    逐行"灌一行排空一次"的话，每次 Poll 都只看见一行 ——
   *    那测的是"单行解析"（V1 已经测过），根本没碰"只取最新"。
   */
  is_true("用例前提：这一串 cx 都在画面内",
          v_in_screen(V_SEQ_CX0, AVE_VISION_CENTER_Y) &&
          v_in_screen(V_SEQ_CX0 + 9, AVE_VISION_CENTER_Y));
  for (i = 0; i < 10; i++)
  {
    (void)sprintf(s, "%d,%d,1", V_SEQ_CX0 + i, AVE_VISION_CENTER_Y);
    (void)vs_inject(s);
  }
  AVE_Vision_Poll(1000U);

  st = AVE_Vision_State();
  eq_u32("10 行都被解析（没有静默丢帧）", st->ok, 10UL);
  eq_u32("没有协议错", st->proto_err, 0UL);

  is_true("有帧", AVE_Vision_Latest(&f) == 1);
  eq_i32("发布的是**最后一行**的 cx", (long)f.cx, (long)(V_SEQ_CX0 + 9));
  eq_u32("序号是 10", f.seq, 10UL);
  eq_u32("本帧时刻是这一轮的时刻", f.tick_ms, 1000UL);
  end();
}

/** V6 断流只在**跳变**那一刻计数，不是每轮都加。 */
static void test_V6_stale_counts_transitions(void)
{
  const AVE_VisionState *st;
  int                    i;

  begin("V6 断流态下连 Poll 30 次 → stale_events 恰好 1");
  fresh();

  vs_feed_xyz(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y, 1, 100U);
  eq_u32("前提：此刻未断流", AVE_Vision_State()->stale, 0UL);

  /* 之后再不喂数据，50 赫兹地空转 600 毫秒 */
  for (i = 0; i < 30; i++)
  {
    AVE_Vision_Poll(200U + (uint32_t)(i * 20));
  }

  st = AVE_Vision_State();
  eq_u32("当前处于断流态", st->stale, 1UL);
  eq_u32("跳变次数恰好 1（不是 30）", st->stale_events, 1UL);
  eq_u32("last_frame_ms 仍是那一帧的时刻", st->last_frame_ms, 100UL);
  end();
}

/** V7 M3.2 ④：畸形行整行丢弃，且**不影响后续解析**。 */
static void test_V7_malformed_then_recover(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;

  begin("V7 abc,def 整行丢弃、其后一行照常（M3.2 ④）");
  fresh();

  vs_feed("abc,def", 500U);
  st = AVE_Vision_State();
  eq_u32("协议错 +1", st->proto_err, 1UL);
  eq_u32("ok 仍为 0", st->ok, 0UL);
  eq_u32("没有被判成范围错", st->range_err, 0UL);
  eq_u32("没有帧发布", (unsigned long)AVE_Vision_Latest(&f), 0UL);
  eq_i32("没帧时 kind 是 NONE", (long)f.kind, (long)AVE_VISION_KIND_NONE);
  eq_u32("last_frame_ms 仍是 0（从没收到过）", st->last_frame_ms, 0UL);

  vs_feed_xyz(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y, 1, 520U);
  st = AVE_Vision_State();
  eq_u32("后续行照常被接受", st->ok, 1UL);
  eq_u32("协议错没有再涨", st->proto_err, 1UL);
  is_true("有帧", AVE_Vision_Latest(&f) == 1);
  eq_i32("坐标正确", (long)f.cx, (long)AVE_VISION_CENTER_X);
  eq_u32("last_frame_ms 被刷新", st->last_frame_ms, 520UL);
  end();
}

/** V8 文法与判定表逐行 —— 这一条覆盖的是"同一个机制兜住所有畸形输入"。 */
static void test_V8_grammar_table(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;
  uint32_t               proto_before;
  uint32_t               nt_before;
  uint32_t               ok_before;
  char                   s[48];

  begin("V8 判定表逐行（合法/畸形/无目标/空行）");
  fresh();

  /* ── 合法写法 ── */
  is_true("用例前提：下面三条的坐标都在画面内",
          v_in_screen(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y));
  (void)sprintf(s, "  %d , %d , 1  ", AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y);
  vs_feed(s, 10U);                       /* 字段间与首尾容忍空白 */
  (void)sprintf(s, "+%d,%d,1", AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y);
  vs_feed(s, 20U);                       /* 允许前导 '+' */
  (void)sprintf(s, "-0,%d,1", AVE_VISION_CENTER_Y);
  vs_feed(s, 30U);                       /* -0 就是 0 */
  st = AVE_Vision_State();
  eq_u32("三条都算合法目标帧", st->ok, 3UL);
  is_true("有帧", AVE_Vision_Latest(&f) == 1);
  eq_i32("最后一条的 cx 是 -0 → 0", (long)f.cx, 0L);

  /* ── 语法错：整行丢弃，一条都不许漏 ──
     下面的畸形行**故意**保留 640 时代的数字：它们无论坐标是否越界都该判 FORMAT，
     属于"内容无关"的输入，**不是**陈旧字面量，别跟着上面一起改。 */
  proto_before = st->proto_err;
  vs_feed("320,240,1,", 40U);            /* 第三个整数之后还有逗号 */
  vs_feed("320,,1", 50U);                /* 空字段 */
  vs_feed(",,1", 60U);                   /* 两个空字段 */
  vs_feed("320,240", 70U);               /* 只有两个整数 */
  vs_feed("320,240,2", 80U);             /* flag 不在 {0,1} */
  vs_feed("+", 90U);                     /* 孤零零的符号 */
  vs_feed("320,240,-1", 95U);            /* 负的 flag */
  st = AVE_Vision_State();
  eq_u32("七条语法错一条不漏", st->proto_err, proto_before + 7UL);
  eq_u32("其中没有一条落到范围错", st->range_err, 0UL);
  eq_u32("ok 一点没涨", st->ok, 3UL);

  /* ── 无目标：合法帧，不算错 ── */
  nt_before = st->no_target;
  vs_feed("-1,-1,0", 100U);
  st = AVE_Vision_State();
  eq_u32("合规的无目标帧 +1", st->no_target, nt_before + 1UL);
  eq_u32("它不算协议错", st->proto_err, proto_before + 7UL);
  eq_u32("它刷新了 last_frame_ms", st->last_frame_ms, 100UL);
  is_true("发布的 kind 是 NO_TARGET",
          (AVE_Vision_Latest(&f) == 1) && (f.kind == AVE_VISION_KIND_NO_TARGET));

  /* ── flag==0 却带坐标：越规但无害，单独计数、不当错 ── */
  vs_feed_xyz(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y, 0, 110U);
  st = AVE_Vision_State();
  eq_u32("仍算无目标帧", st->no_target, nt_before + 2UL);
  eq_u32("单独计入 flag0_coords", st->flag0_coords, 1UL);
  eq_u32("没有判成协议错", st->proto_err, proto_before + 7UL);
  is_true("坐标被归一成 -1,-1",
          (AVE_Vision_Latest(&f) == 1) && (f.cx == -1) && (f.cy == -1));
  eq_u32("它也刷新了 last_frame_ms", st->last_frame_ms, 110UL);

  /* ── flag==0 但坐标越界：必须判错，否则 §5.6 被判据旁路 ── */
  vs_feed_xyz(V_OOR_X, V_OOR_Y, 0, 120U);
  st = AVE_Vision_State();
  eq_u32("越界的 flag==0 也判协议错", st->proto_err, proto_before + 8UL);
  eq_u32("且计入范围错", st->range_err, 1UL);
  eq_u32("**不**计入 flag0_coords（它根本没被当成无目标帧）", st->flag0_coords, 1UL);
  eq_u32("last_frame_ms 不刷新", st->last_frame_ms, 110UL);

  /* ── 空行 / 纯空白行：忽略、不算错、也**不**刷新断流计时 ── */
  proto_before = st->proto_err;
  vs_feed("", 130U);
  vs_feed("   ", 140U);
  vs_feed("\t", 150U);
  st = AVE_Vision_State();
  eq_u32("三条空行被记下", st->blank, 3UL);
  eq_u32("空行不算协议错", st->proto_err, proto_before);
  eq_u32("空行**不**刷新 last_frame_ms（否则 \\n 洪泛能骗过断流检测）",
         st->last_frame_ms, 110UL);

  /* ── 纯函数直调：分类码与状态机分离 ── */
  ok_before = st->ok;
  (void)sprintf(s, "%d,%d,1", AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y);
  eq_i32("直调：合法帧", (long)AVE_Vision_ParseLine(s, &f), (long)AVE_VISION_PARSE_OK);
  eq_i32("直调：无目标", (long)AVE_Vision_ParseLine("-1,-1,0", &f), (long)AVE_VISION_PARSE_NO_TARGET);
  eq_i32("直调：语法错", (long)AVE_Vision_ParseLine("abc", &f), (long)AVE_VISION_PARSE_FORMAT);
  (void)sprintf(s, "%d,%d,1", V_OOR_X, V_OOR_Y);
  eq_i32("直调：范围错", (long)AVE_Vision_ParseLine(s, &f), (long)AVE_VISION_PARSE_RANGE);
  eq_i32("直调：空行", (long)AVE_Vision_ParseLine("   ", &f), (long)AVE_VISION_PARSE_BLANK);
  eq_u32("直调不碰任何计数器（纯函数）", AVE_Vision_State()->ok, ok_before);
  end();
}

/** V9 位数上限 9：十位数字必须判语法错（不给整数溢出留门）。 */
static void test_V9_digit_limit(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;
  char                   s[48];

  begin("V9 十位数字被判语法错（位数上限 9）");
  fresh();

  (void)sprintf(s, "9999999999,%d,1", AVE_VISION_CENTER_Y);   /* 10 位 —— 必须拒 */
  vs_feed(s, 100U);
  st = AVE_Vision_State();
  eq_u32("十位数字判语法错", st->proto_err, 1UL);
  eq_u32("没有落到范围错（它根本没被当成数字）", st->range_err, 0UL);
  eq_u32("没有帧发布", (unsigned long)AVE_Vision_Latest(&f), 0UL);

  /*
   * 九位仍然合法。这里**用纯函数直调**而不是走状态机：范围错会同时计入
   * `proto_err` 与 `range_err`（有意设计，见 handle_line），走状态机的话
   * `proto_err` 从 1 变 2，就分不清"又一条语法错"和"这一条是范围错"了。
   */
  (void)sprintf(s, "999999999,%d,1", AVE_VISION_CENTER_Y);
  eq_i32("九位通过语法检查，分类是范围错",
         (long)AVE_Vision_ParseLine(s, &f), (long)AVE_VISION_PARSE_RANGE);
  (void)sprintf(s, "9999999999,%d,1", AVE_VISION_CENTER_Y);
  eq_i32("十位连语法都过不去",
         (long)AVE_Vision_ParseLine(s, &f), (long)AVE_VISION_PARSE_FORMAT);

  /* 状态机路径也确认一次：九位那行走下去只会涨 range_err */
  (void)sprintf(s, "999999999,%d,1", AVE_VISION_CENTER_Y);
  vs_feed(s, 200U);
  st = AVE_Vision_State();
  eq_u32("越界 → 范围错 +1", st->range_err, 1UL);
  eq_u32("proto_err 是「1 条语法错 + 1 条范围错」", st->proto_err, 2UL);
  end();
}

/** V10 行超过 `AVE_VISION_LINE_MAX` → `line_spilled`（ENOSPC 路径）。 */
static void test_V10_line_spilled(void)
{
  const AVE_VisionState *st;
  char                   s[64];

  begin("V10 超长行 → line_spilled（ENOSPC 路径）");
  fresh();

  /*
   * 内容**本身完全合法**，只是后面缀了一堆空白把行撑长。
   * 这样断言才有指向：丢它不是因为内容错，而是因为行级的长度上限。
   * 长度按 `AVE_VISION_LINE_MAX + 8` 现算，不写死 40 —— 上限若改，这里跟着改。
   */
  (void)sprintf(s, "%d,%d,1", AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y);
  while (strlen(s) < (size_t)(AVE_VISION_LINE_MAX + 8))
  {
    (void)strcat(s, " ");
  }
  is_true("用例前提：这行确实超过了行长度上限",
          (int)strlen(s) > AVE_VISION_LINE_MAX);
  vs_feed(s, 100U);

  st = AVE_Vision_State();
  eq_u32("超长行计入 line_spilled", st->line_spilled, 1UL);
  eq_u32("整行被丢弃，没算成合法帧", st->ok, 0UL);
  eq_u32("也没算成协议错（它是行级的，不是帧级的）", st->proto_err, 0UL);
  eq_u32("没有帧发布", st->last_frame_ms, 0UL);

  /* 长度刚好在 cap 内的同一内容必须照常通过 —— 证明上面拦的是长度，不是内容 */
  vs_feed_xyz(AVE_VISION_CENTER_X, AVE_VISION_CENTER_Y, 1, 200U);
  st = AVE_Vision_State();
  eq_u32("同样内容、长度合规 → 正常接受", st->ok, 1UL);
  eq_u32("line_spilled 不再涨", st->line_spilled, 1UL);
  end();
}

/** V11 `[VS]` 行的宽度是**结构性保证**的：最坏情况也塞得进日志预算。 */
static void test_V11_vs_line_width(void)
{
  AVE_VisionState st;
  AVE_VisionFrame f;
  char            line[AVE_LOG_LINE_MAX];
  int             n;

  begin("V11 [VS] 行最坏宽度 ≤ 预算（实测 82 / 预算 98）");
  fresh();

  /*
   * 构造"结构最坏"：`t` 满 10 位 + 4 个计数**超过封顶值** + 坐标取最大。
   *
   * ⚠️ 计数必须填到**超过** `AVE_VISION_PRINT_CAP`，不能正好等于它 ——
   *    正好等于 99999999 时，封顶与不封顶渲染出的字节数**一模一样**，
   *    用例就完全看不见封顶有没有生效（第一版就是这么写的，
   *    结果"去掉封顶"那条变异体安然全过）。
   */
  memset(&st, 0, sizeof(st));
  st.stale        = 1U;                 /* 0/1 标志，不是计数 —— 只占 1 位 */
  st.stale_events = 4294967295UL;
  st.ok           = 4294967295UL;
  st.proto_err    = 4294967295UL;
  st.line_spilled = 4294967295UL;

  memset(&f, 0, sizeof(f));
  f.cx   = AVE_VISION_SCREEN_W - 1;
  f.cy   = AVE_VISION_SCREEN_H - 1;
  f.kind = AVE_VISION_KIND_OK;

  n = AVE_Vision_Format(line, (int)sizeof(line), 4294967295UL, &st, &f);

  is_true("渲染成功（返回长度合法）", n > 0);
  is_true("长度不超过 AVE_Log 的正文预算（否则行尾被砍、最新的字段死亡率 100%）",
          (n > 0) && (n <= AVE_LOG_LINE_MAX - 2));

  /*
   * 钉住**算出来的那个数**（82），而不是只钉一个上界。
   *
   * 理由：这个数字是宽度论证的载体 —— 往格式串里加字段的人必须重新算它，
   * 而"只钉 <= 98"时加到 95 也不会报错，下一次再加就穿了。钉死等于把
   * 文档里那个 82 变成机器可校验的契约。
   *
   * ⚠️ 它**同时**钉住了"结构最坏值没算错"这件事：本工程已经算错过一次
   *    （把 `st` 这个 0/1 标志当成 8 位计数，得出 89）—— 那个错正是这行
   *    用例抓出来的。将来的修改若让这行不等于 82，请**先重新算一遍**
   *    再改这个数，别顺手把期望改成实测值。
   */
  eq_i32("结构最坏渲染长度正好是文档里那个数", (long)n, 82L);
  printf("       （实测最坏 = %d，正文预算 = %d）\n", n, AVE_LOG_LINE_MAX - 2);

  /* 渲染出来的内容里不能有截断标记 */
  is_true("行内没有截断标记 '~'", strchr(line, '~') == 0);
  end();
}

/** V12 排空循环有硬上限：触顶要计数，剩下的留给下一轮（决策 6）。 */
static void test_V12_backlog_bounded(void)
{
  const AVE_VisionState *st;
  AVE_VisionFrame        f;
  char                   s[16];
  int                    i;

  begin("V12 一次灌入 > 上限的行 → backlog 且剩余下次处理（决策 6）");
  fresh();

  is_true("用例前提：这一串 cx 都在画面内",
          v_in_screen(V_SEQ_CX0, AVE_VISION_CENTER_Y) &&
          v_in_screen(V_SEQ_CX0 + AVE_VISION_MAX_LINES_PER_POLL + 4, AVE_VISION_CENTER_Y));
  for (i = 0; i < (AVE_VISION_MAX_LINES_PER_POLL + 5); i++)
  {
    (void)sprintf(s, "%d,%d,1", V_SEQ_CX0 + i, AVE_VISION_CENTER_Y);
    (void)vs_inject(s);
  }

  AVE_Vision_Poll(1000U);
  st = AVE_Vision_State();
  eq_u32("本轮只处理了上限那么多行", st->ok, (unsigned long)AVE_VISION_MAX_LINES_PER_POLL);
  eq_u32("触顶被记下来", st->backlog, 1UL);

  AVE_Vision_Poll(1020U);
  st = AVE_Vision_State();
  eq_u32("剩下的 5 行在下一轮处理",
         st->ok, (unsigned long)(AVE_VISION_MAX_LINES_PER_POLL + 5));
  eq_u32("这一轮没触顶，backlog 不再涨", st->backlog, 1UL);
  is_true("有帧", AVE_Vision_Latest(&f) == 1);
  eq_i32("发布的是最后一行的 cx",
         (long)f.cx, (long)(V_SEQ_CX0 + AVE_VISION_MAX_LINES_PER_POLL + 4));
  end();
}

/* ============================================================================
 * 四、入口
 * ==========================================================================*/

int main(void)
{
  printf("=== M1.3 运行时自检（宿主机 / 仿真 HAL）===\n");
  /*
   * 打印画面尺寸是有用的：V 组的坐标由它派生，它一变整组行为就变。
   * 让它出现在输出最顶端，"用例为什么红/绿"一眼能和它对上。
   */
  printf("编译期配置：AVE_UART_RX_BUF_SIZE=%d  AVE_LASER_ACTIVE_HIGH=%d  "
         "画面=%d×%d  行上限=%d\n\n",
         (int)AVE_UART_RX_BUF_SIZE, (int)AVE_LASER_ACTIVE_HIGH,
         (int)AVE_VISION_SCREEN_W, (int)AVE_VISION_SCREEN_H,
         (int)AVE_VISION_LINE_MAX);

  printf("-- 环形缓冲本体 --\n");
  test_R1_fill_no_overwrite();
  test_R2_wraparound_order();
  test_R3_counter_overflow();
  test_R4_find_semantics();
  test_R5_read_skip_bounds();

  printf("-- 串口层 --\n");
  test_U1_basic_line();
  test_U2_crlf();
  test_U3_accept_800_bytes();
  test_U4_three_lines();
  test_U5_counters_stay_zero();
  test_U6_escape_path_no_deadlock();
  test_U7_overflow_counter();
  test_U8_line_too_long_for_caller();
  test_U9_argument_validation();
  test_U10_error_recovery();
  test_U11_write_and_isolation();
  test_U12_sustained_rearm();

  printf("-- 视觉层（M3.2 前半）--\n");
  test_V1_basic_frame();
  test_V2_deviation_signs();
  test_V3_out_of_range();
  test_V4_no_target_keeps_alive();
  test_V5_only_latest_published();
  test_V6_stale_counts_transitions();
  test_V7_malformed_then_recover();
  test_V8_grammar_table();
  test_V9_digit_limit();
  test_V10_line_spilled();
  test_V11_vs_line_width();
  test_V12_backlog_bounded();

  printf("-- 板级 --\n");
  test_L1_laser_polarity();

  /*
   * 🔴 顺序即判据：B 组会给故障码置位且没有清除接口，
   *    所以必须在 L1（要验"健康时能点亮"）**之后**跑。理由见上面那一节的注释。
   */
  printf("-- 板级：启动故障记录 + 激光联锁 --\n");
  test_B1_zero_code_rejected();
  test_B2_first_code_wins();
  test_B3_count_still_counts();
  test_B4_laser_blocked_after_boot_fail();

  printf("\n=== 用例 %d 通过 / %d 失败 ===\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}
