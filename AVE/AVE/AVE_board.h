

#ifndef AVE_BOARD_H
#define AVE_BOARD_H

#include "main.h"
#include "usart.h"
#include "gpio.h"
#include "tim.h"        /* 需要 htim2 / htim5 的声明 */

/* ===== 一、引脚映射 =====
 * 这些宏不参与初始化（初始化在 CubeMX 生成的 usart.c / gpio.c），
 * 只为让"哪个外设接在哪根线"有唯一、可搜索的出处。 */

/* ── USART1：视觉模块 MaixCAM（只需 RX）────────────────────────────── */
/* 为何不是 PA9/PA10：实板这两脚没引到排针。USART1 走 PB6/PB7（都是 AF7），
 * 只换引脚不换串口，huart1 与通道枚举都不变。⚠️ 宏本身不参与初始化，
 * 真正生效的是 HAL_UART_MspInit 里的引脚配置，两侧必须一致。 */
#define AVE_CAM_UART_INSTANCE       USART1
#define AVE_CAM_UART_AF             GPIO_AF7_USART1
#define AVE_CAM_UART_TX_PORT        GPIOB
#define AVE_CAM_UART_TX_PIN         GPIO_PIN_6     /* 本工程不使用，可悬空 */
#define AVE_CAM_UART_RX_PORT        GPIOB
#define AVE_CAM_UART_RX_PIN         GPIO_PIN_7     /* MaixCAM 的 TX 接这里 */

/* ── USART2：ZDT 电机总线已作废，宏已删除 ──────────────────────────────
 * 这里曾有 6 个宏描述 USART2 挂 PA2/PA3。电机改 STEP/DIR 后 USART2 零引用，宏已删。
 * ⇒ 接线以实配为准。🔴 2026-10-10 按实物实测【对调两轴功能】：此前文档一律写反 ——
 *   **PB10 那台＝俯仰(上下)、PA3 那台＝水平(左右)**。水平＝STEP PA3/TIM5_CH4、DIR PA5；
 *   俯仰＝STEP PB10/TIM2_CH3、DIR PA4。两轴必须各用一个定时器，否则共计数器、频率锁死。
 * ⚠️ 改引脚要同时改三处：本组宏、AVE.ioc、生成代码，只改宏不会有任何报错。 */
#define AVE_YAW_STEP_PORT           GPIOA
#define AVE_YAW_STEP_PIN            GPIO_PIN_3    /* 水平(左右)：TIM5_CH4 */
#define AVE_PITCH_STEP_PORT          GPIOB
#define AVE_PITCH_STEP_PIN           GPIO_PIN_10   /* 俯仰(上下)：TIM2_CH3 —— 不是 PA2（旧文档历史错） */
#define AVE_YAW_DIR_PORT            GPIOA
#define AVE_YAW_DIR_PIN             GPIO_PIN_5    /* 与水平 STEP(PA3) 同属一台电机 */
#define AVE_PITCH_DIR_PORT           GPIOA
#define AVE_PITCH_DIR_PIN            GPIO_PIN_4    /* 与俯仰 STEP(PB10) 同属一台电机 */

/* ── USART3：云端关闭时兼作调试口；开启时归 ESP8266 ────────────────────── */
#define AVE_ESP_UART_INSTANCE       USART3
#define AVE_ESP_UART_AF             GPIO_AF7_USART3
/* TX 不是 PB10：被 TIM2_CH3 占走，USART3_TX 重映射到 PD8。照旧接 PB10 只收乱码。 */
#define AVE_ESP_UART_TX_PORT        GPIOD
#define AVE_ESP_UART_TX_PIN         GPIO_PIN_8     /* CH340 的 RX 接这里 */
#define AVE_ESP_UART_RX_PORT        GPIOB
#define AVE_ESP_UART_RX_PIN         GPIO_PIN_11    /* CH340 的 TX 接这里 */

/* ── 激光 ───────────────────────────────────────────────────────────── */
#define AVE_LASER_GPIO_PORT         GPIOC
#define AVE_LASER_GPIO_PIN          GPIO_PIN_13


/* ===== 二、串口句柄的逻辑名 =====
 * 让"物理外设"与"业务角色"对应起来，将来换外设只改这一处。 */
#define AVE_HUART_CAM               (&huart1)   /* 视觉：MaixCAM */
/* AVE_HUART_MOTOR(&huart2) 已删除：随 USART2 禁用失效，无法编译 */
#define AVE_HUART_ESP               (&huart3)   /* 调试口 / ESP8266 */

/* ── 定时器句柄的逻辑名 ────────────────────────────────────────────────
 * 让"哪一路 PWM 在出脉冲"有唯一、可搜索的出处。AVE_stepper.c 通过这两条宏真正
 * 操作 htim2/htim5。⚠️ AVE_STEP_*_CHANNEL 须与三处一致：tim.c 的 ConfigChannel
 * 通道号、MspPostInit 复用脚、.ioc 图形配置，缺一不出脉冲且不报错。 */
#define AVE_HTIM_YAW_STEP           (&htim5)          /* 水平轴 STEP：TIM5_CH4 → PA3 */
#define AVE_HTIM_PITCH_STEP          (&htim2)          /* 俯仰轴 STEP：TIM2_CH3 → PB10 */
#define AVE_STEP_YAW_CHANNEL        TIM_CHANNEL_4
#define AVE_STEP_PITCH_CHANNEL       TIM_CHANNEL_3
/* 🔴 改上面这 4 行，必须同时改 AVE_stepper.c 的 s_cfg[] 表：那张表里
 *    TIM_IT_CCx / TIM_CCER_CCxE / TIM_CCMR2_OCx* / TIM_CCMR2_CCxS / TIM_CCER_CCxP 共 14 个字面量
 *    是**写死的通道号**，既不跟本组宏走、也不会有任何编译告警。漏改的症状：
 *    自检照过、脉冲照出，但两条轴的"通道寄存器"与"中断源"挂反 —— 表现为某条轴走完不停、
 *    另一条轴一发指令就立刻报完成。AVE_stepper.c:26 的编译期断言会先把这一类漏改挡在编译期。 */

/* ===== 三、激光极性 ===== */

/** 激光是高电平点亮还是低电平点亮。
 *  🔴 改成 0 之前必须先改 CubeMX 里 PC13 的初始电平：MX_GPIO_Init() 无条件写 RESET，
 *     若本宏为 0（低电平点亮）就等于上电瞬间点亮激光，中间有窗口期对着人眼出光。
 *     本宏为 1 时无此问题（RESET 正好是"关"）。 */
#define AVE_LASER_ACTIVE_HIGH       1

/** 由极性推导出某开关状态对应的引脚电平。抽成独立宏，让这段安全映射可被编译期测试覆盖。 */
#if AVE_LASER_ACTIVE_HIGH
  #define AVE_LASER_LEVEL(on)       ((on) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#else
  #define AVE_LASER_LEVEL(on)       ((on) ? GPIO_PIN_RESET : GPIO_PIN_SET)
#endif

/* ===== 四、启动故障码 =====
 * CubeMX 的 Error_Handler() 原本是静默死循环——停机且不留痕迹，与"卡死/缺电"难区分。
 * 启动失败必须留下能被读出的数字。只登记"启动阶段失败、且导致功能缺失"的事件。 */


typedef enum
{
  AVE_BOARD_BOOT_FAIL_NONE = 0,   /* 一切正常（默认值，也是唯一的好值） */
  AVE_BOARD_BOOT_FAIL_HAL  = 1,   /* Error_Handler() 被 HAL 调用：时钟/外设初始化失败 */
} AVE_BoardBootFail;

/**
  * 函    数：记录一个启动故障码
  * 参    数：code 故障码
  * 返 回 值：无
  * 注意事项：先到先得：只有第一个码留存，后续只累加计数（留根因不留症状）。
  *           传 0 是空操作（表示"无故障"）。码一旦记下本上电周期内不清除。只写内存，可在中断里调用。
  */
void AVE_Board_NoteBootFail(uint32_t code);

/**
  * 函    数：取第一个启动故障码
  * 参    数：无
  * 返 回 值：故障码，无故障时返回 AVE_BOARD_BOOT_FAIL_NONE
  */
uint32_t AVE_Board_BootFailCode(void);

/**
  * 函    数：取启动故障发生次数
  * 参    数：无
  * 返 回 值：发生次数，同一个码记多次也会累加；无故障时为 0
  */
uint32_t AVE_Board_BootFailCount(void);

/* ──────────────────────────────────────────────────────────────────────────
 * 复位原因 —— 没有万用表时判断"电够不够"的唯一软件手段。
 * 它记"上一次启动怎么结束的"（启动之前），与上面 bf=（启动之中）是两回事，不能合并。
 * ──────────────────────────────────────────────────────────────────────────*/

/** 复位原因位掩码。可能同时置多位，判据须逐位与、不能等某值。 */
typedef enum
{
  AVE_RST_POR  = (1u << 0),   /* 上电复位 POR */
  AVE_RST_BOR  = (1u << 1),   /* 欠压复位 BOR */
  AVE_RST_PIN  = (1u << 2),   /* NRST 引脚被拉低 */
  AVE_RST_SOFT = (1u << 3),   /* 软件复位 SFT */
  AVE_RST_IWDG = (1u << 4),   /* 独立看门狗 IWDG */
  AVE_RST_WWDG = (1u << 5),   /* 窗口看门狗 WWDG */
  AVE_RST_LPWR = (1u << 6),   /* 低功耗复位 LPWR */
} AVE_ResetCause;

/**
  * 函    数：读本次启动的复位原因
  * 参    数：无
  * 返 回 值：复位原因位掩码，可同时置多位；全没置位返回 0
  * 注意事项：硬件只置位、不互斥（掉电瞬间 POR/BOR 常同亮），判据须逐位与。
  *           读完全部位再清标志（RMVF 是清全部的单一开关）。只调一次、幂等。
  *           调试器/手动复位会污染 PIN=1；真正要看 BOR/IWDG/SOFT。
  */
uint32_t AVE_Board_ResetCause(void);

/* ===== 五、对外接口 ===== */

/**
  * 函    数：板级初始化
  * 参    数：无
  * 返 回 值：无
  * 注意事项：在 MX_GPIO_Init() / MX_USART*_UART_Init() 之后调用。这里按极性宏把激光置关，
  *           保证两种极性下"关"语义都成立。若已登记启动故障，激光被强制置关且之后点不亮。
  */
void AVE_Board_Init(void);

/**
  * 函    数：激光开关
  * 参    数：on 非 0 点亮
  * 返 回 值：无
  * 注意事项：fail-safe：已登记启动故障则强制不出光——启动阶段上层代码可能根本没创建，
  *           指望它关激光不现实，故由本函数兜底（"不确定机器是好的就不出光"）。
  */
void AVE_Laser_Set(uint8_t on);

#endif /* AVE_BOARD_H */
