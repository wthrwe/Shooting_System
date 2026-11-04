#include "AVE_uart.h"
#include "AVE_ringbuf.h"

#if (AVE_SELFTEST_PIN_PROBE != 0)
#include <stdio.h>       /* snprintf —— 只有探针任务用得到 */
#include "cmsis_os2.h"   /* osThreadNew / osDelay */
#include "AVE_log.h"     /* AVE_Log —— 探针行走和 [HB] 相同的日志出口 */
#endif
/* ============ 一、私有数据 ============ */

static UART_HandleTypeDef *const s_huart[] =
{
  AVE_HUART_CAM,      //ch = 0,摄像头串口
  AVE_HUART_ESP       //ch = 1,ESP串口
};

static AVE_RingBuf s_rb[AVE_UART_CH_NUM];       //环形缓冲区结构体
static uint8_t s_rb_storage[AVE_UART_CH_NUM][AVE_UART_RX_BUF_SIZE]; //环形缓冲区数组
static uint8_t s_rx_byte[AVE_UART_CH_NUM];      //中断接收单字节缓存
static AVE_UartStats s_stats[AVE_UART_CH_NUM];  //串口统计结构体
static uint8_t s_tx_buf[AVE_UART_CH_NUM][AVE_UART_TX_BUF_SIZE]; //发送缓冲区数组
static volatile uint8_t s_tx_busy[AVE_UART_CH_NUM]; //发送忙标志位

/**
  * 二、私有函数
  *
  * 函    数：由 HAL 句柄反查通道号
  * 参    数：huart HAL 句柄
  * 返 回 值：通道号，不属于本工程返回 -1
  * 注意事项：在中断里调用，必须快，只有几次指针比较。
  */
static int chan_of(const UART_HandleTypeDef *huart)
{
  int i;

  for (i = 0; i < (int)AVE_UART_CH_NUM; i++)
  {
    if (s_huart[i] == huart)
    {
      return i;
    }
  }
  return -1;
}

/**
  * 函    数：通道号合法性检查
  * 参    数：ch 通道号
  * 返 回 值：1 合法，0 越界
  * 注意事项：防止用非法通道号越界访问数组。
  */
static int chan_valid(AVE_UartChannel ch)
{
  return ((int)ch >= 0) && ((int)ch < (int)AVE_UART_CH_NUM);
}

/**
  * 函    数：重新武装一路接收
  * 参    数：ch 通道号
  * 返 回 值：0 成功，-1 失败（会计入 err）
  */
static int rearm(int ch)
{
  if (HAL_UART_Receive_IT(s_huart[ch], &s_rx_byte[ch], 1U) != HAL_OK)
  {
    s_stats[ch].err++;
    return -1;
  }
  return 0;
}


/* ===================== B/D 口引脚探针（默认不编译） ===================== */
#if (AVE_SELFTEST_PIN_PROBE != 0)
typedef struct
{
  GPIO_TypeDef *port;   /* 端口 */
  uint16_t      pin;    /* 脚号掩码，如 GPIO_PIN_7 */
  const char   *name;   /* 日志里显示的名字，和板上丝印一致 */
} AVE_PinProbe;

/* 为什么是这 13 个脚：丝印上那一组（B6 B4 D7 D6 D4 D2 D0 / B8 B7 B5 B3 D5 D3）
   全部落在 GPIOB / GPIOD 里，抄下来就能逐个对。已业务占用的脚一个都不能列：
   PA3/PA4/PA5（水平 STEP、俯仰 DIR、水平 DIR）、PB10（俯仰 STEP）、PB11（调试口 RX）、
   PD8（调试口 TX）、PC13（激光）—— 把它们的复用切掉会直接打断电机或日志。 */
static const AVE_PinProbe s_probe_pins[] =
{
  { GPIOB, GPIO_PIN_6, "B6" },
  { GPIOB, GPIO_PIN_7, "B7" },
  { GPIOB, GPIO_PIN_5, "B5" },
  { GPIOB, GPIO_PIN_4, "B4" },
  { GPIOB, GPIO_PIN_3, "B3" },
  { GPIOB, GPIO_PIN_8, "B8" },
  { GPIOD, GPIO_PIN_7, "D7" },
  { GPIOD, GPIO_PIN_6, "D6" },
  { GPIOD, GPIO_PIN_5, "D5" },
  { GPIOD, GPIO_PIN_4, "D4" },
  { GPIOD, GPIO_PIN_3, "D3" },
  { GPIOD, GPIO_PIN_2, "D2" },
  { GPIOD, GPIO_PIN_0, "D0" }
};

#define AVE_PIN_PROBE_NUM   ((int)(sizeof(s_probe_pins) / sizeof(s_probe_pins[0])))

static osThreadId_t s_pin_probe_tid;   /* 0 = 还没建；非 0 = 已建（防重复） */

static const osThreadAttr_t s_pin_probe_attr =
{
  .name       = "pinprobe",
  .stack_size = 512U * 4U,             /* = 2048 字节：AVE_Log 内部要跑 vsnprintf，栈不能抠 */
  .priority   = (osPriority_t)osPriorityLow,   /* 最低优先级：它只是“偷空采样” */
};

/**
  * 函    数：读一路被探针监听的脚（0 低 / 1 高）
  * 参    数：i 表内下标
  * 返 回 值：0 或 1
  * 注意事项：直接读 IDR，不走 HAL —— 采样要快，函数调用本身就会把采样率打下来。
  */
static uint8_t pin_level(int i)
{
  return ((s_probe_pins[i].port->IDR & s_probe_pins[i].pin) != 0U) ? 1U : 0U;
}

/**
  * 函    数：引脚探针任务
  * 参    数：argument 未使用（osThreadNew 要求这个原型）
  * 返 回 值：永不返回
  * 注意事项：① 先把 13 个脚全部从复用功能切回输入上拉，否则读 IDR 读到的是
  *              复用功能下的电平，不是“这根线本身”；
  *           ② 采样窗口是忙等，13 个脚 × 20 万次 ≈ 100 ms，期间不让出 CPU ——
  *              只有采样率远高于 115200 的位宽（约 8.7 µs）才数得到跳变；
  *           ③ `low=` 是**短接判据**（接地 → 变 0），`edg=` 是**信号判据**
  *              （相机在发 → 非 0）。两件事分开看，别混。
  * 谁调用：只在 AVE_Uart_Init() 里用 osThreadNew 建一次，之后由调度器运行。
  */
static void pin_probe_task(void *argument)
{
  GPIO_InitTypeDef g;
  uint32_t         edges[AVE_PIN_PROBE_NUM];
  uint8_t          last[AVE_PIN_PROBE_NUM];
  char             low[128];
  char             edg[128];
  int              i;
  int              k;
  int              ln;
  int              le;

  (void)argument;

  /* 必须在 AVE_Uart_Init() 的 MspInit/rearm 之后切：顺序反了会被改回 AF。 */
  for (i = 0; i < AVE_PIN_PROBE_NUM; i++)
  {
    g.Pin   = s_probe_pins[i].pin;
    g.Mode  = GPIO_MODE_INPUT;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(s_probe_pins[i].port, &g);
  }

  for (;;)
  {
    for (i = 0; i < AVE_PIN_PROBE_NUM; i++)
    {
      last[i]  = pin_level(i);
      edges[i] = 0U;
    }

    for (k = 0; k < 200000; k++)
    {
      for (i = 0; i < AVE_PIN_PROBE_NUM; i++)
      {
        uint8_t now = pin_level(i);

        if (now != last[i])
        {
          edges[i]++;
          last[i] = now;
        }
      }
    }

    low[0] = '\0';
    edg[0] = '\0';
    ln = 0;
    le = 0;

    for (i = 0; i < AVE_PIN_PROBE_NUM; i++)
    {
      int w;

      if (pin_level(i) == 0U)
      {
        w = snprintf(&low[ln], sizeof(low) - (size_t)ln, "%s%s",
                     (ln == 0) ? "" : " ", s_probe_pins[i].name);
        if ((w > 0) && ((ln + w) < (int)sizeof(low)))
        {
          ln += w;
        }
      }

      if (edges[i] > 0U)
      {
        w = snprintf(&edg[le], sizeof(edg) - (size_t)le, "%s%s(%lu)",
                     (le == 0) ? "" : " ", s_probe_pins[i].name,
                     (unsigned long)edges[i]);
        if ((w > 0) && ((le + w) < (int)sizeof(edg)))
        {
          le += w;
        }
      }
    }

    /* 注意：AVE_Log 自己会在行尾补 "\r\n"，这里不能重复写。 */
    (void)AVE_Log("[PIN] t=%lu low=[%s] edg=[%s]",
                  (unsigned long)osKernelGetTickCount(),
                  (ln > 0) ? low : "none",
                  (le > 0) ? edg : "none");

    osDelay(1000U);   /* 1 秒一次；osDelay 让出 CPU，不会饿死别的任务 */
  }
}
#endif /* AVE_SELFTEST_PIN_PROBE */

/**
  * 三、对外接口
  *
  * 函    数：初始化三路串口并启动接收
  * 参    数：无
  * 返 回 值：无
  * 注意事项：必须在 MX_USART*_UART_Init() 之后、首个用串口的任务启动前调用；幂等。
  */
void AVE_Uart_Init(void)
{
  int ch;

  // 遍历所有串口通道（ch=0 CAM，ch=1 ESP）
  for (ch = 0; ch < (int)AVE_UART_CH_NUM; ch++)
  {
    // 1. 初始化【接收环形缓冲区】
    (void)AVE_RbInit(&s_rb[ch], s_rb_storage[ch], AVE_UART_RX_BUF_SIZE);
    
    // 2. 清零这一路串口的所有统计计数
    s_stats[ch].rx_bytes   = 0U;  //收到总字节数
    s_stats[ch].drop_bytes = 0U;  //ringbuf丢弃总字节数
    s_stats[ch].drop_lines = 0U;  //ringbuf丢弃总行数
    s_stats[ch].err        = 0U;  //串口接收错误次数
    s_stats[ch].rx_restart = 0U;  //串口接收重启次数
    s_stats[ch].tx_err     = 0U;  //串口发送错误次数
    s_stats[ch].tx_drop    = 0U;  //串口发送丢弃次数

    //3.发送忙标志位清零
    s_tx_busy[ch] = 0U;   

    //4.单字节接收中断重新武装
    (void)rearm(ch);
  }

#if (AVE_SELFTEST_PIN_PROBE != 0)
  /* 探针任务只建一次：本函数声明为幂等，重复调用不能再建第二个探针。 */
  if (s_pin_probe_tid == 0)
  {
    s_pin_probe_tid = osThreadNew(pin_probe_task, 0, &s_pin_probe_attr);
  }
#endif
}

/**
  * 函    数：按字节读取，不按行
  * 参    数：ch 通道号；out 输出缓冲；cap 字节数
  * 返 回 值：实际读到的字节数，0 表示暂无数据
  */
/**
  * 函    数：按行读取（消费端切帧）
  * 参    数：ch 通道号；out 输出缓冲；cap 缓冲字节数
  * 返 回 值：>=0 行长度（不含换行符）；否则各负值错误码
  * 注意事项：缓冲满且仍无换行时强制丢弃整段并计数，避免读不到行而永久卡死（溢出/粘包）。
  */
int AVE_Uart_ReadLine(AVE_UartChannel ch, char *out, int cap)
{
  AVE_RingBuf *rb;
  uint32_t     used;
  uint32_t     at;
  uint32_t     n;

  // 1.校验通道、指针、buffer 长度合法性
  if (!chan_valid(ch) || (out == 0) || (cap < 2))
  {
    return AVE_UART_EBADCH;
  }
  
  //2.读取环形缓冲区已用字节数，无数据直接返回`ENODATA`
  rb   = &s_rb[ch];
  used = AVE_RbUsed(rb);

  if (used == 0U)
  {
    return AVE_UART_ENODATA;
  }
  
  //3.`AVE_RbFind`查找`\n`换行符
  if (AVE_RbFind(rb, (uint8_t)'\n', &at) == 0)
  {
    if (at >= (uint32_t)cap) //eg: 缓冲区里是 `abc\r\n123`，at = 4
    {
      AVE_RbSkip(rb, at + 1U);              
      s_stats[ch].drop_lines++;
      return AVE_UART_ENOSPC;
    }

    n = at;
    AVE_RbRead(rb, (uint8_t *)out, n);
    AVE_RbSkip(rb, 1U);                     

    if ((n > 0U) && (out[n - 1U] == '\r'))
    {
      n--;
    }
    out[n] = '\0';
    return (int)n;
  }
  
  if (used >= rb->size)
  {
    AVE_RbDropAll(rb);
    s_stats[ch].drop_lines++;
    return AVE_UART_ELINE_TOO_LONG;
  }
  return AVE_UART_ENODATA;
}

int AVE_Uart_Read(AVE_UartChannel ch, uint8_t *out, int cap)
{
  uint32_t used;  //ringbuf 当前缓存里未读取的字节总数
  uint32_t n;     //本次实际读取字节数

  if (!chan_valid(ch) || (out == 0) || (cap <= 0))
  {
    return AVE_UART_EBADCH;
  }

  used = AVE_RbUsed(&s_rb[ch]);
  n    = (used < (uint32_t)cap) ? used : (uint32_t)cap; //不超过输出缓冲区大小，防止溢出
  if (n == 0U)
  {
    return 0;
  }

  AVE_RbRead(&s_rb[ch], out, n);
  return (int)n;
}

/**
  * 函    数：查询缓冲内可读字节数
  * 参    数：ch 通道号
  * 返 回 值：可读字节数
  */
uint32_t AVE_Uart_Available(AVE_UartChannel ch)
{
  if (!chan_valid(ch))
  {
    return 0U;
  }
  return AVE_RbUsed(&s_rb[ch]);
}

/**
  * 函    数：丢弃一路的全部未读数据
  * 参    数：ch 通道号
  * 返 回 值：无
  */
void AVE_Uart_Flush(AVE_UartChannel ch)
{
  if (!chan_valid(ch))
  {
    return;
  }
  AVE_RbDropAll(&s_rb[ch]);
}

/**
  * 函    数：阻塞发送
  * 参    数：ch 通道号；buf 数据指针；len 长度
  * 返 回 值：发送的字节数（==len）；失败返回负值
  * 注意事项：同一路不允许两任务并发调用，须由上层互斥量串行化；忙等不发散到其他任务。
  */
int AVE_Uart_Write(AVE_UartChannel ch, const uint8_t *buf, int len)
{
  if (!chan_valid(ch) || (buf == 0) || (len <= 0))
  {
    return AVE_UART_EBADCH;
  }
  
  if (len > 0xFFFF)
  {
    return AVE_UART_EBADCH;
  }

  if (HAL_UART_Transmit(s_huart[ch], (uint8_t *)buf, (uint16_t)len,
                        (uint32_t)AVE_UART_TX_TIMEOUT_MS) != HAL_OK)
  {
    s_stats[ch].tx_err++;
    return AVE_UART_ETIMEOUT;
  }

  return len;
}

/**
  * 函    数：字符串快捷发送，长度自己数
  * 参    数：ch 通道号；s 字符串
  * 返 回 值：发送的字节数；失败返回负值
  */
int AVE_Uart_WriteStr(AVE_UartChannel ch, const char *s)
{
  int n = 0;

  if (s == 0)
  {
    return AVE_UART_EBADCH;
  }
  while (s[n] != '\0')
  {
    n++;
  }
  if (n == 0)
  {
    return 0;
  }
  return AVE_Uart_Write(ch, (const uint8_t *)s, n);
}


/**
  * 函    数：非阻塞发送（中断发送），立即返回
  * 参    数：ch 通道号；buf 数据指针；len 长度
  * 返 回 值：>0 已受理字节数；EBUSY 上次未发完；ENOSPC 超缓冲；ETIMEOUT 启动失败；EBADCH 非法
  * 注意事项：数据先拷进静态缓冲再启动发送；先占忙标志再拷数据；禁止在中断里调用。
  */
int AVE_Uart_WriteIT(AVE_UartChannel ch, const uint8_t *buf, int len)
{
  // 1.判断通道合法、数据长度不超过本地 tx 静态 buffer、`s_tx_busy`空闲
  if (!chan_valid(ch) || (buf == 0) || (len <= 0))
  {
    return AVE_UART_EBADCH;
  }
  
  if (len > (int)AVE_UART_TX_BUF_SIZE)
  {
    s_stats[ch].tx_err++;
    return AVE_UART_ENOSPC;
  }

  if (s_tx_busy[ch] != 0U)
  {
    s_stats[ch].tx_drop++;
    return AVE_UART_EBUSY;
  }

  // 2.标记`s_tx_busy=1`，把用户传入的数据拷贝到本模块静态`s_tx_buf`
  s_tx_busy[ch] = 1U;

  {
    int i;
    for (i = 0; i < len; i++)
    {
      s_tx_buf[ch][i] = buf[i];
    }
  }

  // 3.启动`HAL_UART_Transmit_IT`中断发送
  if (HAL_UART_Transmit_IT(s_huart[ch], s_tx_buf[ch], (uint16_t)len) != HAL_OK)
  {
    s_tx_busy[ch] = 0U;
    s_stats[ch].tx_err++;
    return AVE_UART_ETIMEOUT;
  }

  return len;
}
/**
  * 函    数：查询发送是否忙
  * 参    数：ch 通道号
  * 返 回 值：1 有未发完数据，0 空闲
  */
int AVE_Uart_TxBusy(AVE_UartChannel ch)
{
  if (!chan_valid(ch))
  {
    return 0;
  }
  return (s_tx_busy[ch] != 0U) ? 1 : 0;
}
/**
  * 函    数：取统计量快照指针
  * 参    数：ch 通道号
  * 返 回 值：内部只读指针，不要写它
  */
const AVE_UartStats *AVE_Uart_Stats(AVE_UartChannel ch)
{
  if (!chan_valid(ch))
  {
    return 0;
  }
  return &s_stats[ch];
}

/**
  * 四、HAL 中断回调（全工程唯一一份）
  *
  * 函    数：收到一个字节的中断回调
  * 参    数：huart HAL 句柄
  * 返 回 值：无
  * 注意事项：必须重新武装，否则接收永久静默死亡；缓冲满时丢新字节不覆盖旧数据。
  */
 //收到 1 字节 → 拿到通道号 → 把字节推入环形缓冲区；缓冲区满则丢字节计数 + 1；->
 //**立刻 rearm，开启下一次单字节接收**。
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  int     ch;
  uint8_t b;

  ch = chan_of(huart);
  if (ch < 0)
  {
    return;                 
  }

  b = s_rx_byte[ch];        //把接收到的一字节数据拿出来
  s_stats[ch].rx_bytes++;   //接收字节数 + 1

  if (AVE_RbPush(&s_rb[ch], b) != 0)  //把接收到的字节压入环形缓冲区ringbuf
  {
    s_stats[ch].drop_bytes++;    //环形缓冲区满，丢弃字节数 + 1
  }

  (void)rearm(ch);      //重新开启一字节中断接收
}

/**
  * 函    数：接收错误回调
  * 参    数：huart HAL 句柄
  * 返 回 值：无
  * 注意事项：必须先 AbortReceive_IT 清回 READY 再武装，否则因状态忙返回 HAL_BUSY；发送方向也要清。
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  int ch;

  ch = chan_of(huart);
  if (ch < 0)
  {
    return;
  }

  s_stats[ch].err++;

  (void)HAL_UART_AbortReceive_IT(huart);
  
  (void)HAL_UART_AbortTransmit_IT(huart);
  s_tx_busy[ch] = 0U;

  huart->ErrorCode = HAL_UART_ERROR_NONE;

  if (rearm(ch) == 0)
  {
    s_stats[ch].rx_restart++;
  }
}

/**
  * 函    数：发送完成中断回调
  * 参    数：huart HAL 句柄
  * 返 回 值：无
  * 注意事项：清忙标志，否则后续发送会被误判为忙。
  */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  int ch;
  ch = chan_of(huart);
  if (ch < 0)
  {
    return;
  }
  s_tx_busy[ch] = 0U;
}

typedef char ave_uart_chk_table_size
  [((sizeof(s_huart) / sizeof(s_huart[0])) == (unsigned)AVE_UART_CH_NUM) ? 1 : -1];
typedef char ave_uart_chk_cam_is_first [((int)AVE_UART_CAM == 0) ? 1 : -1];
typedef char ave_uart_chk_dbg_is_last  [((int)AVE_UART_DBG == ((int)AVE_UART_CH_NUM - 1)) ? 1 : -1];
