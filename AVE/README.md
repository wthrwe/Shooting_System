# AVE — STM32F407 二维云台视觉跟踪打靶系统

MaixCAM 运行 YOLOv5 找到靶心，通过串口把 `cx,cy,flag` 发给 STM32F407；
F407 在 FreeRTOS 下解析坐标、计算像素偏差、做比例控制，再把转速换算成
STEP/DIR 脉冲，驱动两台闭环步进电机带动二维云台，让激光指向靶心。

> 本文按**当前代码**整理，参数以 [`AVE/AVE_tuning.h`](AVE/AVE_tuning.h) 和
> [`AVE/AVE_board.h`](AVE/AVE_board.h) 为准。

---

## 1. 关键事实

| 项目 | 内容 |
| --- | --- |
| MCU | STM32F407VGTx（Cortex-M4F，168 MHz，HSI 16 MHz 经 PLL：M=8 / N=168 / P=2） |
| RTOS | FreeRTOS + CMSIS-RTOS v2（`osThreadNew` / `osMutexNew` / `osDelayUntil`） |
| 工具链 | Keil MDK，ARMCC V5.06（AC5），C99；工程文件 [`MDK-ARM/AVE.uvprojx`](MDK-ARM/AVE.uvprojx) |
| CubeMX | STM32Cube FW_F4 V1.28.3，配置 [`AVE.ioc`](AVE.ioc) |
| 视觉端 | MaixCAM，YOLOv5，输入/坐标空间 224×224 |
| 执行器 | ZDT X42S 第二代闭环步进 ×2，STEP/DIR 模式（ZDT 串口协议模块已移除） |
| 控制周期 | 20 ms（50 Hz），本系统唯一硬实时约束 |
| 串口 | USART1 视觉 115200-8-N-1；USART3 调试 115200-8-N-1 |
| FreeRTOS 堆 | `configTOTAL_HEAP_SIZE = 32768` B，栈溢出检查等级 2 |
| 激光 | PC13，高电平点亮（`AVE_LASER_ACTIVE_HIGH = 1`） |

---

## 2. 数据流与分层

```text
  MaixCAM (YOLOv5)
      │  "cx,cy,flag\n"  115200 8N1
      ▼
  USART1 RX (PB7) ──中断逐字节──► 环形缓冲(1024B) ──► AVE_Vision_Poll()
                                                        │ 50 Hz
                                                        ▼
                                                 AVE_vision.c
                                        解析 / 只保留最新帧 / 断流判定
                                                        │ 像素误差 ex,ey
                                                        ▼
                                              vision_track_step()  纯 P
                                  死区(8px) → Kp(0.10) → 限幅(±2 rpm)
                                                        │ 带符号转速
                                                        ▼
                                               AVE_gimbal.c
                                    角度/脉冲换算、软件门控、速度/定位接口
                                                        │ 频率 Hz + 方向
                                                        ▼
                                               AVE_stepper.c
                                  TIM5_CH4/PA3(YAW)、TIM2_CH3/PB10(PITCH)
                                     PWM 波形 + DIR(PA5/PA4) + 脉冲计数 ISR
                                                        ▼
                                              X42S 闭环步进 (YAW / PITCH)

  目标丢失或断流 ──► 两轴转速写 0 + 激光 PC13 关断并闩锁
```

软件分四层（从下到上）：

| 层 | 文件 | 职责 |
| --- | --- | --- |
| 板级/外设 | `AVE_board.c` / `AVE_uart.c` / `AVE_log.c` | 激光安全电平、启动故障码、复位原因、串口环形缓冲、非阻塞日志 |
| 驱动 | `AVE_stepper.c` | 定时器 PWM 波形、DIR 建立时间、比较中断数脉冲、寄存器自检 |
| 应用 | `AVE_gimbal.c` | 角度↔脉冲、转速↔频率、速度/定位模式、软件门控与仲裁 |
| 业务/装配 | `AVE_vision.c` / `AVE_app.c` | 协议解析、最新帧、断流、闭环一拍、任务与初始化 |

---

## 3. 硬件接线

| 功能 | 外设 / 通道 | 引脚 | 说明 |
| --- | --- | --- | --- |
| 视觉接收 | USART1_RX / AF7 | **PB7** | MaixCAM 的 TX 接这里 |
| 视觉发送 | USART1_TX / AF7 | PB6 | 本工程不用，可悬空 |
| 调试发送 | USART3_TX / AF7 | **PD8** | CH340 的 RX 接这里（PB10 被 TIM2_CH3 占用，故重映射到 PD8） |
| 调试接收 | USART3_RX / AF7 | PB11 | CH340 的 TX 接这里 |
| 水平轴 STEP（左右，YAW） | TIM5_CH4 | **PA3** | 8.889 脉冲/度，3200 脉冲/圈 |
| 水平轴 DIR | GPIO | **PA5** | 与 PA3 同一台电机 |
| 俯仰轴 STEP（上下，PITCH） | TIM2_CH3 | **PB10** | 与 PA4 同一台电机 |
| 俯仰轴 DIR | GPIO | **PA4** | |
| 激光 | GPIO | **PC13** | 高电平点亮 |

> ⚠️ 引脚宏写在 [`AVE_board.h`](AVE/AVE_board.h)，但**真正生效的是 CubeMX 生成的
> `usart.c` / `gpio.c` / `tim.c`**。改引脚必须同时改宏、`AVE.ioc` 和生成代码，
> 只改宏不会有任何报错。
>
> 🔴 2026-10-10 按实物实测确认：**PB10 那台是俯仰（上下），PA3 那台是水平（左右）**。
> 旧文档曾写反。

---

## 4. 目录结构

```text
AVE/
├─ AVE.ioc                  CubeMX 配置（时钟、引脚、外设、FreeRTOS）
├─ Core/                    CubeMX 生成代码
│  ├─ Src/main.c            时钟、外设初始化、调度器启动
│  ├─ Src/freertos.c        默认任务、栈溢出钩子、调用 AVE_App_RtosInit()
│  ├─ Src/usart.c           USART1/USART3 引脚与参数
│  ├─ Src/tim.c             TIM2_CH3、TIM5_CH4 PWM 通道
│  ├─ Src/gpio.c            PC13 激光等
│  ├─ Src/stm32f4xx_it.c    TIM2/TIM5/USART1/USART3/TIM7 中断入口
│  └─ Src/stm32f4xx_hal_timebase_tim.c   TIM7 作为 HAL 时基
├─ Drivers/                 STM32F4 HAL 驱动 + CMSIS（不需要业务阅读）
├─ Middlewares/             FreeRTOS 内核 + CMSIS-RTOS v2
├─ AVE/                     业务代码（本项目重点）
├─ MDK-ARM/                 Keil 工程、构建产物、构建/烧录日志
├─ maixpy/                  MaixCAM 视觉脚本（YOLOv5 → 串口）
├─ _maixcam/                MaixCAM 时序一致性闸门脚本
├─ _selftest_ave/           主机端自测：C 单元测试、Python 契约测试、变异测试
├─ _v38_selftest/           脉冲后端专项自测（ISR 语义、变异）
├─ plan.md                  系统设计规格与任务计划
├─ ROADMAP.md               里程碑与状态变更
├─ verify.md                实测证据与失败留痕（唯一一份）
├─ temporary.md             跨对话交接
├─ troubleshooting.md       故障排查经验
├─ CLAUDE.md                工程协作规则（读取范围、交付格式、文档上限）
├─ D36A驱动用户手册_V1.5_2026.6.17.pdf
└─ ZDT_X42S第二代闭环步进电机用户手册V1.0.5_260527.pdf
```

---

## 5. 模块职责

| 文件 | 负责 | 不负责 |
| --- | --- | --- |
| [`AVE_app.c`](AVE/AVE_app.c) | 初始化顺序、四个任务、互斥量、`[HB]/[VS]/[TR]` 日志 | 串口/视觉/电机的具体实现 |
| [`AVE_board.c`](AVE/AVE_board.c) | 激光开关与启动故障闩锁、复位原因 | 外设初始化（在 CubeMX 生成代码里） |
| [`AVE_uart.c`](AVE/AVE_uart.c) | 两路串口（视觉 / 调试）逐字节中断接收、环形缓冲、按行切帧、IT 发送 | 协议解析 |
| [`AVE_ringbuf.h`](AVE/AVE_ringbuf.h) | 无锁单生产者/单消费者环形缓冲 | 线程调度 |
| [`AVE_log.c`](AVE/AVE_log.c) | 定长栈缓冲 + `vsnprintf`，忙就丢，不阻塞控制 | 日志存储/审计 |
| [`AVE_vision.c`](AVE/AVE_vision.c) | `cx,cy,flag` 解析、最新帧发布、断流判定、`[VS]` 行 | 电机控制 |
| [`AVE_gimbal.c`](AVE/AVE_gimbal.c) | 角度↔脉冲、rpm↔Hz、速度/相对/绝对移动、软件门控 | 定时器波形、DIR 时序 |
| [`AVE_stepper.c`](AVE/AVE_stepper.c) | PWM 频率与脉冲数、DIR 建立时间、ISR 数脉冲、寄存器自检 | 角度换算、PID |
| [`AVE_tuning.h`](AVE/AVE_tuning.h) | 唯一调参入口：功能开关、PID、限速、地址、机械换算、时序 | 逻辑实现 |

---

## 6. RTOS 任务

| 任务 | 优先级 | 周期 | 栈 | 当前工作 |
| --- | --- | --- | --- | --- |
| `VisionTask` | High | 20 ms | 1024 B | 排空视觉串口、解析、只留最新帧、断流、纯 P 闭环、激光 fail-safe、`[VS]/[TR]` |
| `MotorTask` | AboveNormal | 10 ms | 768 B | **占位**：只累加计数（闭环故意放在 VisionTask，避免跨任务读帧撕裂） |
| `CmdTask` | Normal | 10 ms | 1024 B | **占位**：预留上位机/云端命令解析 |
| `HeartbeatTask` | Low | 2000 ms | 512 B | 每 2 秒输出 `[HB]` 心跳 |
| `defaultTask` | Normal | 1 tick | 512 B | CubeMX 默认任务，空转 `osDelay(1)` |

- 所有周期任务都用**绝对时刻累加**（`next += PERIOD; osDelayUntil(next)`），避免周期漂移。
- 电机互斥量 `MotorMutex`（优先级继承）仍创建并检查，但**当前没有消费者**：
  脉冲后端两轴各占一个定时器，不存在共享总线。
- 任务创建失败会记入 `AVE_App_FailName`，再由 `Error_Handler()` 上报；
  栈溢出由 `vApplicationStackOverflowHook` 记标志，心跳行报 `ovf=`。

---

## 7. 视觉串口协议

MaixCAM → STM32，**ASCII 文本，一行一帧，只发 `\n`**：

```text
cx,cy,flag\n
```

| 字段 | 含义 | 约束 |
| --- | --- | --- |
| `cx` | 靶心 x（像素，向右为正） | `flag=1` 时必须 `0..223` |
| `cy` | 靶心 y（像素，向下为正） | `flag=1` 时必须 `0..223` |
| `flag` | 有无目标 | **只能是 0 或 1**；无目标发 `-1,-1,0` |

协议硬约束：

| 规则 | 值 |
| --- | --- |
| 画面尺寸 | 224 × 224（`AVE_VISION_SCREEN_W/H`，必须与相机一致） |
| 画面中心 | (112, 112) |
| 单行上限 | 32 字节（`AVE_VISION_LINE_MAX`） |
| 行尾 | 只允许 `\n`，不要 `\r\n` |
| 数字上限 | 9 位（防止有符号累加溢出） |
| 断流判定 | 超过 200 ms 无合法帧即判断流 |
| 每轮最多处理 | 16 行（防任务饿死，超限记 `backlog`） |

解析结果码（[`AVE_vision.h`](AVE/AVE_vision.h)）：

| 结果 | 值 | 处理 |
| --- | --- | --- |
| `PARSE_BLANK` | -3 | 空行，忽略，不计错误 |
| `PARSE_RANGE` | -2 | `flag=1` 但坐标越界，整帧丢弃 |
| `PARSE_FORMAT` | -1 | 语法错，整行丢弃 |
| `PARSE_NO_TARGET` | 1 | 合法无目标帧 |
| `PARSE_OK` | 0 | 合法目标帧 |

`flag=0` 却带合法坐标属于"越规但无害"，计入 `flag0_coords` 后按无目标帧处理。

---

## 8. 控制链与调参

| 参数 | 当前值 | 位置 |
| --- | --- | --- |
| 控制周期 | 20 ms（50 Hz） | `AVE_TASK_VISION_PERIOD_MS` |
| 画面中心 | 112, 112 | `AVE_VISION_CENTER_X/Y` |
| 死区 | 8 px，采用"减掉死区"保证输出连续 | `AVE_VISION_TOL_PX` |
| YAW 增益 | Kp 0.10 / Ki 0.00 / Kd 0.01 | `AVE_VISION_YAW_PID_*` |
| PITCH 增益 | Kp 0.10 / Ki 0.00 / Kd 0.01 | `AVE_VISION_PITCH_PID_*` |
| 跟踪限速 | ±2 rpm | `AVE_VISION_TRACK_MAX_RPM` |
| 方向符号 | YAW +1 / PITCH +1 | `AVE_VISION_YAW/PITCH_SIGN` |
| 每度脉冲 | 8.889（3200 脉冲/圈） | `AVE_YAW/PITCH_STEPS_PER_DEG` |
| 云台最大转速 | 200 rpm | `AVE_GIMBAL_MAX_RPM` |
| 脉冲频率范围 | 1 ~ 40000 Hz | `AVE_STEPPER_MIN/MAX_HZ` |
| 单次定位上限 | 2,000,000 脉冲 | `AVE_STEPPER_MAX_MOVE_PULSES` |
| DIR 建立时间 | 10 µs | `AVE_STEPPER_DIR_SETUP_US` |
| 定时器时基 | 840 kHz（PSC=99，APB1 定时器时钟 84 MHz） | `AVE_STEPPER_TIMEBASE_HZ` |

闭环当前是**纯比例控制**：Ki/Kd 已有宏但未接入，先用 P 把方向验对。

> ⚠️ 两个方向符号与 DIR 极性"哪个电平算正"仍需实物确认。符号反了的现象是
> **云台越追越远、目标几帧内出画**。首次试车建议先看 `[TR]` 行的 `hz` 与 `ex` 符号。

---

## 9. 调试日志（USART3）

所有日志非阻塞：发送忙就整条丢弃并计数，**日志给控制让路**。

| 行 | 频率 | 主要字段 | 用途 |
| --- | --- | --- | --- |
| `[RST]` | 开机一次 | `cause=` 复位原因位掩码 | 判断上电/欠压/看门狗/软件复位 |
| `[TIM]` | 开机一次 | `tb/clk/exppsc/A=psc,ccmr2,ccer/B=…/bad=` | 定时器寄存器回读自检，`bad=0,0` 才正常 |
| `[HB]` | 2 s | `t v m c hb rx drrx drtx ovf bf p=A,B` | 任务计数、接收/丢弃字节、栈溢出、启动故障、两轴累计脉冲 |
| `[VS]` | 2 s（奇数秒半区） | `st se ok dr sp f=` | 断流、断流跳变、合法帧数、协议错、超长丢弃、最新帧 |
| `[TR]` | 约 5 行 / 2 s | `nv ex ey hz rj rc dc age` | 闭环这一拍的输入/输出/实际频率/拒收/帧年龄 |
| `[JOG]` | 仅诊断 | `ax n hz rc f b sp mv` | 上电点动自检（`AVE_DIAG_JOG_PULSES != 0` 时） |
| `[PIN]` | 仅诊断 | `low=[] edg=[]` | 引脚探针（`AVE_SELFTEST_PIN_PROBE = 1` 时，会占用 USART1，测完必须改回 0） |

排查顺序建议：`[RST]` → `[TIM] bad` → `[HB] ovf/bf` → `[VS] st/ok` → `[TR] age/hz`。

---

## 10. 构建、烧录与运行

### 10.1 Keil 构建

1. 用 Keil MDK 打开 [`MDK-ARM/AVE.uvprojx`](MDK-ARM/AVE.uvprojx)，目标名 `AVE`。
2. 编译器选择 **ARMCC V5.06（AC5）**，C99，优化等级 3。
3. `Rebuild`（F7）。产物：`MDK-ARM/AVE/AVE.axf`、`AVE.hex`。
4. 构建成功的标准是 **0 Error / 0 Warning**；产物为 `AVE.axf`、`AVE.hex`、`AVE.map`。
   清理后实测：`Code=27188  RO-data=1624  RW-data=236  ZI-data=40644`。

### 10.2 烧录与串口

1. 通过 ST-Link / J-Link 下载 `AVE.hex`。
2. 调试串口接 **USART3：PD8(TX) / PB11(RX)**，115200-8-N-1。
3. 上电后应依次看到 `[RST]`、`[TIM]`，之后每 2 秒 `[HB]`。

### 10.3 视觉端

1. 把 [`maixpy/v5_send.py`](maixpy/v5_send.py) 或
   [`_maixcam/maixcam_track_gate.py`](_maixcam/maixcam_track_gate.py) 部署到 MaixCAM。
2. 相机串口 `/dev/ttyS0`，115200；模型路径按脚本里的 `MODEL` 修改。
3. 必须保证相机坐标空间与固件 `AVE_VISION_SCREEN_W/H` 都是 **224×224**。
4. 行格式固定 `"%d,%d,%d\n"`，`flag` 只能 0/1，行尾只发 `\n`。

### 10.4 修改 CubeMX 后

`AVE.ioc` 重新生成代码时，业务代码都在 `/* USER CODE BEGIN/END */` 块内。
⚠️ `main.c` 里的保护标记必须成对且唯一，注释中不要出现标记原文。
引脚/通道改动要同时核对 [`AVE_board.h`](AVE/AVE_board.h) 的宏和
`AVE_stepper.c` 里 `s_cfg[]` 的通道寄存器字面量（有编译期断言兜底）。

---

## 11. 功能开关（`AVE_tuning.h`）

| 宏 | 当前值 | 作用 | 是否生效 |
| --- | --- | --- | --- |
| `AVE_ENABLE_VISION` | 1 | 视觉自动跟踪总开关（预留） | 业务代码当前未引用 |
| `AVE_ENABLE_VISION_TRACK` | **1** | 视觉误差驱动两轴转速；同时决定开机是否开两轴门控 | ✅ 生效 |
| `AVE_ENABLE_GIMBAL` | 1 | 云台层接线开关（只挡 `AVE_Gimbal_Init()`） | ✅ 生效 |
| `AVE_ENABLE_CLOUD` | 0 | ESP8266 + MQTT 云端（`AVE_secrets.h` 仅此时需要） | 未接入 |
| `AVE_ENABLE_MOTOR_READBACK` | 0 | 电机位置回读（会让总线变吵） | 未接入 |
| `AVE_ENABLE_STARTUP_MOTOR_TEST` | 1 | 上电两轴点动自检（预留） | 业务代码当前未引用 |

另有诊断开关 `AVE_DIAG_JOG_PULSES`（当前 0 = 关闭）、`AVE_DIAG_BLOCK_VISION_MS`
（当前 0）、`AVE_SELFTEST_PIN_PROBE`（当前 0）。

> 🔴 上电默认**两轴门控关闭**（`AVE_Stepper_Init()` 的 fail-safe），
> 所以正常上电时 `[HB]` 的 `p=0,0` 是设计行为，不是故障。`AVE_ENABLE_VISION_TRACK=1`
> 会在开机末尾统一开两轴门控。

---

## 12. 测试脚手架

| 资产 | 类型 | 内容 |
| --- | --- | --- |
| [`_selftest_ave/host/test_gimbal.c`](_selftest_ave/host/test_gimbal.c) | C 主机测试 | 角度/脉冲换算、门控、仲裁（带 fake HAL） |
| [`_selftest_ave/host/test_stepper.c`](_selftest_ave/host/test_stepper.c) | C 主机测试 | 频率/脉冲、ISR、自检位、边界 |
| [`_selftest_ave/test_contract_vision.py`](_selftest_ave/test_contract_vision.py) | Python 契约测试 | 视觉协议与固件解析一致性 |
| [`_selftest_ave/test_gate_host.py`](_selftest_ave/test_gate_host.py) | Python | 把 MaixCAM 闸门段原样抽出来跑 |
| [`_selftest_ave/mutate_selftest.sh`](_selftest_ave/mutate_selftest.sh) | 变异测试 | 验证测试本身能抓到错误 |
| [`_v38_selftest/test_pt.c`](_v38_selftest/test_pt.c) | C 主机测试 | 脉冲 ISR "一边沿一次调用"语义 |
| [`_v38_selftest/mutate.py`](_v38_selftest/mutate.py) | 变异测试 | 脉冲后端专项 |

C 测试用 `gcc -std=c99 -Wall -Wextra` 编译，零 HAL / 零 FreeRTOS 依赖（fake 头在
`_selftest_ave/host/fake`）。具体命令见各测试文件头注释。

---

## 13. 文档索引

| 文档 | 内容 |
| --- | --- |
| [`plan.md`](plan.md) | 系统设计规格、模块接口、验收标准 |
| [`ROADMAP.md`](ROADMAP.md) | 里程碑状态、发现的坑、待办 |
| [`verify.md`](verify.md) | 每次上板/自测的实测证据与失败记录（唯一一份） |
| [`temporary.md`](temporary.md) | 跨对话交接：进度、下一步、待裁决项 |
| [`troubleshooting.md`](troubleshooting.md) | 典型故障现象与排查路径 |
| [`CLAUDE.md`](CLAUDE.md) | 协作规则：读取范围、改动交付五节、文档 ≤1000 行 |
| `D36A驱动用户手册_V1.5_2026.6.17.pdf` | 驱动器手册 |
| `ZDT_X42S第二代闭环步进电机用户手册V1.0.5_260527.pdf` | 步进电机手册 |

辅助脚本：[`check_pins.sh`](check_pins.sh)（引脚核对）、
[`_doc_encoding.py`](_doc_encoding.py)（文档编码处理）。

---

## 14. 当前状态与已知限制

**已实现并接入：**

- 视觉串口协议解析、最新帧、断流判定与 `[VS]` 统计。
- 50 Hz 纯 P 视觉闭环：死区 → 增益 → ±2 rpm 限幅 → 云台速度模式。
- 双轴 STEP/DIR 脉冲后端：频率/脉冲数、DIR 建立时间、比较中断计数、寄存器自检。
- 云台层：角度↔脉冲、相对/绝对移动、速度模式、软件门控与"定位优先、视觉让路"仲裁。
- 安全：上电激光置关、启动故障强制不出光、目标丢失关光闩锁、栈溢出与启动故障上报。
- 开机诊断：`[RST]` 复位原因、`[TIM]` 定时器自检、`[HB]` 心跳。

**仍未完成 / 未验证（如实列出）：**

| 项 | 状态 |
| --- | --- |
| 视觉方向符号 / DIR 极性 | ⬜ 未在实物上最终定案，首次试车必须确认转向 |
| PID 的 I / D | ⬜ 只有 P，Ki/Kd 宏未接入 |
| `MotorTask` / `CmdTask` | ⬜ 占位任务，只计数 |
| 上电点动自检 | ⬜ `AVE_ENABLE_STARTUP_MOTOR_TEST` 未被业务代码引用；可用 `AVE_DIAG_JOG_PULSES` 临时打开 |
| 云端（ESP8266 + MQTT） | ⬜ `AVE_ENABLE_CLOUD = 0`，未接入 |
| 电机位置回读 | ⬜ `AVE_ENABLE_MOTOR_READBACK = 0`，未接入 |
| ZDT 串口协议后端 | ✅ 已删除，当前电机走 STEP/DIR |
| 电机互斥量 `MotorMutex` | ⬜ 已创建但零调用者，当前不保护任何东西 |

**容易误判的设计行为：**

- 上电 `p=0,0`（两轴零脉冲）是门控默认关闭，不是电机故障。
- `[VS] st=1` 在开机约 201 ms 内出现属正常：视觉尚未上线，断流是预期的安全状态。
- `[TR] hz=0` 且 `rj` 上涨，说明指令被门控/量程/忙碌拒收，不是 PID 算错。

---

## 15. 开发约定（摘自 `CLAUDE.md`）

| 约定 | 要点 |
| --- | --- |
| 读取范围 | 分模块开发；不一次性全量扫描工程；HAL/CMSIS 不需要读 |
| 改动交付 | 改动位置表 + 代码解读 + 系统视角七问 + 至少一张表/图 + 中文注释 |
| 自测 | 完成任务必须实际运行或审查，只维护一份 `verify.md` |
| 文档上限 | `plan.md` / `verify.md` / `ROADMAP.md` / `temporary.md` 每份 ≤ 1000 行 |
| 上下文交接 | 上下文超过 70% 时先写 `temporary.md` 再换窗口 |
| 风格 | 讲取舍、给现象、不吹牛；测了写 ✅，没测算 ⬜ |
