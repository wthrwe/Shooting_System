

#ifndef AVE_UART_H
#define AVE_UART_H

#include <stdint.h>
#include "AVE_config.h"

/* ==========================================================================
 * 现场诊断开关：B/D 口引脚探针（默认 0 = 不编译任何额外代码）
 * ==========================================================================
 * 打开后（=1）：
 *   · 把 13 个可疑脚全部从复用功能切回“普通输入 + 内部上拉”：
 *         PB6 PB7 PB5 PB4 PB3 PB8  PD7 PD6 PD5 PD4 PD3 PD2 PD0
 *     （就是丝印 B6 B4 D7 D6 D4 D2 D0 / B8 B7 B5 B3 D5 D3 对得上的那些口）
 *   · 一个最低优先级任务每秒打一行：
 *         [PIN] t=<毫秒> low=[读到 0 的脚] edg=[有跳变的脚(次数)]
 *   · 该行走调试口（USART3），和 [HB]/[VS] 同一个串口。
 *
 * 怎么用（不需要万用表，也不需要相机）：
 *   拿一根线，一头插要验的孔，另一头插板上任意 GND。
 *   → 那个孔对应的脚会从 1 变 0，名字出现在 low=[...] 里；
 *   → 如果 low=[none]，说明这一下短接没落到任何被监听的脚上：
 *      要么孔不对，要么这根地线/接触不可靠（这两种以前分不开，现在能分开）。
 *   要验“相机信号有没有到”：让相机跑起来、TX 接在该孔上，看 edg=[
 *   有没有那个脚的名字 —— 有跳变说明信号真的进了 MCU。
 *
 * 🔴 为什么一次扫 13 个、而不是只盯 PB7：
 *   逐孔试每次都要重新看日志，而且“PB7 一直是 1”有两种完全不同的原因
 *   （孔不是 PB7 / 地线根本没接上）。单脚探针对这两种给的是同一个读数，
 *   一组脚同时扫才能把它们分开。
 *
 * 🔴 打开它 = USART1 收不到任何数据，视觉链路必然断。
 *    测完必须改回 0 并重新烧录，否则会误以为相机坏了。
 * ========================================================================== */
#define AVE_SELFTEST_PIN_PROBE      0

#ifdef __cplusplus
extern "C" {
#endif



/**
  * 一、通道号：串口助手按索引访问，CAM 在前、DBG 在后。
  */
typedef enum
{
  AVE_UART_CAM = 0,     // 视觉口
  AVE_UART_DBG,         // 调试口
  AVE_UART_CH_NUM       // 通道总数，须放最后
} AVE_UartChannel;



/**
  * 二、错误码：按负值区分，正数一律表示行长度/字节数。
  */
#define AVE_UART_ENODATA         (-1)   // 还没有完整一行 / 无数据
#define AVE_UART_ELINE_TOO_LONG  (-2)   // 缓冲满且无换行，整段被强制丢弃
#define AVE_UART_ENOSPC          (-3)   // 放不下，行已丢弃
#define AVE_UART_EBADCH          (-4)   // 参数非法
#define AVE_UART_ETIMEOUT        (-5)   // 发送超时
#define AVE_UART_EBUSY           (-6)   // 上次未发完被拒


/**
  * 三、每路统计量：各字段写入方不同，写前看清谁在写。
  */
typedef struct
{
  uint32_t rx_bytes;     // 累计进入缓冲的字节数
  uint32_t drop_bytes;   // 因缓冲满被丢弃的字节数
  uint32_t drop_lines;   // 因满且无换行被整段丢弃
  uint32_t err;          // HAL 接收错误次数
  uint32_t rx_restart;   // 出错后成功重装次数
  uint32_t tx_err;       // 发送失败/超时次数
  uint32_t tx_drop;      // 上次未发完被拒次数
} AVE_UartStats;





/**
  * 四、对外接口
  *
  * 函    数：初始化三路串口并启动接收
  * 参    数：无
  * 返 回 值：无
  * 注意事项：必须在 MX_USART*_UART_Init() 之后调用；幂等可重复调用。
  */
void AVE_Uart_Init(void);

/**
  * 函    数：按行读取（切帧）
  * 参    数：ch；out；cap
  * 返 回 值：>=0 行长度，否则错误码
  * 注意事项：缓冲满且无换行时强制丢弃整段防死锁；out 已补结束符。
  */
int AVE_Uart_ReadLine(AVE_UartChannel ch, char *out, int cap);

/**
  * 函    数：按字节读取
  * 参    数：ch；out；cap
  * 返 回 值：实际读到的字节数，0 表示无数据
  */
int AVE_Uart_Read(AVE_UartChannel ch, uint8_t *out, int cap);

/**
  * 函    数：查询可读字节数
  * 参    数：ch
  * 返 回 值：可读字节数
  */
uint32_t AVE_Uart_Available(AVE_UartChannel ch);

/**
  * 函    数：丢弃一路全部未读数据
  * 参    数：ch
  * 返 回 值：无
  */
void AVE_Uart_Flush(AVE_UartChannel ch);

/**
  * 函    数：阻塞发送
  * 参    数：ch；buf；len
  * 返 回 值：发送字节数，失败返回负值
  * 注意事项：同路禁止两任务并发，须由上层互斥量串行化。
  */
int AVE_Uart_Write(AVE_UartChannel ch, const uint8_t *buf, int len);

/**
  * 函    数：字符串快捷发送
  * 参    数：ch；s
  * 返 回 值：发送字节数，失败返回负值
  */
int AVE_Uart_WriteStr(AVE_UartChannel ch, const char *s);

/**
  * 函    数：非阻塞发送（立即返回）
  * 参    数：ch；buf；len
  * 返 回 值：>0 受理字节数；EBUSY/ENOSPC/ETIMEOUT/EBADCH
  * 注意事项：先占忙标志再拷数据；禁止在中断里调用。
  */
int AVE_Uart_WriteIT(AVE_UartChannel ch, const uint8_t *buf, int len);

/**
  * 函    数：查询发送是否忙
  * 参    数：ch
  * 返 回 值：1 忙，0 空闲
  */
int AVE_Uart_TxBusy(AVE_UartChannel ch);

/**
  * 函    数：取统计量只读指针
  * 参    数：ch
  * 返 回 值：内部只读指针，不要写它
  */
const AVE_UartStats *AVE_Uart_Stats(AVE_UartChannel ch);

#ifdef __cplusplus
}
#endif

#endif
