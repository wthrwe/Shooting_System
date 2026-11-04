/**
 * 仿真 HAL 实现 —— 行为对齐 STM32F4 HAL，见 stm32f4xx_hal.h 顶部的保真度说明。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "fake_hal.h"
#include "usart.h"
#include "host_trace.h"
#include <string.h>

/* ============================================================================
 * 一、句柄实例（真实工程里在 CubeMX 生成的 usart.c）
 * ==========================================================================*/

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;

/*
 * 假 `RCC->CSR`。**定义**在这里、声明在 stm32f4xx_hal.h ——
 * 因为读它的是产品代码 `AVE_board.c`，它只看得见那个头文件。
 * 初始 0 = 一个复位标志都没亮，于是 `AVE_Board_ResetCause()` 在本机恒返回 0。
 * 想构造某个复位原因，用 host_rcc_set_csr()。
 */
uint32_t host_rcc_csr = 0U;

void host_rcc_set_csr(uint32_t bits)
{
  host_rcc_csr = bits;
}

/* ============================================================================
 * 二、每路的仿真状态
 * ==========================================================================*/

#define HOST_CH_NUM   3
#define HOST_TX_CAP   4096
#define HOST_GPIO_CAP 8

typedef struct
{
  UART_HandleTypeDef *h;
  uint8_t            *rx_dst;    /**< 已武装的落脚点；NULL 表示没武装 */
  int                 rx_size;   /**< 武装时申请的字节数（本工程恒为 1） */
  uint8_t             tx[HOST_TX_CAP];
  int                 tx_len;
  const uint8_t      *tx_src;    /**< 中断发送的源指针；NULL 表示没有在途的发送 */
  int                 tx_size;   /**< 在途发送的字节数 */
} host_ch_t;

static host_ch_t s_ch[HOST_CH_NUM];

typedef struct
{
  GPIO_TypeDef *port;
  uint16_t      pin;
  int           level;
  unsigned long writes;
} host_gpio_t;

static host_gpio_t s_gpio[HOST_GPIO_CAP];
static int         s_gpio_n;

unsigned long host_dead_rx_count;
unsigned long host_delivered_count;
unsigned long host_rearm_busy_count;
unsigned long host_tx_busy_count;
unsigned long host_tx_finish_count;

/**
 * 非 0 = **关掉"时间流逝会自动完成在途发送"这条保真**，用来构造
 * "上一帧迟迟发不完"的工况（只有那时 `AVE_gimbal.c` 的重试分支才会被走到）。
 * 默认 0，即打开自动完成。
 */
static int s_tx_hold;

static int slot_of(const UART_HandleTypeDef *huart)
{
  int i;
  for (i = 0; i < HOST_CH_NUM; i++)
  {
    if (s_ch[i].h == huart)
    {
      return i;
    }
  }
  return -1;
}

static void host_init_once(void)
{
  static int done = 0;
  if (done)
  {
    return;
  }
  done = 1;

  memset(s_ch, 0, sizeof(s_ch));

  s_ch[0].h = &huart1;
  s_ch[1].h = &huart2;
  s_ch[2].h = &huart3;

  huart1.Instance  = USART1;
  huart2.Instance  = USART2;
  huart3.Instance  = USART3;

  huart1.ErrorCode = HAL_UART_ERROR_NONE;
  huart2.ErrorCode = HAL_UART_ERROR_NONE;
  huart3.ErrorCode = HAL_UART_ERROR_NONE;

  huart1.RxState = HAL_UART_STATE_READY;
  huart2.RxState = HAL_UART_STATE_READY;
  huart3.RxState = HAL_UART_STATE_READY;

  huart1.gState = HAL_UART_STATE_READY;
  huart2.gState = HAL_UART_STATE_READY;
  huart3.gState = HAL_UART_STATE_READY;
}

/* ============================================================================
 * 三、HAL 接口
 * ==========================================================================*/

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *pData, uint16_t Size)
{
  int i;

  host_init_once();

  /*
   * 🔴 真实 HAL 在这里的行为：`RxState` 处于 BUSY_RX 时返回 HAL_BUSY，**不武装**。
   *    这就是 AVE_uart.c 的 ErrorCallback 里为什么必须先 Abort 再武装 ——
   *    少了 Abort，这一句永远返回 BUSY，"我明明重新武装了"却依然收不到数据。
   */
  if (huart->RxState == HAL_UART_STATE_BUSY_RX)
  {
    host_rearm_busy_count++;
    return HAL_BUSY;
  }

  i = slot_of(huart);
  if (i < 0)
  {
    return HAL_ERROR;   /* 不属于本工程的串口 */
  }

  s_ch[i].rx_dst  = pData;
  s_ch[i].rx_size = (int)Size;
  huart->RxState  = HAL_UART_STATE_BUSY_RX;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *huart)
{
  int i;

  host_init_once();

  i = slot_of(huart);
  if (i >= 0)
  {
    s_ch[i].rx_dst = NULL;
  }
  huart->RxState = HAL_UART_STATE_READY;   /* 真实 HAL 在这里把状态清回 READY */
  return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart, const uint8_t *pData,
                                    uint16_t Size, uint32_t Timeout)
{
  int i;

  host_init_once();
  (void)Timeout;

  /* 与真实 HAL 一致：这一路在忙就返回 HAL_BUSY，一个字节都不发 */
  if (huart->gState != HAL_UART_STATE_READY)
  {
    host_tx_busy_count++;
    return HAL_BUSY;
  }

  i = slot_of(huart);
  if (i < 0)
  {
    return HAL_ERROR;
  }
  if ((int)Size > (HOST_TX_CAP - s_ch[i].tx_len))
  {
    return HAL_ERROR;   /* 抓包缓冲满，视为发送失败 */
  }

  huart->gState = HAL_UART_STATE_BUSY_TX;
  memcpy(&s_ch[i].tx[s_ch[i].tx_len], pData, (size_t)Size);
  s_ch[i].tx_len += (int)Size;
  huart->gState = HAL_UART_STATE_READY;   /* 阻塞发送返回时已经发完了 */
  return HAL_OK;
}

/**
 * 模拟"这一笔中断发送真的发完了" —— 等价于硬件发完最后一个字节后进中断。
 *
 * 🔴 **拷贝放在这里，而不是 `Transmit_IT()` 里** —— 这是本替身最要紧的一条保真度。
 *    真实的 `Transmit_IT()` 只记指针，内容是在之后由中断一个个搬走的。
 *    若替身提前抄走，产品代码"忘了自己拷一份缓冲"就永远测不出来：
 *    那个 bug 在真机上表现为**发出去的内容是调用方栈上的残留**，
 *    而在这里表现为"抓到的字节数和启动时给的一模一样"，判据全绿。
 *
 * 返回 0 表示确实有一笔在途发送被完成；-1 表示这一路本来就没在发。
 */
int host_uart_tx_finish(UART_HandleTypeDef *huart)
{
  int i;

  host_init_once();

  i = slot_of(huart);
  if (i < 0)
  {
    return -1;
  }

  if (huart->gState != HAL_UART_STATE_BUSY_TX)
  {
    return -1;
  }

  if ((s_ch[i].tx_src != NULL) &&
      (s_ch[i].tx_size > 0) &&
      (s_ch[i].tx_size <= (HOST_TX_CAP - s_ch[i].tx_len)))
  {
    memcpy(&s_ch[i].tx[s_ch[i].tx_len], s_ch[i].tx_src, (size_t)s_ch[i].tx_size);
    s_ch[i].tx_len += s_ch[i].tx_size;
  }

  s_ch[i].tx_src  = NULL;
  s_ch[i].tx_size = 0;

  /*
   * 🔴 顺序照抄真实 HAL 的 `UART_EndTransmit_IT()`：
   *    `gState = READY` → 关中断 → `HAL_UART_TxCpltCallback()`。
   *    **回调执行期间 gState 已经是 READY** —— 与接收方向完全对称。
   *    这样就允许"在 TxCplt 回调里立刻启动下一笔发送"这种用法，
   *    而 AVE_uart.c 的忙标志正好也是在同一时刻清的，两边语义一致。
   */
  huart->gState = HAL_UART_STATE_READY;
  HAL_UART_TxCpltCallback(huart);
  return 0;
}

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart, const uint8_t *pData,
                                       uint16_t Size)
{
  int i;

  host_init_once();

  /*
   * 🔴 真实 HAL 的判据是 `gState != HAL_UART_STATE_READY` → 返回 HAL_BUSY，**一个字节都不发**。
   *    这正是 AVE_uart.c 里"启动失败必须把忙标志清回去"和错误回调里
   *    "顺手 Abort 发送方向"两段代码针对的场景：
   *    `gState` 卡在 BUSY_TX 之后，所有日志都会静默消失。
   */
  if (huart->gState != HAL_UART_STATE_READY)
  {
    host_tx_busy_count++;
    return HAL_BUSY;
  }

  i = slot_of(huart);
  if (i < 0)
  {
    return HAL_ERROR;
  }
  if ((pData == NULL) || (Size == 0U))
  {
    return HAL_ERROR;
  }

  host_trace("txit");           /* 轨迹里必须看得见"这一刻启动了一笔发送" */

  s_ch[i].tx_src  = pData;      /* 只记指针 —— 内容等"发完"那一刻再抄 */
  s_ch[i].tx_size = (int)Size;
  huart->gState   = HAL_UART_STATE_BUSY_TX;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef *huart)
{
  int i;

  host_init_once();

  i = slot_of(huart);
  if (i >= 0)
  {
    s_ch[i].tx_src  = NULL;
    s_ch[i].tx_size = 0;
  }
  huart->gState = HAL_UART_STATE_READY;
  return HAL_OK;
}

/**
 * 时间流逝 —— 把仍在途的中断发送当作已经发完。
 *
 * 🔴 **这不是"方便测试"，它是一条必须有的保真度。**
 *    真实的 `HAL_UART_Transmit_IT()` 是"启动一次、发完进中断通知"。
 *    115200 下 12 字节只要约 1.1 毫秒，所以只要代码在两次发送之间**等了 10 毫秒**，
 *    上一笔**必然已经发完**了。
 *
 *    替身若不做这件事，在途标志会一直挂着 ⇒ 第二帧永远撞 `HAL_BUSY` ⇒
 *    于是被测代码的重试/丢弃分支在测试里被**高频触发**，
 *    而真机上那条分支几乎走不到。测试就会开始汇报一个真机不存在的问题，
 *    同时掩盖真机上真正会发生的事。
 *
 *    它由**延时函数**（`HAL_Delay` / 仿真 `osDelay`）调用 ——
 *    因为"时间往前走"正是延时的定义，也是真机上这笔画完的唯一原因。
 */
void host_uart_tx_progress(uint32_t ms)
{
  int i;

  if ((ms == 0U) || (s_tx_hold != 0))
  {
    return;
  }

  host_init_once();

  for (i = 0; i < HOST_CH_NUM; i++)
  {
    if ((s_ch[i].h != NULL) && (s_ch[i].h->gState == HAL_UART_STATE_BUSY_TX))
    {
      if (host_uart_tx_finish(s_ch[i].h) == 0)
      {
        host_tx_finish_count++;
        host_trace("txfin");
      }
    }
  }
}

void host_uart_tx_hold(int on)
{
  s_tx_hold = (on != 0) ? 1 : 0;
}

/**
 * 阻塞延时。真实实现是"忙等 `HAL_GetTick()`"，本替身只需两件事：
 * 记一笔轨迹（供断言"延时到底发生没发生、多少毫秒、在锁内还是锁外"），
 * 以及让时间往前走（`host_uart_tx_progress`）。
 */
void HAL_Delay(uint32_t Delay)
{
  host_trace_ms("hdelay", Delay);
  host_uart_tx_progress(Delay);
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
  int i;

  for (i = 0; i < s_gpio_n; i++)
  {
    if ((s_gpio[i].port == GPIOx) && (s_gpio[i].pin == GPIO_Pin))
    {
      s_gpio[i].level = (int)PinState;
      s_gpio[i].writes++;
      return;
    }
  }
  if (s_gpio_n < HOST_GPIO_CAP)
  {
    s_gpio[s_gpio_n].port   = GPIOx;
    s_gpio[s_gpio_n].pin    = GPIO_Pin;
    s_gpio[s_gpio_n].level  = (int)PinState;
    s_gpio[s_gpio_n].writes = 1UL;
    s_gpio_n++;
  }
}

/* ============================================================================
 * 四、注入
 * ==========================================================================*/

int host_uart_inject(UART_HandleTypeDef *huart, uint8_t b)
{
  int i;

  host_init_once();

  i = slot_of(huart);
  if (i < 0)
  {
    return -1;
  }

  /*
   * 🔴 探针：没人武装就收不到 —— 真实硬件上这一刻是**完全静默**的。
   *    把它变成计数，才能让"漏掉重新武装"这条判据可失败。
   */
  if (s_ch[i].rx_dst == NULL)
  {
    host_dead_rx_count++;
    return -1;
  }

  *(s_ch[i].rx_dst) = b;
  s_ch[i].rx_dst    = NULL;                 /* 收一个字节就停，与 HAL 一致 */

  /*
   * 🔴 顺序不能反：真实 F4 HAL 在 `UART_Receive_IT()` 里是
   *    `RxState = READY` → `__HAL_UNLOCK` → `HAL_UART_RxCpltCallback()`。
   *    所以**回调执行期间 RxState 是 READY**，
   *    回调里那句 `HAL_UART_Receive_IT()` 才不会撞上 HAL_BUSY。
   *    见 stm32f4xx_hal.h 顶部的保真度说明。
   */
  huart->RxState = HAL_UART_STATE_READY;
  host_delivered_count++;
  HAL_UART_RxCpltCallback(huart);

  return 0;
}

int host_uart_inject_str(UART_HandleTypeDef *huart, const char *s, int n)
{
  int i;
  for (i = 0; i < n; i++)
  {
    if (host_uart_inject(huart, (uint8_t)s[i]) != 0)
    {
      return i;   /* 返回成功注入的字节数 —— 中途"死亡"时调用方能看到卡在第几个 */
    }
  }
  return n;
}

void host_raise_error(UART_HandleTypeDef *huart, uint32_t hal_error_code, int leave_busy)
{
  host_init_once();

  huart->ErrorCode = hal_error_code;
  if (leave_busy)
  {
    huart->RxState = HAL_UART_STATE_BUSY_RX;   /* 最坏情形：状态卡在 BUSY_RX */
  }
  HAL_UART_ErrorCallback(huart);
}

/* ============================================================================
 * 五、发送抓包 / GPIO 查询 / 复位
 * ==========================================================================*/

void host_tx_reset(UART_HandleTypeDef *huart)
{
  int i = slot_of(huart);
  if (i >= 0)
  {
    s_ch[i].tx_len = 0;
  }
}

const uint8_t *host_tx_data(UART_HandleTypeDef *huart, int *len)
{
  int i = slot_of(huart);
  if (i < 0)
  {
    if (len)
    {
      *len = 0;
    }
    return NULL;
  }
  if (len)
  {
    *len = s_ch[i].tx_len;
  }
  return s_ch[i].tx;
}

int host_gpio_last(GPIO_TypeDef *port, uint16_t pin)
{
  int i;
  for (i = 0; i < s_gpio_n; i++)
  {
    if ((s_gpio[i].port == port) && (s_gpio[i].pin == pin))
    {
      return s_gpio[i].level;
    }
  }
  return -1;
}

unsigned long host_gpio_writes(GPIO_TypeDef *port, uint16_t pin)
{
  int i;
  for (i = 0; i < s_gpio_n; i++)
  {
    if ((s_gpio[i].port == port) && (s_gpio[i].pin == pin))
    {
      return s_gpio[i].writes;
    }
  }
  return 0UL;
}

void host_reset_all(void)
{
  int i;

  host_init_once();

  for (i = 0; i < HOST_CH_NUM; i++)
  {
    s_ch[i].rx_dst  = NULL;
    s_ch[i].rx_size = 0;
    s_ch[i].tx_len  = 0;
    s_ch[i].tx_src  = NULL;
    s_ch[i].tx_size = 0;
  }
  s_gpio_n = 0;

  host_dead_rx_count    = 0UL;
  host_delivered_count  = 0UL;
  host_rearm_busy_count = 0UL;
  host_tx_busy_count    = 0UL;
  host_tx_finish_count  = 0UL;
  s_tx_hold             = 0;

  /* 复位标志回到"什么也没亮"。
   *
   * 🔴 这一行不是可有可无的洁癖：产品代码 `AVE_Board_ResetCause()` 把结果
   *    **缓存在一个 static 里**（哨兵 0xFFFFFFFF），一个进程内只算一次。
   *    于是"注入 CSR→调它→断言"这条用例**一个进程内只能跑一次**；
   *    第二个注入无论设什么，读到的都是第一次的缓存值。
   *    将来真要给这段逻辑写用例，得换个进程（或先给它加一个可测试的入口），
   *    光靠这里清零是不够的。此处只保证"忘了设 CSR 的用例从 0 开始"。 */
  host_rcc_csr = 0U;

  huart1.RxState = huart2.RxState = huart3.RxState = HAL_UART_STATE_READY;
  huart1.gState  = huart2.gState  = huart3.gState  = HAL_UART_STATE_READY;
  huart1.ErrorCode = huart2.ErrorCode = huart3.ErrorCode = HAL_UART_ERROR_NONE;
}
