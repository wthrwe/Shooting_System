/* AVE_app.h —— 应用装配入口：把板级+串口+任务装成一台能跑的机器 */
/* 拆两个口：AVE_App_Init 纯外设（osKernelInitialize 前）；AVE_App_RtosInit 建锁建任务（之后） */
#ifndef AVE_APP_H
#define AVE_APP_H

#include "AVE_config.h"

/* 用 cmsis_os2.h 而非 cmsis_os.h：前者是 CMSIS-RTOS v2 门面，不拖入 FreeRTOS 私有世界；
   本模块只用 osThreadNew/osMutexNew/osDelayUntil/osKernelGetTickCount 等标准接口 */
#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 一、初始化 ======================== */

/**
  * 函    数：非 RTOS 部分初始化（osKernelInitialize 之前调用）
  * 参    数：无
  * 返 回 值：无（两步都无失败路径，硬造假返回值只会骗人）
  * 注意事项：① 在调度器启动前就打开串口中断，让视觉上电即收字节进环形缓冲，
  *           不依赖任务；否则"中断打开→任务开始消费"之间的数据永久丢失。
  *           ② 按极性先把激光置关（安全）。
  */
void AVE_App_Init(void);

/**
  * 函    数：RTOS 部分初始化，创建互斥量与全部任务
  * 参    数：无
  * 返 回 值：0 全部创建成功；-1 有对象创建失败
  * 注意事项：① 必须在 osKernelInitialize 之后、osKernelStart 之前调用。
  *           ② 任务创建返回值逐个检查：osThreadNew 失败静默返回 NULL，漏检=某个
  *           功能永远不工作却看似正常（心跳在打、灯在闪就是不动），失败多因堆不足。
  *           ③ 失败靠全局 FailName + Error_Handler 死循环上报——此时调度器未启动，
  *           日志根本发不出去，不能指望串口。
  */
int AVE_App_RtosInit(void);

/* ======================== 二、共享资源 ======================== */

/**
  * 函    数：取电机总线互斥量
  * 参    数：无
  * 返 回 值：互斥量句柄
  * 注意事项：必须是带优先级继承的互斥量（osMutexPrioInherit）：两轴优先级不同，
  *           无继承会产生优先级反转，表现为偶发卡住几十毫秒。
  */
osMutexId_t AVE_App_MotorMutex(void);

/* ======================== 三、健康度 ======================== */

/* 栈溢出标志：任务栈被写穿时由 vApplicationStackOverflowHook 置 1，心跳下一轮报出来；
   钩子运行时栈已坏不能打印，所以只置标志，溢出的是谁见下面那个指针 */
extern volatile uint32_t AVE_AppStackOverflow;

/* 溢出那个任务的名字（FreeRTOS 传入钩子的任务名），未溢出为 NULL；Keil Watch 里可看 */
extern const char *volatile AVE_AppStackOverflowTask;

/* 第一个创建失败的 RTOS 对象名（"MotorMutex"/"VisionTask"/…），全部成功为 NULL。
   存全局而非当场打印：失败发生在 osKernelStart 前，日志根本发不出去 */
extern const char *AVE_App_FailName;

#ifdef __cplusplus
}
#endif

#endif /* AVE_APP_H */
