/* 唯一调参入口。所有可调常数集中在纯宏这里，逻辑代码不许出现裸数字。 */

#ifndef AVE_TUNING_H
#define AVE_TUNING_H

/* ============================================================================
 * 一、功能开关（plan.md §1.3）
 * ==========================================================================*/

/* 视觉自动跟踪总开关。关掉后云台只响应串口文本命令。 */
#define AVE_ENABLE_VISION               1

/* 视觉闭环下发开关。1 = 视觉误差驱动两轴转速；0 = 只解析、不驱动。
   🔴 默认 0 是**安全要求**，不是没做完：两轴方向符号（AVE_VISION_YAW/PITCH_SIGN）
      至今**没有在实物上核过**，而符号反了的现象是"云台越追越远、目标几帧内出画"。
      第一次试车顺序：① 保持 0 烧一版，确认 [HB]/[VS] 与门控状态正常；
                      ② 拨 1 再烧，把靶心放在画面中心偏右一点，看云台是否**转向靶心**；
                      ③ 若转向相反，改 AVE_VISION_*_SIGN 的符号，不要动接线。
   ⚠️ 它同时控制开机是否开两轴门控（见 AVE_app.c 的 AVE_App_Init）。 */
#define AVE_ENABLE_VISION_TRACK         1

/* 云端（ESP8266 + MQTT）。非核心任务，默认关。 */
#define AVE_ENABLE_CLOUD                0

/* 电机位置回读（0x30 等查询命令）。开着会让总线变吵，调试时再开。 */
#define AVE_ENABLE_MOTOR_READBACK       0

/* 云台层【接线】开关。1 = 已接线（2026-10-06 第 35 次拨回）；0 = 不接线。
   ⚠️ 它【不】控制 AVE_gimbal.c 是否参与编译 —— 薄壳已零依赖（不碰 UART/HAL/RTOS），
   永远参与编译。这样「文件改了但其实根本没被编译」这类静默失效不会再发生；
   真正被本宏挡住的，是 AVE_app.c 里那句 AVE_Gimbal_Init()。
   🔴 接线 ≠ 会动：① 门控默认关（fail-safe，见 AVE_gimbal.h 硬约束 3）；
      ② 视觉 → 云台的 PID 下发还没写（M3.x）。
      所以上电就该看到两轴脉冲计数 p 不涨 —— 那是设计，不是故障。拨 0 时也是这个现象。
      第 37 次起 `[HB]` 行尾带 `p=A,B`（两轴**开机以来的脉冲数**），不用再连 SWD。
      🔴 第 36 次那个 `clk=A,B`（TIMx->CNT 原值）**已废弃**：pwm_stop 收尾的 EGR=UG
         把 CNT 清成 0，它天生恒 0，证明不了“没发脉冲” —— 那一轮验收判据是空的。
         详见 verify.md 第 37 次。
   引脚：PB10=TIM2_CH3 俯仰 STEP、PA3=TIM5_CH4 水平 STEP、PA4/PA5=DIR（🔴 2026-10-10 按实物对调）。 */
#define AVE_ENABLE_GIMBAL               1

/* 上电两轴点动自检（约 0.9 秒）。第一次接电机时很有用，装好后可关。 */
#define AVE_ENABLE_STARTUP_MOTOR_TEST   1


/* ============================================================================
 * 二、通信参数
 * ==========================================================================*/

/* 视觉串口（USART1）波特率。必须与 MaixCAM 侧一致。 */
#define AVE_CAM_UART_BAUDRATE           115200

/* 「正方向」对应的 DIR 引脚电平：1=高，0=低。
   这是机械安装事实，只能实测，推不出来；装反则闭环一开始就背道而驰。
   改它不需要重新标定每度脉冲数。当前均为 1，尚未在实物上核过。 */
#define AVE_YAW_POS_DIR_LEVEL           1
#define AVE_PITCH_POS_DIR_LEVEL         1

/* 每路串口环形缓冲大小（字节）。1024 够一帧视觉数据 + 突发余量。
   必须为 2 的幂（下方有编译期检查）—— 环形缓冲用掩码代替取模，非 2 的幂会越界。 */
#define AVE_UART_RX_BUF_SIZE            1024

/* 阻塞发送超时（毫秒）。115200 发 20 字节约 1.7ms，50ms 容得下最坏情况又不卡死任务。 */
#define AVE_UART_TX_TIMEOUT_MS          50

/* 每路串口的非阻塞发送缓冲（字节）。HAL_UART_Transmit_IT 要求发完前内存一直有效，
   故由 AVE_uart.c 持有静态缓冲，调用方拷入。不是 2 的幂限制，只是长度上限，
   超长的发送被拒绝（ENOSPC）而非截断。128：放得下 100 字节日志与电机帧。 */
#define AVE_UART_TX_BUF_SIZE            128




/* ============================================================================
 * 三、视觉通道（plan.md §5.6 / §6.3）
 * ==========================================================================*/

/* 视觉画面分辨率。必须严格等于 MaixCAM 实际输出分辨率，否则像素→角度换算恒偏（云台稳定偏一侧）。
   ★ 2026-10-08 按实测改 224×224：上游相机开在模型输入尺寸上 ⇒ 它报的 cx/cy 就在 224 画布里（旧值 640×480 的依据"感光 4:3"是猜测、已作废；代价见 verify.md 第四十三次）。 */
#define AVE_VISION_SCREEN_W             224
#define AVE_VISION_SCREEN_H             224

/* 画面中心（由上面两值派生，不要单独改）。 */
#define AVE_VISION_CENTER_X             (AVE_VISION_SCREEN_W / 2)
#define AVE_VISION_CENTER_Y             (AVE_VISION_SCREEN_H / 2)

/* 像素死区。偏差小于它不动作。太小 → 中心附近微抖（嗡鸣）；太大 → 静态精度差。 */
#define AVE_VISION_TOL_PX               8

/* 视觉坐标一阶低通系数 0~100。100=不滤波（默认）。YOLO 框跳致电机嗡鸣可降到 30~50，
   但滤波多一层延迟易振荡，优先试其它手段。 */
#define AVE_VISION_FILTER_ALPHA         100

/* 视觉断流判定（毫秒）。超时没收到新帧判视觉挂 → 停机。 */
#define AVE_VISION_RX_TIMEOUT_MS        200

/* 单条视觉行最大字节数（传给 AVE_Uart_ReadLine 的 cap）。协议契约，不是细节：
   更长的行被整行丢弃并计 line_spilled。32 的来历：合法写法最多 19 字节，留 3 倍余量。 */
#define AVE_VISION_LINE_MAX             32

/* 单次 AVE_Vision_Poll 最多处理的行数。排空必须有硬上限：视觉任务优先级最高，
   最坏耗时必须可证；若对端持续灌垃圾，无界排空会饿死心跳任务（唯一诊断手段）。
   50Hz 下正常 1 行，16 是 8 倍余量，触顶计 backlog。 */
#define AVE_VISION_MAX_LINES_PER_POLL   16

/* [VS] 行发送周期（毫秒）。必须与心跳错相且不能短于它（硬约束）：
   AVE_Log 非阻塞、发送忙整条丢弃，同一次循环打两条必丢第二条。
   心跳在偶数秒、[VS] 只在奇数秒发，绝对时刻延时永久错相。 */
#define AVE_VISION_VS_LOG_PERIOD_MS     2000

/* 闭环 PID 增益 —— 增量式，两轴各一组：Δu = Kp·Δe + Ki·e + Kd·(Δe−Δe_prev)。
   Ki=0 时退化为纯比例 u=Kp·e，故先只给 P 试方向，再加 I。
   Kp 上限由闭环延迟（曝光+YOLO+串口+控制周期≈50~100ms）决定，振荡时该降分辨率
   而不是硬拧 Kp。0.10/0.00/0.01 只是能上电试车的量级，必须实测整定。
   ⚠️ 2026-10-20 第七十三次：一度降到 0.03（怕 53 px 处 4.5 rpm 过冲），**又改回 0.10**。
      依据是真机日志反算：旧镜像 Kp=0.10 下 ex=13→hz=27、+29→112、−13→−27、−21→−69、
      +25→91、+17→48 **六点全中**；且那些日志里 |ex| 全程 ≤29 px、`dc` 0→1→2（误差过零
      就换向）、**不跑到底** ⇒ 0.10 在小偏差区是**稳的**。⇒ 过冲风险改由跟踪限速兜大偏差，
      Kp 不动。**没有 53 px 偏差区的实测数据前，不许再拿"过冲"当理由降 Kp。** */
#define AVE_VISION_YAW_PID_KP           0.10f
#define AVE_VISION_YAW_PID_KI           0.00f
#define AVE_VISION_YAW_PID_KD           0.01f

#define AVE_VISION_PITCH_PID_KP         0.10f
#define AVE_VISION_PITCH_PID_KI         0.00f
#define AVE_VISION_PITCH_PID_KD         0.01f

/* 输出限幅（转/分）。唯一抗积分饱和机制，也是安全上限，限得越小越安全。先 200 试车。 */
#define AVE_VISION_MAX_RPM_X            200
#define AVE_VISION_MAX_RPM_Y            200

/* 跟踪限速（转/分，整数 —— 下面要用 #if 查它，浮点在 #if 里非法）。
   它与上面两条是**两道闸**：这里先夹，MAX_RPM 只管"后端能不能表示"。
   ⚠️ 2026-10-20 第七十二~七十三次：10 → **2** 转/分（12°/秒）。它现在的作用**不是**判方向
     （方向已由「`hz` 与 `ex` 同号」定案 = +1），而是**给大偏差兜底**：
     Kp=0.10 时 `rpm = 0.1×(ex−8)` ⇒ **|ex| ≤ 28 px 不受影响**（细调仍是 1.7 rpm 那种量级，
     与已实测跟踪良好的镜像逐位一致），**|ex| > 28 px 才夹到 2 rpm** —— 而"一出现靶子就跑到端"
     正是那一档（实测出现过的最大偏差 **55 px**）。⇒ 小偏差保住响应，大偏差限住速度。
     ⚠️ 别拿它当 PID 输出限幅的替代：`MAX_RPM_X/Y = 200` 只管"后端能不能表示"，不是安全上限。 */
#define AVE_VISION_TRACK_MAX_RPM        2

/* 轴向符号 ±1。装反就改这里不要改接线。判据看它是「追过去」还是「逃开」。
   通电测此项务必先关激光，否则云台一逃开激光就扫别处去了。
   ⚠️ 2026-10-20 实测记录（第七十~七十一次）：
     · 初值 YAW=+1 / PITCH=-1 ⇒ 用户报"两轴都反"；
     · 双翻成 YAW=-1 / PITCH=+1 后，PITCH 正常，**YAW 仍是逃开**：
       `[TR]` 实测 `ex=+53 → hz=-240`（-4.5 rpm），两秒内靶子从 +32 px 直接丢帧
       （`nv=1 → 0`）⇒ 画面被转走；最后**顶到机械端**"到一个点就不动"。
     · 结论：**YAW 的正方向与 PITCH 相反**，故 YAW 回到 +1、PITCH 保持 +1。
   🔑 若 YAW 再出现"两个符号都朝同一个方向转"，那不是符号问题 ——
      是 PA5→驱动器 DIR 那根线（`[TR]` 的 `dir=` 字段可分辨：dir 电平在翻 = 软件侧没问题）。
   错符号的现象是「云台一追就逃」；但反过来**符号对了也会一直跑** —— 那是速度问题。
   ⚠️ 2026-10-20 第七十二次：把 YAW 改成 +1 后用户实测"一出现靶子又朝刚才的反方向
      一直跑，仍然没有跟踪效果"（PA5 已确认接驱动器 DIR）⇒ 两个符号各跑一个方向、
      都不跟踪 ⇒ **符号不是唯一变量**，上面第 148 行的结论作废。
      真正的问题：4.5 rpm = 27°/秒（224 px≈30°）对着 53 px≈7° 的偏差，0.26 秒冲过靶心，
      纯 P 无阻尼 ⇒ 每一拍都在过冲，两个方向看起来都像"逃开"。
   ⚠️ 2026-10-20 第七十三次（**定案**）：用户抄回的 `[TR]` 里 `hz` 的符号**永远与 ex 同号**
      （ex=+13→hz=+27、+29→+112、−13→−27、−21→−69、+25→+91、+17→+48），
      而那全是 **SIGN=+1** 的旧镜像（Kp 0.10 逐项对得上）⇒ **+1 的镜像在正常跟踪**：
      |ex| 全程 ≤29 px、`dc` 0→1→2（误差过零就换向）、**没有再跑到底**。
      ⇒ **YAW 定为 +1**。前两轮"两个符号都逃开"的真因是**速度**（4.5 rpm = 27°/s 追 7° 偏差），
      不是符号；速度已由上面的跟踪限速兜住，所以这里只动符号。
   🔑 判据只留一条：**`hz` 与 `ex` 同号 ⇒ 该镜像的 SIGN 是 +1**。
      别再用"顶不顶机械端"判 —— 两个符号都会顶到端，上一轮就是被它带偏的。 */
#define AVE_VISION_YAW_SIGN             (+1)
#define AVE_VISION_PITCH_SIGN           (+1)


/* ============================================================================
 * 四、机械换算
 * ==========================================================================*/

/* 每度对应脉冲数。有 20.48 倍量纲陷阱：下发位置模式(0xFD)是细分脉冲(3200/圈)，
   回读 realtime_pos(0x3A)是编码器计数(65536/圈)，两者差 65536/3200=20.48 倍。
   8.889 = 3200÷360 按 16 细分折算，必须按驱动器实际细分重算并实测标定。 */
#define AVE_YAW_STEPS_PER_DEG           8.889f
#define AVE_PITCH_STEPS_PER_DEG         8.889f

/* ============================================================================
 * 六、上电点动自检
 * ==========================================================================*/


/* 自检转速（转/分）与持续时间（毫秒）。水平轴先动，间隔后再俯仰轴。 */
#define AVE_STARTUP_TEST_RPM            300
#define AVE_STARTUP_TEST_RUN_MS         300


/* ============================================================================
 * 七、云端（仅在 AVE_ENABLE_CLOUD=1 时有意义）
 * ==========================================================================*/

/* 上电时是否发送基础 AT 指令探测 ESP8266。默认关 —— 会让开机变慢。 */
#define AVE_ESP_AUTOPROBE               0


/* ============================================================================
 * 八、脉冲/方向后端（STEP/DIR）—— 电机唯一后端（ZDT 串口已作废）
 * ==========================================================================*/

/* 步进时基频率（赫兹）。由 PSC 决定，不是自由参数：TIM2/TIM5 挂 APB1=84MHz，
   生成物里 Prescaler=99 ⇒ 84MHz/(99+1)=840kHz。改它必须同时改 CubeMX 的 PSC，
   只改一处不会报错，只会让所有速度按同一比例整体偏掉。 */
#define AVE_STEPPER_TIMEBASE_HZ         840000UL

/* 步进频率上下限（赫兹）。ARR = 时基/频率 − 1。
   上限 40kHz：电机最大空载 420RPM × 3200 脉冲/圈 ≈ 22.4kHz；位置模式每脉冲一次比较
   中断，40kHz 约 3.5% CPU，比电机需求高 79%，又离中断负担临界点还远。
   下限 1Hz：TIM2/TIM5 的 ARR 是 32 位，根本撑得住低频，真正的下限是没有意义有多低。 */
#define AVE_STEPPER_MIN_HZ              1U
#define AVE_STEPPER_MAX_HZ              40000U

/* DIR 建立时间（微秒）—— 换向后、第一个脉冲前必等的静默。张大头手册没给这个数，
   取通用驱动器的 5~6μs 上浮到 10μs。给短了现象是偶发丢步，极易误诊成负载太重或
   PID 增益太大。用微秒级等待而非 osDelay：osDelay 粒度 1ms 粗 100 倍还会让出 CPU。 */
#define AVE_STEPPER_DIR_SETUP_US        10U

/* 一次位置移动允许的最大脉冲数 —— 防手滑输一个天文数字。
   2000000 ≈ 625 圈，比任何合法需求都宽，但能挡住「负数被当无符号数」那类量级错误
   （-3200 变 42.9 亿脉冲，发出去就撞止挡）。 */
#define AVE_STEPPER_MAX_MOVE_PULSES     2000000L

/* 云台层允许的每轴最大转速（转/分）。量纲与 plan.md §5.6「两轴最大转速 200 转/分」
   及 §5.5 的 PID 输出一致 —— 云台层收到的就是转/分，工程里不再有"速度档位(0~0x4FF)"
   这个中间量纲（那是 ZDT 串口时代的产物，随后端一起作废）。
   AVE_gimbal.c 据此把越界转速拒成 ERANGE，并用编译期断言保证
   「本值 × 每度脉冲数 × 6 ≤ AVE_STEPPER_MAX_HZ」，否则"合法"转速会被后端 ERANGE 拒收。
   ⚠️ 它只管【云台层】。诊断点动走 AVE_DIAG_JOG_HZ，两者互不影响、互不校验。 */
#define AVE_GIMBAL_MAX_RPM              200.0f


/* ============================================================================
 * 九、任务周期、栈与日志（plan.md §4.2.1）
 * ==========================================================================*/

/* 各任务周期（毫秒）。用 osDelayUntil 按绝对时刻唤醒，周期不随代码耗时漂移；
   osDelay 是执行完再等 N 毫秒，实际周期会累积偏慢。 */
#define AVE_TASK_VISION_PERIOD_MS       20      /**< 50 赫兹控制环 —— 本系统唯一的硬实时约束 */
#define AVE_TASK_MOTOR_PERIOD_MS        10
#define AVE_TASK_CMD_PERIOD_MS          10
/* 心跳打印周期。M1.5 验收手段：串口助手每 2 秒见一行。 */
#define AVE_TASK_HEARTBEAT_PERIOD_MS    2000

/* 各任务栈大小（字节）。CMSIS-RTOS2 的 stack_size 单位是字节不是字，
   按字算会让栈小 4 倍，症状是随机栈溢出（configCHECK_STACK_OVERFLOW=2 可抓住）。 */
#define AVE_TASK_VISION_STACK           1024
#define AVE_TASK_MOTOR_STACK            768
#define AVE_TASK_CMD_STACK              1024
#define AVE_TASK_HEARTBEAT_STACK        512


/* 诊断探针（正常运行必须为 0）。让视觉任务每轮额外阻塞这么久，
   只为验证「一个任务长时间阻塞时其余任务照常跑」—— 只能上板验。
   设成非 0 后预期：v≈t/5020 几乎冻结；m=c=t/10+1、hb=t/2000+1 仍精确成立。
   改回 0 不用靠记：正常心跳的 v=t/20+1 本身就是判据。 */

#define AVE_DIAG_BLOCK_VISION_MS        0

/* 诊断探针 · 上电点动（正常运行必须为 0）。向某个轴发一小段脉冲，用来验"脉冲链真的通"。
   `PULSES == 0` ⇒ 整个点动块在预处理期被摘掉，**产物逐字节不变** —— 这是它的验收判据，
   也是它存在的理由：门控默认关是 fail-safe **设计**（不是漏调），`AVE_Stepper_MovePulses()`
   全工程零调用者 ⇒ 镜像里**没有任何运动路径**。要点动就必须有一条路，而这条路只该在
   "这一版就是来验电机的"时候存在 —— 所以做成默认 0 的编译期开关，**不是临时改一行再删**
   （靠人记的清理迟早会漏，本工程为此栽过一次，见上面 AVE_DIAG_BLOCK_VISION_MS 的注释）。

   ⚠️ ⚠️ **它不能用来证明"电机会转"。** `Com` 是 `En`/`Stp`/`Dir` 三路光耦的公共端，
   不接 ⇒ 三路都没回路 ⇒ **定时器照样出脉冲、`[JOG]` 照样打 `rc=0`、`busy` 照样归零，
   而轴一步都不动**。这一条在 `verify.md` 第三十次的第一节，别读成"已验证"。 */
#define AVE_DIAG_JOG_PULSES             0        /* 0 = 关闭；非 0 = 脉冲数（带符号可换向）。
                                                  * 🔴 **已还原为 0**（第四十五次测完）—— 非 0 = 每次上电该轴自动走，反复上电会累积、顶到止点。
                                                  * ✅ M2.6 第二标定点（2026-10-09 第四十五次，200 脉冲 / AXIS=1）：
                                                  * 上板 `[JOG] ax=1 n=200 hz=200 rc=0,0,0 f=200 b=0 sp=34768492 mv=1`、
                                                  * `[HB] p=0,200`、驱动器 `clk` = **−200**（精确、无丢步）⇒ 云台实测 **≈22.6°**
                                                  * （顺时针）。200 ÷ 8.889 = 22.5 预测 vs 22.6 实测 ⇒ **偏差 +0.44%**
                                                  * ⇒ `AVE_PITCH_STEPS_PER_DEG = 8.889f` **第二点印证、不改**。
                                                  * 两点反算斜率 = (200−100) ÷ (22.6−11.25) = **8.81 脉冲/度**（vs 8.889，−0.9%）。
                                                  * ⚠️ 22.6 是**目测/量角器**读数（±0.3° 量级）⇒ 8.81 / 8.85 / 8.889 三者之差
                                                  * **全在误差带内**，别把 +0.44% 当成"真值偏高"的证据。
                                                  * 🔴 **该轴正方向朝左还是朝右仍未定** —— 本次只知道是**顺时针**；⚠️ 2026-10-10 更正：此地旧文写"俯仰/是上是下"，但 AXIS=1 走的是 TIM5_CH4/PA3 ⇒ 按实物它是**水平(左右)**轴，问错了轴，须重测。
                                                  * ✅ M2.5 机械层 2026-10-06 上板通过（旧称"水平 PAN"=400、AXIS=0 ⇒ 实为俯仰轴那版）：
                                                  * 顺时针**≈45°**（⚠️ 2026-10-10 更正：此版走 PB10 = **俯仰(上下)**轴，旧文"向右"不可能成立；**45° 这个数字仍有效**，方向须重测）；驱动器 OLED `clk` 读 **400**、串口
                                                  * `[HB] p=400,0` 也读 **400** ⇒ 发出 / 收到 / 机械转角**三条
                                                  * 独立链 1:1 对上、无丢步**，400 ÷ 8.889 ≈ 45.0° 闭合。
                                                  * 🔴 那次首测报「没转」的真因 = **`Stp` 线当时没接**（`plan.md
                                                  * §10.6` 同类接线故障）—— 固件无罪，`p=400,0` 早已证明发得出；⚠️ 与第三十七次**不是一回事**：
                                                  * 那次置 1 只为让 `p` 有非零读数。
                                                  * ✅ 旧称"俯仰 TILT"轴首测 2026-10-07 上板通过（=100 / AXIS=1 那版 ⇒ 实为**水平(左右)**轴）：
                                                  * `[JOG] ax=1 n=100 ... sp=17390942` + `[HB] p=0,100` + `clk` 0→−100。
                                                  * 📌 **`sp` = 「剩余自旋预算」，不是消耗量**（本轮定案）：上界 = `expect_ms × 40000`
                                                  * ⇒ 200 脉冲上界 40,000,000、打印 34,768,492 ⇒ **消耗 5,231,508 / 1000 ms = 5232 次/ms**；
                                                  * 同式代回旧数据：100 脉冲 ⇒ **5218**、400 脉冲 ⇒ **5239**，与 20 脉冲实测 5111
                                                  * 共四次落在 5111~5239（**±1.3%**）⇒ 可复现、`SPIN_PER_MS=40000` 是实测的 ~7.6 倍余量。
                                                  * 🔴 屏上第 1 行 `0.0°` 是**指令计数**（`clk ÷ 8.889`），手转云台
                                                  * 时它不动是**定义使然、不是故障**；编码器活不活看第 2 行 `err`
                                                  * （手扰 ⇒ err 动 ⇒ 回 0.0）。我一度据此判「标定得印证」=
                                                  * **循环论证**，已当场撤回；改由云台上量到的角度立住。
                                                  * 🔴 间歇故障（走一点就顶住 + 原地抖 + 堵转蓝灯，一次上电连发
                                                  * 三次）**至今未定因**，头号候选 = 当时顶在止点附近；其后多次走动
                                                  * 全正常 ⇒ 未复现 ⇒ **不建议主动复现**（大电流；竖直轴掉使能
                                                  * 会因自重下垂）。
                                                  * ⚠️ 埋雷：`clog_rpm = 8` vs 200 Hz = **3.75 RPM** ⇒ 条件①恒满足；
                                                  * 最低档 4 RPM 仍高于 3.75 ⇒ 降阈值没用，脱检测区须 ≥ 427 Hz。 */
#define AVE_DIAG_JOG_HZ                 200U     /* 首次点动用 200 赫兹：位置模式没有加减速斜坡，
                                                  * 从静止直给高频会堵转/丢步（ROADMAP 建议 ≤500） */
#define AVE_DIAG_JOG_AXIS               0        /* 0 = 水平(YAW, TIM5_CH4/PA3)、1 = 俯仰(PITCH, TIM2_CH3/PB10)（2026-10-10 由 PAN/TILT 改名，值不变）
                                                  * 🔴 **已还原为 0**（第四十五次测完；两轴首测见上）。
                                                  * ⚠️ 这个宏只**选轴** —— 真正决定"点动存不存在"的是
                                                  * AVE_DIAG_JOG_PULSES（它为 0 时整块连同调用点被 `#if`
                                                  * 摘掉，改这个宏不会有任何效果）。 */

/* 点动等待的自旋上界（空循环次数 / 每毫秒）。
   取值依据是**实测**：① 第三十次（20 步 / 200 赫兹 = 100 ms）消耗 511130 ⇒ **约 5100 次/毫秒**；
   ② 第三十八次（400 步 / 200 赫兹 = 2000 ms）消耗 `80,000,000 − 69,522,994 = 10,477,006`
   ⇒ **5239 次/毫秒** —— 两次独立测量差 **2.5%** ⇒ 该速率**可复现**。
   ⚠️ 我先前的估计是"每次循环 3~4 个周期 ⇒ 40000 次/毫秒"，**差了近 8 倍**：循环体里
   `AVE_Stepper_IsBusy()` 是跨文件函数调用（约 30 周期），不是一条自减。这里取 40000，
   是实测值的 **7.6~8 倍**余量 ⇒ 400 步（2000 ms）的等待上界约 **15 秒**（不影响正确性，
   只决定"等它走完"最多等多久）。🔴 给它小了会误报 b=1（其实是没等够），给它大了开机多停一瞬。
   与 AVE_LOG_BOOT_TXIDLE_SPIN 同理 —— 不图省事填个大的，也不要小到误判。 */
#define AVE_DIAG_JOG_SPIN_PER_MS        40000UL

/* 单条日志最大字节数（含 \r\n 和结尾 '\0'）。超长被截断，日志宁可少写也不能溢出；
   AVE_LOG_LINE_MAX 是留给正文的部分（=本值−2）。 */
#define AVE_LOG_LINE_MAX                100

/* 开机日志串行化的自旋上界（空循环次数）。AVE_Log 发送忙就丢，而开机那三条日志
   （[RST] / [TIM] / [HB] t=0）全挤在头 1.5 毫秒之内 ⇒ 后两条必丢。
   第 28 次上板实测：drtx 每次开机恰好 +2，丢的正是 [TIM] 与 [HB] t=0，
   其中 [TIM] 是专门加来看定时器配置的，结果它在板上**永远看不见**。
   同一条规矩在 [VS] 与 [HB] 之间早就用"错相"实现了（见上面的
   AVE_VISION_VS_LOG_PERIOD_MS），开机这一段只是漏了。修法：发下一条之前先等上一笔发完。
   取值依据（估，非实测）：168 兆赫、该空循环每次约 3~4 个周期 ⇒ 约 45 毫秒，
   是"最长一条日志 100 字节 ≈ 8.7 毫秒"的 5 倍以上。
   🔴 这个数**不影响正确性**：给小了 = 退回旧行为（那条日志照丢），给大了 = 开机多等几十毫秒。
   唯一会被它坑到的是"串口卡住发不出去"——那时它是防死等的闸。 */
#define AVE_LOG_BOOT_TXIDLE_SPIN        2000000UL


/* ============================================================================
 * 十、编译期自检 —— 只放「写错必然出问题且编译器能查出来」的项
 * ==========================================================================*/

/* 画面尺寸必须为正 */
#if (AVE_VISION_SCREEN_W <= 0) || (AVE_VISION_SCREEN_H <= 0)
  #error "AVE_VISION_SCREEN_W 与 AVE_VISION_SCREEN_H 必须为正数"
#endif

/* 中心点按整数除法派生，屏幕尺寸为奇数会丢掉半个像素 */
#if ((AVE_VISION_SCREEN_W % 2) != 0) || ((AVE_VISION_SCREEN_H % 2) != 0)
  #error "AVE_VISION_SCREEN_W 与 AVE_VISION_SCREEN_H 建议为偶数"
#endif

/* 滤波系数范围 0~100 */
#if (AVE_VISION_FILTER_ALPHA < 0) || (AVE_VISION_FILTER_ALPHA > 100)
  #error "AVE_VISION_FILTER_ALPHA 必须在 0~100 之间"
#endif

/* 视觉闭环开关只能是 0/1。写成 -1 之类时，`#if AVE_ENABLE_VISION_TRACK` 会
   把 -1 当真值、编译通过，但调用方的直觉（"负数应该是关"）与行为不一致。 */
#if (AVE_ENABLE_VISION_TRACK != 0) && (AVE_ENABLE_VISION_TRACK != 1)
  #error "AVE_ENABLE_VISION_TRACK 只能是 0 或 1"
#endif

/* 跟踪依赖云台层接线：AVE_ENABLE_GIMBAL == 0 时 AVE_App_Init 不调 AVE_Gimbal_Init，
   而跟踪仍会去开两轴门控 ⇒ 状态没初始化过。这类组合必须在编译期挡住。 */
#if (AVE_ENABLE_VISION_TRACK == 1) && (AVE_ENABLE_GIMBAL == 0)
  #error "AVE_ENABLE_VISION_TRACK=1 需要 AVE_ENABLE_GIMBAL=1"
#endif

/* 跟踪限速：必须为正，且不超过云台层硬上限。
   写 0 的现象不是"不转"，而是每一拍都被 ERANGE 拒（rpm != 0 但换算后 hz == 0）
   ⇒ 云台纹丝不动、闭环看起来像没接上。 */
#if (AVE_VISION_TRACK_MAX_RPM < 1) || (AVE_VISION_TRACK_MAX_RPM > AVE_VISION_MAX_RPM_X)
  #error "AVE_VISION_TRACK_MAX_RPM 应在 [1, AVE_VISION_MAX_RPM_X]"
#endif

/* 轴向符号只能是 ±1 —— 写成别的值会让输出被意外放大 */
#if ((AVE_VISION_YAW_SIGN != 1) && (AVE_VISION_YAW_SIGN != -1))
  #error "AVE_VISION_YAW_SIGN 只能是 +1 或 -1"
#endif
#if ((AVE_VISION_PITCH_SIGN != 1) && (AVE_VISION_PITCH_SIGN != -1))
  #error "AVE_VISION_PITCH_SIGN 只能是 +1 或 -1"
#endif

/* 环形缓冲大小必须为 2 的幂。写成 1000 这种「更整」的数能编过、能上电，
   只在绕回时才算出越界下标 —— 现象是随机丢字节或踩内存，看不出和大小有关。
   判据：2 的幂 & (自身−1) == 0（8&7==0，1000&999 != 0）。 */
#if (AVE_UART_RX_BUF_SIZE < 2) || ((AVE_UART_RX_BUF_SIZE & (AVE_UART_RX_BUF_SIZE - 1)) != 0)
  #error "AVE_UART_RX_BUF_SIZE 必须是 2 的幂且不小于 2（环形缓冲用掩码取模）"
#endif

/* 超时必须为正，否则 HAL 会立刻超时返回失败 */
#if (AVE_UART_TX_TIMEOUT_MS <= 0)
  #error "AVE_UART_TX_TIMEOUT_MS 必须为正数"
#endif

/* 发送缓冲至少要能装下一条日志的主体，否则日志会被截断（见 AVE_LOG_LINE_MAX） */
#if (AVE_UART_TX_BUF_SIZE < AVE_LOG_LINE_MAX)
  #error "AVE_UART_TX_BUF_SIZE 不能小于 AVE_LOG_LINE_MAX，否则日志会被截断"
#endif

/* 任务周期必须为正。写成 0 不是跑得快，而是 osDelayUntil 里 tick+=0 永远等于当前，
   任务再不让出 CPU，低优先级任务全饿死（心跳不打印，像板子卡死）。 */
#if (AVE_TASK_VISION_PERIOD_MS    <= 0) || (AVE_TASK_MOTOR_PERIOD_MS <= 0) || \
    (AVE_TASK_CMD_PERIOD_MS       <= 0) || (AVE_TASK_HEARTBEAT_PERIOD_MS <= 0)
  #error "任务周期必须为正数（0 会让 osDelayUntil 永远立即返回，任务独占处理器）"
#endif

/* 诊断探针范围 [0,30000]。下限 1 毫无意义（比周期还短看不到现象）；上限 30 秒
   让「误填大数」在编译期暴露，而不是上板后看着心跳发呆。 */
#if (AVE_DIAG_BLOCK_VISION_MS < 0) || (AVE_DIAG_BLOCK_VISION_MS > 30000)
  #error "AVE_DIAG_BLOCK_VISION_MS 应在 [0, 30000]：0 = 关闭，其余为阻塞毫秒数"
#endif

/* 点动探针。三条都只在**开启时**才查（PULSES == 0 是常态，不该被它拦）。
   ⚠️ 这里**不设**"频率 ≤500"那道闸：500 是**首次**运动的经验值，不是正确性边界，
   设成 #error 会让以后想跑快一点的人被迫同时改两处。频率的合法范围由
   AVE_Stepper_MovePulses() 自己判（越界返回 ERANGE 并计 range_rejects）。 */
#if (AVE_DIAG_JOG_PULSES > AVE_STEPPER_MAX_MOVE_PULSES) || (AVE_DIAG_JOG_PULSES < -AVE_STEPPER_MAX_MOVE_PULSES)
  #error "AVE_DIAG_JOG_PULSES 超出模块单次上限 AVE_STEPPER_MAX_MOVE_PULSES（会被 MovePulses 拒收，白烧一版）"
#endif
#if (AVE_DIAG_JOG_AXIS != 0) && (AVE_DIAG_JOG_AXIS != 1)
  #error "AVE_DIAG_JOG_AXIS 只能是 0（水平）或 1（俯仰）"
#endif
#if (AVE_DIAG_JOG_PULSES != 0) && (AVE_DIAG_JOG_HZ < AVE_STEPPER_MIN_HZ)
  #error "AVE_DIAG_JOG_HZ 低于模块下限，MovePulses 会返回 ERANGE"
#endif

/* 任务栈必须为 8 的倍数。Cortex-M 异常入口要求栈指针 8 字节对齐（AAPCS），
   栈大小不是 8 的倍数时 FreeRTOS 分出的栈顶错位 —— 现象是随机硬件错误。 */
#if ((AVE_TASK_VISION_STACK    % 8) != 0) || ((AVE_TASK_MOTOR_STACK % 8) != 0) || \
    ((AVE_TASK_CMD_STACK       % 8) != 0) || ((AVE_TASK_HEARTBEAT_STACK % 8) != 0)
  #error "任务栈大小必须是 8 的倍数（Cortex-M 栈对齐要求）"
#endif

/* 栈不能太小。512 字节是「只做几层调用、不做库调用」的下限，低于它一旦调用
   vsnprintf 之类库函数就溢出。 */
#if (AVE_TASK_VISION_STACK < 512) || (AVE_TASK_MOTOR_STACK < 512) || \
    (AVE_TASK_CMD_STACK    < 512) || (AVE_TASK_HEARTBEAT_STACK < 512)
  #error "任务栈不能小于 512 字节"
#endif

/* 日志行要留得下 "\r\n" 加结尾的 '\0' */
#if (AVE_LOG_LINE_MAX < 8)
  #error "AVE_LOG_LINE_MAX 太小（至少 8：正文 + CRLF + 结尾零）"
#endif

/* 开机日志等待上界只查上限。0 是**合法**值（= 关掉等待，退回"开机日志只出第一条"的旧行为）。
   🔴 特意**不写**「不能为负」那一条 —— 本宏是无符号数，那个判断恒为假。
   本工程已经吃过一次同义反复断言的亏（判据必须能失败），所以这里不摆样子。 */
#if (AVE_LOG_BOOT_TXIDLE_SPIN > 500000000UL)
  #error "AVE_LOG_BOOT_TXIDLE_SPIN 过大（>5e8 次 ≈ 十几秒）：开机将被肉眼可见地卡住"
#endif

/* 视觉行缓冲上下界 [8,64]。下界 8：ReadLine 在 cap<2 时直接 EBADCH，最短合法行
   「0,0,1」5 字符加结尾零。上界 64：缓冲开在视觉任务栈上，还要加 AVE_LOG_LINE_MAX
   字节，栈只有 1024，相乘不得超过它。 */
#if (AVE_VISION_LINE_MAX < 8) || (AVE_VISION_LINE_MAX > 64)
  #error "AVE_VISION_LINE_MAX 应在 [8, 64]：下限受 ReadLine 的 cap<2 拒收限制，上限受视觉任务栈限制"
#endif

/* 视觉行缓冲 + 日志行缓冲必须同时活在视觉任务的栈上（同一任务，一起算），
   再留 256 字节给调用链与 vsnprintf 内部开销。 */
#if ((AVE_VISION_LINE_MAX + AVE_LOG_LINE_MAX + 256) > AVE_TASK_VISION_STACK)
  #error "视觉任务栈放不下「视觉行缓冲 + 日志行缓冲 + 余量」—— 调大 AVE_TASK_VISION_STACK 或调小缓冲"
#endif

/* 单次排空行数上限：至少 1（否则一行都不处理），至多 128（再多该改周期而不加这个数） */
#if (AVE_VISION_MAX_LINES_PER_POLL < 1) || (AVE_VISION_MAX_LINES_PER_POLL > 128)
  #error "AVE_VISION_MAX_LINES_PER_POLL 应在 [1, 128]"
#endif

/* [VS] 周期必须是心跳周期的奇数倍。写成偶数是「恰好错在反方向」：
   心跳 H、[VS] V=nH 落在后半段（偏移 V/2），序列为 (k+1/2)·nH；
   须 n/2 永远不是 H 整数倍 ⇒ n 必须为奇数才永久错开，n=1 最简单。
   不相交很要紧：AVE_Log 发送忙就丢，同时刻各发一条则后发被静默丢弃。 */
#if (AVE_TASK_HEARTBEAT_PERIOD_MS <= 0) || \
    ((AVE_VISION_VS_LOG_PERIOD_MS % AVE_TASK_HEARTBEAT_PERIOD_MS) != 0)
  #error "AVE_VISION_VS_LOG_PERIOD_MS 必须是 AVE_TASK_HEARTBEAT_PERIOD_MS 的整数倍（否则两者相位会漂移，日志被静默丢弃）"
#endif
#if (((AVE_VISION_VS_LOG_PERIOD_MS / AVE_TASK_HEARTBEAT_PERIOD_MS) % 2) != 1)
  #error "AVE_VISION_VS_LOG_PERIOD_MS 必须是心跳周期的**奇数**倍（偶数倍会让两者每次都在同一瞬间发送，[VS] 行被静默丢弃）"
#endif

/* 错相还要求心跳不比视觉任务快：[VS] 判据是周期内后半段，半周期要宽到至少
   容下几个视觉周期，否则窗口可能被整段跳过。 */
#if (AVE_TASK_HEARTBEAT_PERIOD_MS < AVE_TASK_VISION_PERIOD_MS)
  #error "心跳周期不能小于视觉任务周期，否则两个任务的错相无法维持"
#endif
#if ((AVE_VISION_VS_LOG_PERIOD_MS / 2) < (2 * AVE_TASK_VISION_PERIOD_MS))
  #error "[VS] 的错相窗口（半个周期）太窄，至少要有 2 个视觉周期宽，否则可能被整段跳过"
#endif

/* 步进频率上下限。写错一个数所有速度整体偏掉，现象「云台跟不上目标」像 PID 没调好，
   没人会去怀疑频率换算。 */
#if (AVE_STEPPER_MIN_HZ < 1U) || (AVE_STEPPER_MAX_HZ <= AVE_STEPPER_MIN_HZ)
  #error "AVE_STEPPER_MIN_HZ / AVE_STEPPER_MAX_HZ 非法（下限至少 1，上限必须大于下限）"
#endif

/* 上限过高会让 ARR 小到无法维持约 50% 占空比（CCR=ARR/2 算成 0，输出恒为有效电平），
   且位置模式中断频率失控。取「时基/4」为界：ARR≥3、CCR≥1。 */
#if (AVE_STEPPER_MAX_HZ > (AVE_STEPPER_TIMEBASE_HZ / 4U))
  #error "AVE_STEPPER_MAX_HZ 过高：ARR 会小于 3，一半占空比无法维持，且中断负担失控"
#endif

/* 换向等待必须是正数 —— 写成 0 就等于不等，而现象是偶发丢步（最难归因的一种） */
#if (AVE_STEPPER_DIR_SETUP_US < 1U)
  #error "AVE_STEPPER_DIR_SETUP_US 必须为正：0 表示换向不等建立时间，现象是偶发丢步"
#endif

/* ——— [TR] 行发送周期（毫秒）。**故意放在文件末尾**：插在 112 行附近会把它下面
   四百多行的行号整体顶掉，而 `plan.md` / `verify.md` / `temporary.md` 有多处按行号
   引用本文件（如 `:143` 限速、`:172-173` 符号）。放末尾 ⇒ 已有行号一个都不动。

   为什么单独开一个宏：[TR] 原先和 [VS] 共用 AVE_VISION_VS_LOG_PERIOD_MS，而那个宏
   被下面 484~490 行的断言锁成「心跳周期的**奇数倍**」⇒ 最小只能取 2000 ms。
   代价是实测出来的（第七十四次第五批）：闭环约 1 s 就把误差收进死区，2 s 采样的
   日志**永远只拍到"收敛之后"**，瞬态（过冲/振荡）一次也抓不到 —— 你只会看到
   "误差已经很小了"，看不出它是不是冲过头又荡回来的。

   ⚠️ 改小之后：日志带宽 ≈ 67 B × (1000/period)，200 ms 时约 335 B/s（115200 下 3%）。
      AVE_Log 是"忙就丢"，所以会**丢掉一部分抽样** —— 丢的是观测，不是控制。
      现象：`drtx` 涨得比现在快，`[TR]` 行不连续。这是预期的，别当成故障。 */
#define AVE_VISION_TR_LOG_PERIOD_MS     200

/* 下限 100 ms：再短下去带宽与丢弃率都不可接受，而且视觉任务 50 Hz 也撑不起更密的抽样 */
#if (AVE_VISION_TR_LOG_PERIOD_MS < 100)
  #error "AVE_VISION_TR_LOG_PERIOD_MS 不能小于 100 ms（视觉任务 50 Hz + AVE_Log 忙就丢）"
#endif
/* 比 [VS] 还慢就没有意义：[TR] 是比 [VS] 更该先看的那一行 */
#if (AVE_VISION_TR_LOG_PERIOD_MS > AVE_VISION_VS_LOG_PERIOD_MS)
  #error "AVE_VISION_TR_LOG_PERIOD_MS 不能大于 [VS] 周期：比 [VS] 还慢的 [TR] 没有观测价值"
#endif
/* 必须是视觉任务周期的整数倍，否则抽样点随 tick 漂移，"同样周期"在不同拍上实测间隔不同 */
#if ((AVE_VISION_TR_LOG_PERIOD_MS % AVE_TASK_VISION_PERIOD_MS) != 0)
  #error "AVE_VISION_TR_LOG_PERIOD_MS 必须是 AVE_TASK_VISION_PERIOD_MS 的整数倍（否则采样点漂移）"
#endif

#endif /* AVE_TUNING_H */
