/**
 * 宿主机 HAL 替身（shim）—— 让 AVE 的产品代码能在 PC 上原样编译并运行。
 *
 * ── 这里的原则：**只替身 HAL，不改产品代码** ─────────────────────────────────
 *   被测的是工程里那份 `AVE/AVE_uart.c` / `AVE/AVE_board.c` / `AVE/AVE_ringbuf.h`，
 *   一个字节都不复制、不修改。本文件只提供它们**引用到**的 ARM 侧符号的等价物。
 *
 * ── 仿真保真度：容易被忽略但决定测试成败的一条 ───────────────────────────────
 *   真实的 F4 HAL 在 `UART_Receive_IT()` 里是**先**把 `RxState` 置回
 *   `HAL_UART_STATE_READY`、**再**调用 `HAL_UART_RxCpltCallback()` 的。
 *   也就是说：**回调执行期间 RxState 是 READY，不是 BUSY_RX。**
 *
 *   这直接决定了 AVE_uart.c 那个「回调里重新武装」的写法能不能成立 ——
 *   如果仿真成"回调时还是 BUSY_RX"，`HAL_UART_Receive_IT` 会返回 HAL_BUSY，
 *   重新武装全部失败，测试会报一个**现实中不存在**的故障。
 *   所以 fake_hal.c 严格按真实顺序来，见那里的注释。
 *
 *  ── 第二条保真度：发送抓包**发生在"发送完成"时刻，不是启动时刻** ─────────────
 *   `HAL_UART_Transmit_IT()` 只记下指针就返回，真正的搬运在中断里 ——
 *   所以"线上实际出现的内容"取决于**那一刻指针指向的内存**。
 *
 *   如果替身在 `Transmit_IT()` 被调用时就立刻把内容抄进抓包缓冲，
 *   那么"产品代码没有先拷一份自己的缓冲"这个 bug **永远测不出来** ——
 *   而它正是本工程 `AVE_Uart_WriteIT()` 里那段拷贝存在的理由。
 *   所以本替身把拷贝推迟到 `host_uart_tx_finish()`，见 fake_hal.c。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#ifndef HOST_SHIM_STM32F4XX_HAL_H
#define HOST_SHIM_STM32F4XX_HAL_H

#include <stdint.h>

/* ============================================================================
 * 一、类型与常量
 * ==========================================================================*/

typedef enum
{
  HAL_OK      = 0x00U,
  HAL_ERROR   = 0x01U,
  HAL_BUSY    = 0x02U,
  HAL_TIMEOUT = 0x03U
} HAL_StatusTypeDef;

/**
 * 外设"寄存器块"。**本工程只用它的地址做身份标识，从不解引用。**
 * `AVE_uart.c` 的 `chan_of()` 就是拿 `huart->Instance` 做指针比较，
 * 所以底色是什么类型都不影响 —— 保留 `USART_TypeDef *` 的写法是为了
 * 与真实 HAL 完全一致，将来若有人往产品代码里加一句 `huart->Instance->SR`
 * 才会在宿主机上编不过（这是好事：说明它不再是纯逻辑了）。
 */
typedef struct { uint32_t _opaque; } USART_TypeDef;
typedef struct { uint32_t _opaque; } GPIO_TypeDef;

/* 地址照抄 F4 参考手册，纯为可读性；正确性不依赖具体数值，只依赖"各不相同" */
#define USART1   ((USART_TypeDef *)0x40011000UL)
#define USART2   ((USART_TypeDef *)0x40004400UL)
#define USART3   ((USART_TypeDef *)0x40004800UL)

#define GPIOA    ((GPIO_TypeDef  *)0x40020000UL)
#define GPIOB    ((GPIO_TypeDef  *)0x40020400UL)
#define GPIOC    ((GPIO_TypeDef  *)0x40020800UL)

typedef enum
{
  GPIO_PIN_RESET = 0,
  GPIO_PIN_SET   = 1
} GPIO_PinState;

/**
 * 真实 HAL 里 `FlagStatus` 与 `GPIO_PinState` 是**两个不同的枚举**，
 * 值恰好都是 0/1。这里照抄 `stm32f4xx_hal_def.h` 的写法而不是复用
 * `GPIO_PinState` —— 复用能过编译，但会掩盖一处语义错位：
 * `AVE_board.c` 里 `__HAL_RCC_GET_FLAG(...) != RESET` 的 `RESET`
 * 是**标志位状态**，不是引脚电平。两件事将来若各自演化，复用过的代码会先烂。
 */
typedef enum
{
  RESET = 0,
  SET   = !RESET
} FlagStatus, ITStatus;

#define GPIO_PIN_0   ((uint16_t)0x0001)
#define GPIO_PIN_1   ((uint16_t)0x0002)
#define GPIO_PIN_2   ((uint16_t)0x0004)
#define GPIO_PIN_3   ((uint16_t)0x0008)
#define GPIO_PIN_4   ((uint16_t)0x0010)
#define GPIO_PIN_5   ((uint16_t)0x0020)
#define GPIO_PIN_6   ((uint16_t)0x0040)
#define GPIO_PIN_7   ((uint16_t)0x0080)
#define GPIO_PIN_8   ((uint16_t)0x0100)
#define GPIO_PIN_9   ((uint16_t)0x0200)
#define GPIO_PIN_10  ((uint16_t)0x0400)
#define GPIO_PIN_11  ((uint16_t)0x0800)
#define GPIO_PIN_12  ((uint16_t)0x1000)
#define GPIO_PIN_13  ((uint16_t)0x2000)
#define GPIO_PIN_14  ((uint16_t)0x4000)
#define GPIO_PIN_15  ((uint16_t)0x8000)

#define GPIO_AF7_USART1  ((uint8_t)0x07)
#define GPIO_AF7_USART2  ((uint8_t)0x07)
#define GPIO_AF7_USART3  ((uint8_t)0x07)

/* `RxState` 的取值。真实 HAL 里是宏，这里用宏保持写法一致。 */
#define HAL_UART_STATE_RESET            0x00000000U
#define HAL_UART_STATE_READY            0x00000020U
#define HAL_UART_STATE_BUSY             0x00000024U
#define HAL_UART_STATE_BUSY_TX          0x00000021U
#define HAL_UART_STATE_BUSY_RX          0x00000022U

#define HAL_UART_ERROR_NONE             0x00000000U
#define HAL_UART_ERROR_PE               0x00000001U
#define HAL_UART_ERROR_NE               0x00000002U
#define HAL_UART_ERROR_FE               0x00000004U
#define HAL_UART_ERROR_ORE              0x00000008U

/**
 * 串口句柄。只保留本工程真正用到的字段。
 * 真实结构体大得多 —— 但**没用到就是没用到**，补齐反而给"偷看别的字段"
 * 留了后门，那种代码在宿主机上跑得过、在板子上行为不同。
 */
typedef struct
{
  USART_TypeDef *Instance;
  uint32_t       ErrorCode;
  uint32_t       RxState;
  uint32_t       gState;
} UART_HandleTypeDef;

/* ============================================================================
 * 一·五、假 RCC_CSR —— 只为复位原因（[RST]）那条链路存在
 * ==========================================================================*/

/*
 * 产品代码里只有 `AVE_Board_ResetCause()` 用这几个符号，用法**只有一种**：
 *
 *     if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST) != RESET) { v |= AVE_RST_POR; }
 *     ... 七个标志各一句，然后 ...
 *     __HAL_RCC_CLEAR_RESET_FLAGS();
 *
 * 注意它对 `RCC_FLAG_*` 的态度：**当成不透明标签**，只拿去比较，
 * 从不解构、不问它是哪一位、也不问它在哪个寄存器。
 *
 * 🔴 所以本替身**不照抄 ST 的打包格式**。真 HAL 把这些标签编成
 *    `(寄存器选择 << 5) | 位号` 的 8 位码，我们再抄一遍只会多一处
 *    可能与真值不符的抄写 —— 而它一旦抄错，测试仍然全绿（测的是
 *    "产品代码与我的抄写一致"，不是与真值一致），正是 run_m15.sh 头部
 *    记过的那种假绿。**替身只需保证两条**：
 *      ① 七个标签两两不同；② 每个标签对应假 CSR 里唯一一位。
 *    这两条正好是产品代码真正依赖的性质，多一条都不需要。
 *
 * 位序按 RM0090 的 RCC_CSR（BORRSTF=25 … LPWRRSTF=31，RMVF=24），
 * 取它与真芯片一致**只是为了让读代码的人不必换算**，正确性不依赖它。
 */
#define RCC_FLAG_BORRST   ((uint32_t)1U << 25)   /**< 掉电复位（Brown-out） */
#define RCC_FLAG_PINRST   ((uint32_t)1U << 26)   /**< NRST 引脚复位 */
#define RCC_FLAG_PORRST   ((uint32_t)1U << 27)   /**< 上电复位 */
#define RCC_FLAG_SFTRST   ((uint32_t)1U << 28)   /**< 软件复位 */
#define RCC_FLAG_IWDGRST  ((uint32_t)1U << 29)   /**< 独立看门狗 */
#define RCC_FLAG_WWDGRST  ((uint32_t)1U << 30)   /**< 窗口看门狗 */
#define RCC_FLAG_LPWRRST  ((uint32_t)1U << 31)   /**< 低功耗复位 */

/* 七个复位标志的总掩码 —— `CLEAR_RESET_FLAGS` 清的就是它们 */
#define RCC_CSR_RESET_FLAGS_MASK                                            \
  (RCC_FLAG_BORRST | RCC_FLAG_PINRST | RCC_FLAG_PORRST | RCC_FLAG_SFTRST | \
   RCC_FLAG_IWDGRST | RCC_FLAG_WWDGRST | RCC_FLAG_LPWRRST)

/** 假 `RCC->CSR`。定义在 fake_hal.c；测试用 host_rcc_set_csr() 注入。 */
extern uint32_t host_rcc_csr;

#define __HAL_RCC_GET_FLAG(__FLAG__) \
  (((host_rcc_csr & (uint32_t)(__FLAG__)) != 0UL) ? SET : RESET)

/*
 * 🔴 这条宏承载的是**一个真实的硬件约束**，不是随手写的清位：
 *    F4 的 `RCC_CSR` 只有 `RMVF` 一个"全清"开关，**没有**逐位清零。
 *    所以产品代码必须"先读完七个标志，再清" —— 顺序反了，
 *    读到的就全是 0，`[RST] cause` 永远打成 0，而板子上毫无症状。
 *    `AVE_board.c` 的注释记的就是这条；替身必须同构地实现它，
 *    否则那条顺序约束在桌面上**测不出来**（想清哪一位就清哪一位的替身
 *    会让"读完再清"和"边读边清"表现一致）。
 */
#define __HAL_RCC_CLEAR_RESET_FLAGS() \
  do { host_rcc_csr &= ~RCC_CSR_RESET_FLAGS_MASK; } while (0)

/* ============================================================================
 * 二、HAL 接口（实现见 fake_hal.c）
 * ==========================================================================*/

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *pData, uint16_t Size);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart, const uint8_t *pData,
                                    uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *huart);

/*
 * ── 中断发送的两个接口 ────────────────────────────────────────────────────────
 *   真实 HAL 用 `gState`（不是 `TxState`）表示"这一路在忙"：
 *   `HAL_UART_Transmit_IT()` 在 `gState != HAL_UART_STATE_READY` 时返回 HAL_BUSY；
 *   `HAL_UART_AbortTransmit_IT()` 把 `gState` 清回 READY。
 *
 *   ⚠️ 我们**只**在替身里用这个字段，产品代码一句都没碰过它 ——
 *      这是刻意的：产品代码只该通过 `AVE_Uart_TxBusy()` 判断忙闲，
 *      直接看 `gState` 就等于把 HAL 的私有状态机焊进业务层。
 */
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart, const uint8_t *pData,
                                       uint16_t Size);
HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef *huart);

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState);

/*
 * 🔴 `HAL_Delay` 在本工程里有**两种调用时机**，替身两种都要顶住：
 *    ① `AVE_App_Init()` 里（`osKernelStart()` 之前）—— 上电使能的轴间 50 毫秒走这条；
 *    ② 万一将来有人把它挪进任务 —— 那时它是**忙等**，会独占处理器。
 *    替身不区分这两种，但会记轨迹：延时发生在锁内还是锁外，一眼可查。
 */
void HAL_Delay(uint32_t Delay);

/* 回调（产品代码提供实现，这里只是声明，避免宿主编译器把调用当隐式声明） */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart);

#endif /* HOST_SHIM_STM32F4XX_HAL_H */
