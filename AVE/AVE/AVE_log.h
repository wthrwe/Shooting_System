/* AVE_log.h —— 调试日志：格式化一条、非阻塞发出去、发不出去就丢 */
/* 语义：日志给控制让路，宁丢日志不误控制，不可当审计记录 */
#ifndef AVE_LOG_H
#define AVE_LOG_H

#include "AVE_config.h"
#include "AVE_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * 函    数：格式化一条日志并按 \r\n 结尾非阻塞发到调试口
  * 参    数：fmt 格式串，用法同 printf
  * 返 回 值：>0 已受理字节数；AVE_UART_EBUSY 上一笔未发完、本条丢弃；
  *          AVE_UART_EBADCH 格式化失败
  * 注意事项：① 只能在任务上下文调用，禁止中断里用（vsnprintf 非可重入）。
  *           ② 整型显式转 unsigned long 再配 %lu，直接配会 -Wformat 告警。
  *           ③ 不要用 `%f`：会把 printf 浮点部分链进 flash 占几 KB，且更慢。
  *           ④ 返回值可忽略；丢弃数靠心跳报出。
  */
int AVE_Log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* AVE_LOG_H */
