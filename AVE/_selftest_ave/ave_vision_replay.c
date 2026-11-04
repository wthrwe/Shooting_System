/**
 * 跨端契约测试的【固件侧执行体】—— 把视觉端真正发出来的字节流喂给产品解析器。
 *
 * ── 它负责什么 ──────────────────────────────────────────────────────────────
 *   只做一件事：把文件里的原始字节**逐字节**灌进 `AVE_UART_CAM` 通道
 *   （与真实 UART 中断里"一个字节一个字节进来"的到达方式一致），
 *   每攒够 `CT_CHUNK` 个字节就按 20 毫秒一拍调用**真产品函数**
 *   `AVE_Vision_Poll()`，并把每拍的 `[VS]` 行打到 stdout ——
 *   那行是 `AVE_Vision_Report()` 渲染的，与板子串口上出现的**一字不差**。
 *   最后打一行 `[CT]` 汇总，把全部计数器**如实报出来**（不判断好坏）。
 *
 * ── 它【不】负责什么（这一条很重要）─────────────────────────────────────────
 *   **它不做任何断言。判断对错在 `test_contract_vision.py` 里。**
 *   理由：判据必须能失败、且失败要有指向。Python 侧同时掌握"文件里到底有多少行、
 *   每行长什么样"这些**独立于固件**的事实，于是两侧各自从契约（plan.md §6.3）
 *   独立推导，才对得起"跨端契约测试"这个名字；固件侧一旦自己判分，
 *   就变成"自己出题自己判卷"。所以这里只当镜子。
 *
 * ── 为什么必须是真字节 + 真解析器 ───────────────────────────────────────────
 *   在 Python 里照抄一遍解析逻辑，测的是**我抄得像不像**，不是产品对不对。
 *   这里链接的是工程里那份 `AVE/AVE_vision.c`，**一个字节都没改**
 *   （与 `ave_m13_runtime.c` 共用同一套仿真 HAL 与宿主 shim）。
 *   字节则来自 `maixpy/v5_send.py` 本体（见 Python 侧的说明）。
 *
 * ── ⬜ 它【不】证明什么 ─────────────────────────────────────────────────────
 *   ⬜ 不证明 MaixCAM 真的在发这些字节、不证明 115200 的实际误码率、
 *      不证明 USART1 的线接对了、不证明真实相机/模型会给出什么框。
 *      它证明的是：「**这一串**字节，固件全部收下、且零投诉」。
 *
 * ── 用法 ────────────────────────────────────────────────────────────────────
 *   ave_vision_replay.exe <字节文件路径>
 *   返回 0 = 跑完（**不代表通过**，通过与否由 Python 侧判）；
 *   返回 2 = 参数/文件错误；返回 3 = 灌字节时环形缓冲满（本次结论无效）。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "AVE_config.h"
#include "AVE_uart.h"
#include "AVE_vision.h"
#include "fake_hal.h"

#include <stdio.h>

/* 每攒够这么多字节排空一次。
   取 64：一行最长 32 字节，64 够攒下 2 行；又远小于 1024 的环形缓冲，
   保证**永远**不会因为灌得太猛而丢字节。
   ⚠️ 丢字节会让"全部收下"这个结论变成一句空话，所以下面还专门查了
      `drop_bytes` / `drop_lines`，并在注入失败时**直接返回 3** ——
      宁可报"结论无效"，也不报一个看起来通过的绿。 */
#define CT_CHUNK 64L

/* 合成时钟：从 1000 毫秒起算（非零，便于把"从没收到过帧"的 0 与真时刻分开），
   每排空一拍 +20 毫秒 —— 与 plan.md §6.3 建议的 20~30 Hz 一致。 */
static uint32_t g_t_ms = 1000U;

/** 排空一拍，并把这一拍之后的 `[VS]` 行打出来（用的就是产品自己的渲染函数）。 */
static void ct_poll(void)
{
  char buf[AVE_LOG_LINE_MAX];

  AVE_Vision_Poll(g_t_ms);
  if (AVE_Vision_Report(buf, (int)sizeof(buf), g_t_ms) > 0)
  {
    /* ⚠️ 这里只补一个 '\n'，**不要**改写成 `fputs` 或手写 "\r\n" ——
     *    本轮实测（第六十一次）：
     *      · `AVE_Vision_Format()` 渲染出的正文**不含任何行尾**（实测 47 字节，
     *        末尾就是 `f=...,...` 的最后一个数字）；
     *      · 落盘后每行却是 `\r\n` 结尾 —— 那个 `\r` 是 **MinGW 文本模式把
     *        `\n` 翻译成 CRLF** 的结果（Windows 上的宿主产物，板子上没有这回事）。
     *    两个后果：
     *      1. Python 侧的判据必须容忍这个 `\r`，否则一条 `[VS]` 都匹配不上
     *         （本轮真踩了：`(\d+)$` 因 `\r` 卡住，B12 报"found 0"）；
     *      2. 谁要是"顺手"改成手写 `"\r\n"`，会得到 `\r\r\n`；
     *         改成 `fputs` 不带换行，则所有行会连成一坨。
     */
    printf("%s\n", buf);
  }
  g_t_ms += 20U;
}

int main(int argc, char **argv)
{
  const AVE_VisionState *st;
  const AVE_UartStats   *us;
  AVE_VisionFrame        f;
  FILE                  *fp;
  long                   nbytes = 0;
  long                   nlines = 0;

  if (argc < 2)
  {
    fprintf(stderr, "用法: %s <视觉端字节文件>\n", argv[0]);
    return 2;
  }

  fp = fopen(argv[1], "rb");
  if (fp == NULL)
  {
    fprintf(stderr, "打不开文件: %s\n", argv[1]);
    return 2;
  }

  /* 与 ave_m13_runtime.c 的 fresh() 同义：可视化层的静态计数器必须复位，
     否则本程序被重复调用时会累加上一次的结果（现象是"跑第二遍数字翻倍"）。 */
  host_reset_all();
  AVE_Uart_Init();
  AVE_Vision_Init();

  for (;;)
  {
    int c = fgetc(fp);

    if (c == EOF)
    {
      break;
    }
    nbytes++;
    if (c == '\n')
    {
      nlines++;
    }

    /* 一次灌一个字节 —— 与 HAL 的逐字节收中断同构。
       返回非 0 表示环形缓冲已满、这个字节被丢了：那"全部收下"就是假的，
       所以这里**不当成小事**，直接终止并让退出码说话。 */
    if (host_uart_inject(AVE_HUART_CAM, (uint8_t)c) != 0)
    {
      fprintf(stderr,
              "[CT-ERR] 第 %ld 个字节注入失败（环形缓冲满）—— 本次结论无效，不算通过\n",
              nbytes);
      (void)fclose(fp);
      return 3;
    }

    if ((nbytes % CT_CHUNK) == 0L)
    {
      ct_poll();
    }
  }
  (void)fclose(fp);

  /* 收尾一拍：不足 CT_CHUNK 的尾巴也要被排空，否则最后几帧根本没被解析，
     而 summary 会显示"少了几帧"——那种失败会把人往解析器上引，其实是仪器少踩了一脚。 */
  ct_poll();

  st = AVE_Vision_State();
  us = AVE_Uart_Stats(AVE_UART_CAM);
  (void)AVE_Vision_Latest(&f);

  /* 汇总：只报事实。`line_spilled` 与 `ring_flushed` 都是"这一层没收下"的证据，
     必须一起报 —— 只看 proto_err 会把"整行被行长度上限丢掉"漏过去。 */
  printf("[CT] bytes=%ld lines=%ld ok=%lu no_target=%lu flag0=%lu proto_err=%lu"
         " range_err=%lu line_spilled=%lu blank=%lu backlog=%lu ring_flushed=%lu"
         " stale=%lu stale_events=%lu drop_bytes=%lu drop_lines=%lu"
         " last=%d,%d,%d seq=%lu\n",
         nbytes, nlines,
         (unsigned long)st->ok, (unsigned long)st->no_target,
         (unsigned long)st->flag0_coords, (unsigned long)st->proto_err,
         (unsigned long)st->range_err, (unsigned long)st->line_spilled,
         (unsigned long)st->blank, (unsigned long)st->backlog,
         (unsigned long)st->ring_flushed, (unsigned long)st->stale,
         (unsigned long)st->stale_events,
         (unsigned long)((us != NULL) ? us->drop_bytes : 0UL),
         (unsigned long)((us != NULL) ? us->drop_lines : 0UL),
         f.cx, f.cy, (int)f.kind, (unsigned long)f.seq);
  return 0;
}
