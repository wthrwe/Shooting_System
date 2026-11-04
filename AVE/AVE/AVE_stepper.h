#ifndef __AVE_STEPPER_H
#define __AVE_STEPPER_H

#include <stdint.h>
#include "AVE_config.h"
#include "stm32f4xx_hal.h"				//只为 TIM_HandleTypeDef 这个形参类型

/* 轴名。下标顺序必须与 AVE_stepper.c 里的配置表一一对应，不许插队 */
typedef enum
{
	AVE_STEPPER_AXIS_YAW = 0,			//水平轴(左右)：TIM5_CH4 → PA3
	AVE_STEPPER_AXIS_PITCH,				//俯仰轴(上下)：TIM2_CH3 → PB10
	AVE_STEPPER_AXIS_NUM				//轴数，不是一条轴
} AVE_StepperAxis;

/* 返回码。调用方按它分支，不许只判 0/非 0 */
#define AVE_STEPPER_EOK			(0)		//成功
#define AVE_STEPPER_EBADARG		(-1)	//轴号越界
#define AVE_STEPPER_ERANGE		(-2)	//频率或脉冲数越界
#define AVE_STEPPER_EGATE		(-3)	//门控关着，本次一个脉冲都没出
#define AVE_STEPPER_EBUSY		(-4)	//该轴正在走定位动作，不收新指令
#define AVE_STEPPER_ECONFIG		(-5)	//SelfTest 发现定时器配置与本模块的假设不符，详情看 out->bad

/* 诊断计数，只增不减。除 pulses_total（上了 [HB] 行尾的 p=A,B）以外都不上心跳行，
   要看请连调试器 */
typedef struct
{
	uint32_t freq_updates;		//速度模式改频成功的次数
	uint32_t dir_changes;		//换向次数
	uint32_t move_started;		//位置模式启动次数
	uint32_t move_done;			//位置模式正常走完的次数
	uint32_t move_aborted;		//位置模式被 Stop 或新指令打断的次数
	uint32_t gate_rejects;		//门控关着被拒的次数，涨了就说明有指令没生效
	uint32_t range_rejects;		//频率或脉冲数越界被拒的次数
	uint32_t busy_rejects;		//正在定位被拒的次数
	uint32_t stray_irq;			//认不出的中断，应为 0
	uint32_t dwt_fallbacks;		//DWT 不可用、换向等待走空转退路的次数，应恒为 0
	/* 第 37 次加的：各轴**开机以来的 STEP 脉冲数**，只增不减，按轴分。
	   [HB] 行尾 `p=A,B` 的数据源；也是全工程唯一一条“到底发过脉冲没有”的真判据。
	   为什么不拿 TIMx->CNT 充数（第 36 次就是这么干的，是错的，见 verify.md 第 37 次）：
	     ① pwm_stop() 收尾执行 `inst->EGR = TIM_EGR_UG`，**更新事件会把 CNT 重装成 0**
	        （AVE_stepper.c 里“清 CEN”那一行只负责停计数，清零的是它后面那句 EGR）
	        ⇒ 每次定位走完 CNT 必回 0 ⇒ 拿它当判据天生恒 0，什么都证明不了；
	     ② 就算不清零也不能用：CNT 数的是**定时器时钟节拍**，一个 STEP 脉冲占 ARR+1
	        拍，要除 ARR 才换得回脉冲数，而 ARR 随频率变；
	     ③ CNT 是 32 位，40 kHz 满速约 30 小时绕回一圈，长动作的增量再也算不回来。
	   所以只能在 ISR 里数 —— 那里“一次中断 == 一个 STEP 上升沿”是硬事实。
	   🔴 边界（今天不影响，将来会）：速度模式（SetFreqHz）**不开比较中断**
	      （AVE_stepper.c 的 pwm_run 传 count_it=0）⇒ 它的脉冲不进 ISR、不计入本字段。
	      速度模式目前零调用者，所以本字段今天是完整的。将来 M3.x 视觉接速度模式时，
	      必须同时打开 count_it 并调整 ISR 分支，否则 `p=` 会**静默漏计** ——
	      那正是第 36 次 `clk` 的死法。 */
	uint32_t pulses_total[AVE_STEPPER_AXIS_NUM];
} AVE_StepperStats;

void AVE_Stepper_Init(void);

int AVE_Stepper_SetGate(AVE_StepperAxis axis, uint8_t on);

int AVE_Stepper_IsGateOpen(AVE_StepperAxis axis);

int AVE_Stepper_SetFreqHz(AVE_StepperAxis axis, int32_t hz);

int32_t AVE_Stepper_GetFreqHz(AVE_StepperAxis axis);

/* 读一条轴定时器当前的 CNT 原值（只读，不碰任何状态）。
   ---------------------------------------------------------------------------
   为什么需要它：「本模块到底有没有在出脉冲」这件事，真相只落在定时器里 ——
   pwm_run() 置 CEN、pwm_stop() 清 CEN，CEN=0 时 CNT 就冻在当前值不动。
   所以隔 2 秒采一次、两次读数**相等** ⇔ 这 2 秒一个脉冲都没发。
   在这之前判这条得连 SWD 手动读 TIM2/TIM5->CNT（`[HB]` 行里没有这个字段），
   每次上板都要占着调试器；加了这个接口，它变成一行串口日志可读。

   ⚠️ 能直接比“相等”，前提是 TIM2/TIM5 都是 **32 位**计数器，不会绕回。
      哪天换到 16 位定时器，这条判据必须改成“判增量” —— 否则 CNT 绕一圈回来
      读到的还是同一个数，会把“一直在跑”误读成“没在跑”（正是这类静默误判最费时间）。
   ⚠️ 停机后 CNT **不清零**，冻在当前位置（pwm_stop 只清 CEN，见 AVE_stepper.c:222）。
      所以判据是“两次读数相不相等”，**不是**“等不等于 0”。 */
uint32_t AVE_Stepper_GetCounter(AVE_StepperAxis axis);

int AVE_Stepper_MovePulses(AVE_StepperAxis axis, int32_t pulses, uint32_t hz);

int AVE_Stepper_IsBusy(AVE_StepperAxis axis);

int AVE_Stepper_Stop(AVE_StepperAxis axis);

void AVE_Stepper_PulseFinishedIsr(TIM_HandleTypeDef *htim);

const AVE_StepperStats *AVE_Stepper_Stats(void);

/* ============================================================================
 * 定时器配置自检
 *
 * 为什么需要它：本模块的波形全靠 CubeMX 生成的那几个位（ARPE / OCxM / CCxP /
 * PSC），而这些位**改错了不会有任何运行症状**——不报错、不崩溃、`[HB]` 照样
 * 零漂移，只是"电机转得不对"。这类错只能靠把寄存器读回来对账。
 *
 * bad 的每一位对应一条被依赖的假设，为 1 表示不符
 * ==========================================================================*/

#define AVE_STEPPER_CHK_ARPE	(1UL << 0)	/* CR1.ARPE != 1：同向改频不再走影子寄存器，
                                             * 波形在周期中途被撕开，多出半个脉冲 */
#define AVE_STEPPER_CHK_OCMODE	(1UL << 1)	/* OCxM != PWM 模式 1：脉冲边沿挪到周期别处，
                                             * 脉冲数不变但相位不对 */
#define AVE_STEPPER_CHK_OCPE	(1UL << 2)	/* OCxPE != 1：改频当场生效多一个边沿。
                                             * ⚠️ 这一位同时也是「AVE_Stepper_Init() 没跑到」
                                             * 的判据——本模块只在这里补置它（见
                                             * AVE_stepper.c 的 init_one_axis） */
#define AVE_STEPPER_CHK_CCS		(1UL << 3)	/* CCxS != 00：通道被配成输入捕获，一根脉冲都出不来 */
#define AVE_STEPPER_CHK_POL		(1UL << 4)	/* CCxP != 1：共阳接法要的是"低有效"，而在 STM32 里
                                             * **置 1 才是低有效**（`TIM_OCPOLARITY_LOW` ⇒ `CCxP=1`）。
                                             * 配成 0 则空闲电平落在有效侧，光耦常亮（仍能转、白耗电、抗干扰差）。
                                             * 🔴 本条判据的方向在**第二十八次上板实测中纠正过**：原文写"应为 0"，
                                             * 与 `Core/Src/tim.c` 的 `OCPolarity=LOW` 正面对撞，导致自检永远报
                                             * `bad=0x10`。依据与全过程见 `verify.md` 第二十八次。 */
#define AVE_STEPPER_CHK_CCE		(1UL << 5)	/* CCxE != 1：定时器撒手不管引脚，引脚悬空。
                                             * 那根线挂在驱动器光耦阴极上，旁边还是脉冲线 */
#define AVE_STEPPER_CHK_PSC		(1UL << 6)	/* PSC 与按 RCC 实测时钟算出的应然值不符：
                                             * 所有速度成比例地错，最像"PID 参数没调好" */
#define AVE_STEPPER_CHK_CMS		(1UL << 7)	/* CR1.CMS != 0：非边沿对齐，一个周期两个边沿
                                             * ⇒ 脉冲数翻倍、转角翻倍 */

/* 自检结果快照。字段全部是"读回来的实际值"，不是"我们打算配成的值" */
typedef struct
{
	uint32_t timer_clk_hz;					//按 RCC 分频推算出的定时器输入时钟
	uint32_t timebase_hz;					//本模块要求的时间基准（= AVE_STEPPER_TIMEBASE_HZ）
	uint32_t expected_psc;					//由上面两值算出的 PSC 应然值
	uint32_t bad[AVE_STEPPER_AXIS_NUM];		//每轴不符项，位定义见上方 AVE_STEPPER_CHK_*
	uint32_t psc[AVE_STEPPER_AXIS_NUM];		//实际 PSC
	uint32_t ccmr2[AVE_STEPPER_AXIS_NUM];	//实际 CCMR2 原值（便于人眼复核）
	uint32_t ccer[AVE_STEPPER_AXIS_NUM];	//实际 CCER 原值
} AVE_StepperSelfTest;


int AVE_Stepper_SelfTest(AVE_StepperSelfTest *out);

#endif
