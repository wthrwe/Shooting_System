/* AVE_log.c —— 调试日志实现：vsnprintf 到定长栈缓冲，再交给非阻塞发送 */
/* 语义：日志给控制让路，一行阻塞调用都没有，发送忙就丢并计数 */
#include "AVE_log.h"

#include <stdarg.h>
#include <stdio.h>      /* vsnprintf */

/* 编译期静态断言：行缓冲至少装得下 "\r\n"（2 字节）加一个截断标记（1 字节） */
/* AVE_tuning.h 已保证 AVE_LOG_LINE_MAX >= 8，本文件的钳位/截断推理都依赖它 */
typedef char ave_log_chk_line_max[(AVE_LOG_LINE_MAX >= 8) ? 1 : -1];
/**
  * 函    数：格式化一条日志并按 \r\n 结尾非阻塞发出
  * 参    数：fmt 格式串（用法同 printf，变参）
  * 返 回 值：>0 已受理字节数；AVE_UART_EBUSY 上一笔未发完、本条丢弃；
  *          AVE_UART_EBADCH 格式化失败
  * 注意事项：① vsnprintf 到定长栈缓冲（无溢出），本函数不重试、不等待，忙就丢并计数。
  *           ② 只能在任务上下文调用，禁止中断里用（vsnprintf 非可重入）。
  *           ③ 整型显式转 unsigned long 再配 %lu，杜绝 -Wformat 告警掩蔽真问题。
  *           ④ 不发 `%f`：浮点部分会把 printf 链进 flash 占几 KB，且慢一个数量级。
  *           ⑤ 返回负数 = 格式化出错，必须看，否则截断的行会骗人（数字被切半）。
  */
int AVE_Log(const char *fmt, ...)
{
  char    line[AVE_LOG_LINE_MAX];  //栈上定长缓冲区
  va_list ap;   //变参列表：va_start/va_end 之间用它取 fmt 后面实参
  int     cap;  //缓冲区总容量，等于 sizeof(line)
  int     n;    //vsnprintf 返回的本该写入的字符数
  int     cut;  //1 = 内容被截断

  // 1.格式串为空：直接返回出错
  if (fmt == 0)
  {
    return AVE_UART_EBADCH;
  }

  cap = (int)sizeof(line);

  va_start(ap, fmt);
  n = vsnprintf(line, (size_t)cap, fmt, ap);
  va_end(ap);

  /* 负数 = 格式化本身出错。此返回值必须看：只看写进去的部分，被截断的行
     会看起来完全正常（数字被切半），据此得出的结论全是错的 */
  // 2.代表格式化出错
  if (n < 0)
  {
    return AVE_UART_EBADCH;
  }

  // 3.日志没被截断
  cut = 0;
  if (n > (cap - 2))
  {
    //内容已装不下（cap-2 是给 "\r\n" 留位）：钳到能放下的长度，并标记
    n   = cap - 2;
    cut = 1;
  }

  // 4.日志被截断
  if (cut != 0)
  {
    /* 再腾 1 字节放截断标记（cap>=8 保证不 下溢） */
    n--;
    line[n] = '~';      /* 行尾带 '~' = 这条被截断了，内容不完整 */
    n++;
  }

  /* 行尾：串口助手默认按 \r\n 断行，两个都给 */
  line[n] = '\r';
  n++;
  line[n] = '\n';
  n++;

  /* 故意不补 '\0'：发送按长度走，不需要结尾符，补了还白挤一个字符位。
     发不发得出去、丢了怎么计数全在 AVE_Uart_WriteIT() 里，本函数不重试不等待 */
  return AVE_Uart_WriteIT(AVE_UART_DBG, (const uint8_t *)line, n);
}
