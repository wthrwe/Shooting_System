#ifndef AVE_GIMBAL_H
#define AVE_GIMBAL_H

#include <stdint.h>
#include "AVE_config.h"

/* ============================================================================
 * AVE_gimbal.h —— 双轴云台聚合层（脉冲后端的薄壳）
 *
 * 【本文件负责什么】
 *   1. 把"角度世界"翻译成"脉冲世界"：角度 → 带符号的脉冲数；
 *      转/分 → 步进频率 Hz（换算式 = 转/分 × 每度脉冲数 × 6）。
 *   2. 记两轴的【软件累计脉冲】—— 下发量纲（3200 脉冲 = 1 圈），
 *      它是绝对移动（MoveAbsDeg）唯一的位置基准。
 *
 * 【本文件不负责什么】
 *   - 波形生成 / 频率 ↔ ARR 换算 / DIR 引脚电平与建立时间 —— 全在 AVE_stepper.{h,c}
 *   - 串口收发 / 应答解析 / 总线仲裁 —— 脉冲后端两轴各占一个定时器
 *     （YAW(水平/左右) → TIM5_CH4/PA3，PITCH(俯仰/上下) → TIM2_CH3/PB10），既没有共享总线，也没有应答可排
 *   - PID、视觉下发、文本命令解析 —— 上层的事
 *
 * 【硬约束（不许破坏）】
 *   1. 轴枚举顺序必须与 AVE_stepper.h 的 AVE_StepperAxis 一一对应，
 *      本文件靠强制转换把两者当成同一套下标用。文件内有编译期断言守着。
 *   2. 软件累计脉冲是【下发】量纲，不是编码器【回读】量纲。回读是 65536 = 1 圈，
 *      两者差 20.48 倍 —— 混用会让角度整体偏 20 倍，而且不报任何错。
 *   3. 门控是【纯软件门】：En 引脚不接、电机侧配 Hold
 *      ⇒ 关门只会让脉冲指令被拒（返回 EGATE），
 *      **既不会松轴，也不会让电机凉下来，更不能当急停用**。
 *      本工程已经为"把门控当成使能"撤回过一次结论，别再踩。
 * ==========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 一、轴与返回码 ===== */

/** 轴名。顺序必须与 AVE_stepper.h 的 AVE_StepperAxis 一致（有编译期断言守着）。 */
typedef enum
{
  AVE_GIMBAL_AXIS_YAW = 0,   /**< 水平轴(左右) —— TIM5_CH4 / PA3 */
  AVE_GIMBAL_AXIS_PITCH,      /**< 俯仰轴(上下) —— TIM2_CH3 / PB10 */
  AVE_GIMBAL_AXIS_NUM        /**< 轴数，不是一条轴 */
} AVE_GimbalAxis;

/* 返回码刻意与 AVE_stepper.h 的五条一一对齐，调用方可以拿同一套 switch 处理两层。 */
#define AVE_GIMBAL_EOK          (0)    /**< 成功：指令已被后端受理 */
#define AVE_GIMBAL_EBADARG      (-1)   /**< 入参非法：轴号越界 / 角度或转速非有限值 */
#define AVE_GIMBAL_ERANGE       (-2)   /**< 超出后端可表示的范围（脉冲数 / 转速 / 频率） */
#define AVE_GIMBAL_EGATE        (-3)   /**< 门控关着 ⇒ 本次一个脉冲都没出 */
#define AVE_GIMBAL_EBUSY        (-4)   /**< 该轴正在走定位动作 ⇒ 视觉这一拍让路，不排队不打断 */

/* ===== 二、诊断计数 =====
 * 累计值，只增不减。心跳行里没有它们，要看请连调试器。
 * ⚠️ 后端 AVE_stepper.c 另有一份更细的计数（AVE_StepperStats），两处不要混读。 */

typedef struct
{
  uint32_t moves_accepted;  /**< 定位动作受理次数（后端确实开始走了） */
  uint32_t moves_bypassed;  /**< 撞上"该轴定位进行中"而让路 —— 仲裁是否在生效，只能靠它看 */
  uint32_t moves_rejected;  /**< 被拒：入参非法 / 转速越界 / 门控关 —— 只要涨就说明有指令没生效 */
  uint32_t gate_changes;    /**< 门控状态实际发生翻转的次数 */
} AVE_GimbalStats;

/* ===== 三、生命周期 ===== */

/**
  * 函    数：清空云台全部状态（累计脉冲归零、计数清零）
  * 参    数：无
  * 返 回 值：无
  * 注意事项：**不碰后端**，也不发任何运动指令 —— 纯粹清本层的记账。
  *           在调度器起来之前调用即可，本函数不含任何阻塞。
  */
void AVE_Gimbal_Init(void);

/* ===== 四、角度换算（公开，纯函数） ===== */

/**
  * 函    数：把角度拆成脉冲数（绝对值）+ 方向电平
  * 参    数：deg 角度（可负）；steps_per_deg 每度脉冲数；pos_dir_level "正角度"对应的 DIR 电平
  *          pulses 输出脉冲绝对值；dir 输出方向电平
  * 返 回 值：AVE_GIMBAL_EOK 成功；EBADARG 空指针 / 非正系数 / 系数为 NaN；ERANGE 乘积超出范围
  * 注意事项：失败时两个输出一个都不写（与本模块其它接口同一套约定，防止调用方读到半个结果）。
  *           角度不足半个脉冲时脉冲数为 0、方向仍按符号给，不当错误。
  */
int AVE_Gimbal_SplitPulses(float deg, float steps_per_deg, uint8_t pos_dir_level,
                           uint32_t *pulses, uint8_t *dir);

/* ===== 五、门控（**不是**使能，见文件头的硬约束 3） ===== */

/**
  * 函    数：开 / 关一条轴的软件门控
  * 参    数：axis 轴号；on 1=开门（允许出脉冲）、0=关门（脉冲指令一律被拒）
  * 返 回 值：AVE_GIMBAL_EOK 或 EBADARG
  * 注意事项：🔴 关门只是"不让新脉冲出去"，**不会让电机松轴、不会让电机凉**。
  *           关门时后端会顺带把这条轴正在走的定位动作停掉（AVE_Stepper_SetGate 内部行为）。
  */
int AVE_Gimbal_SetGate(AVE_GimbalAxis axis, uint8_t on);

/** 这条轴的门控是否开着（1 开 / 0 关或轴号非法）。真身在后端，本层不另存一份。 */
int AVE_Gimbal_IsGateOpen(AVE_GimbalAxis axis);

/**
  * 函    数：两轴一起开 / 关门控
  * 参    数：on 1=开两轴；0=关两轴
  * 返 回 值：EOK 两条都成功；EBADARG 至少一条失败（**已成功的那条不回滚**）
  * 注意事项：本函数【不再阻塞】—— 脉冲后端两轴各有独立定时器，不存在"一条一条发、中间等帧间隔"
  *           这件事（那是旧的共享串行总线才需要的）。所以在任务里调用是安全的。
  */
int AVE_Gimbal_SetGateAll(uint8_t on);

/* ===== 六、运动 =====
 * 通用约定：返回 EOK 表示【后端已受理】，不等于电机已经走完 ——
 *           要问走完没有，用 AVE_Gimbal_IsBusy()。 */

/**
  * 函    数：持续转速模式 —— 只改频率，不指定走多少步（视觉 PID 的入口）
  * 参    数：axis 轴号；rpm 带符号转速（转/分），正负表示方向，0 表示停
  * 返 回 值：EOK / EBADARG / ERANGE / EGATE / EBUSY
  * 注意事项：🔴 该轴正在走定位动作时返回 EBUSY 且【什么都不做】——
  *           这是"定位优先、视觉让路"的仲裁：撞上就跳过这一拍，不排队也不打断。
  *           特别地，rpm == 0 也照样让路：若不先挡，后端的"0 Hz = 停"会把正在走的定位打断。
  */
int AVE_Gimbal_SetSpeedRpm(AVE_GimbalAxis axis, float rpm);

/**
  * 函    数：相对当前软件零点移动一个角度，走完自动停
  * 参    数：axis 轴号；deg 角度（可负）；rpm 走这段距离用的转速（绝对值，可负则反向）
  * 返 回 值：EOK / EBADARG / ERANGE / EGATE / EBUSY
  * 注意事项：① 角度不足半个脉冲 ⇒ 0 脉冲，后端按"空动作"立刻返回 EOK，**不置忙碌**；
  *           ② 成功后软件累计脉冲按【实际下发】的脉冲数累加；失败时一个字都不动；
  *           ③ 后端单次脉冲上限 AVE_STEPPER_MAX_MOVE_PULSES，超过直接 ERANGE（见 .c 里的注释）。
  */
int AVE_Gimbal_MoveRelDeg(AVE_GimbalAxis axis, float deg, float rpm);

/**
  * 函    数：绝对角度移动 —— 相对【软件累计脉冲零点】
  * 参    数：同 AVE_Gimbal_MoveRelDeg
  * 返 回 值：同 AVE_Gimbal_MoveRelDeg
  * 注意事项：🔴 "绝对"是相对软件自己记的零点，不是驱动器里的真实位置。
  *           丢步 / 堵转 / 被急停打断都会让累计值与真实位置分叉，之后会朝错误偏移量移动
  *           且【不报任何错】。使用约定：先回零（或 ZeroPulses），再绝对移动。
  */
int AVE_Gimbal_MoveAbsDeg(AVE_GimbalAxis axis, float deg, float rpm);

/**
  * 函    数：让一条轴立即停止（会打断正在走的定位动作）
  * 参    数：axis 轴号
  * 返 回 值：EOK 或 EBADARG
  * 注意事项：这是"停"，与 AVE_Gimbal_SetGate(0) 的"关门"是两件事：
  *           停下来之后门还是开的，下一条指令照样能走；本函数也是【唯一的】急停入口
  *           （视觉侧的 SetSpeedRpm 撞上忙碌只会让路，不会打断）。
  */
int AVE_Gimbal_Stop(AVE_GimbalAxis axis);

/** 两轴一起立即停止。不阻塞。 */
int AVE_Gimbal_StopAll(void);

/* ===== 七、状态查询 ===== */

/** 这条轴是否还有未走完的定位动作（1 有 / 0 无）。真身在后端，本层不另存一份。 */
int AVE_Gimbal_IsBusy(AVE_GimbalAxis axis);

/**
  * 函    数：读软件累计脉冲（下发量纲：3200 脉冲 = 1 圈）
  * 参    数：axis 轴号
  * 返 回 值：累计脉冲值，已钳在 int32 范围内
  * 注意事项：⚠️ **不是**编码器回读计数（那是 65536 = 1 圈），两者差 20.48 倍。
  */
int32_t AVE_Gimbal_GetPulses(AVE_GimbalAxis axis);

/** 把两轴的软件累计脉冲清零（配合"先回零，再绝对移动"）。不发任何指令。 */
void AVE_Gimbal_ZeroPulses(void);

/** 取诊断计数快照。返回的是内部存储的只读地址，不要写它。 */
const AVE_GimbalStats *AVE_Gimbal_Stats(void);

#ifdef __cplusplus
}
#endif

#endif /* AVE_GIMBAL_H */
