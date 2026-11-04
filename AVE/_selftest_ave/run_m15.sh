#!/bin/sh
#
# ⛔ 已作废（2026-09-29，第二十八次，用户裁决）—— **不再执行**，见下方文件头说明。
echo "⛔ run_m15.sh 已作废（2026-09-29，第二十八次，用户裁决），未执行任何测试。" >&2
echo "   原因：假 HAL 不建模定时器（TIM_TypeDef/TIM_CR1_ARPE/GPIO_TypeDef），" >&2
echo "         而 AVE_App_Init() 自第二十七次起会调 app_boot_stepper() ⇒ 它建不起来，" >&2
echo "         且整整一轮没人发现（第二十八次跑全套时才炸出来）。" >&2
echo "   ⚠️ 它原先覆盖的『M1.5 装配层运行时行为』目前【没有替代套件】—— 见下方说明。" >&2
echo "   现行套件：run_test_stepper.sh（脉冲后端 + 定时器自检）/ run_runtime.sh（串口层与板级）。" >&2
exit 99
#
# ⛔⛔⛔ 以下是作废前的原文，保留作留痕，**不再执行** ⛔⛔⛔
#
# ── 为什么废（2026-09-29 第二十八次）────────────────────────────────────────
#   修链接清单（把 AVE_stepper.c 加进 CSRC）只是走了一半：本脚本的假 HAL 还**不建模定时器**，
#   而 AVE_Stepper_SelfTest() 是寄存器级的（直读 CCMR2/CCER/CR1/PSC）。
#   补齐这层仿真的收益，比不上"把脉冲后端的验收集中在 run_test_stepper.sh 一处"。
#
# ⚠️ **覆盖缺口（诚实边界，不许含糊）：本脚本废弃后，任务装配层【没有】任何替代覆盖。**
#   run_runtime.sh    → 串口层与板级
#   run_test_stepper.sh → 脉冲后端 + 定时器配置自检
#   **都不测"4 个任务被正确创建 / 周期不漂 / 非阻塞发送状态机"。**
#   要恢复这层覆盖，得另写一个不依赖 `AVE_App_Init()` 完整调用链的套件（例如只链接
#   `AVE_app.c` 的任务装配部分 + 一个桩 `AVE_Stepper_Init()`），**本轮未做**。
#
# M1.5 运行时自检 —— 装配层（4 个任务 + 非阻塞日志）的构建、运行与**变异测试**
#
# ── 和 run_runtime.sh（M1.3）的分工 ──────────────────────────────────────────
#   run_runtime.sh   串口层与板级：环形缓冲、按行切帧、重新武装、激光极性
#   本脚本           装配层：任务创建参数、周期不漂、非阻塞发送状态机
#
#   刻意分成两个脚本而不是塞进一个：M1.3 那套已经跑绿了，
#   往同一个脚本里加东西就得重构它，而重构已验证的东西本身就是引入风险。
#
# ── 本次的替身比 M1.3 多了一个 CMSIS-RTOS2 ───────────────────────────────────
#   HAL 用 host/fake_hal.c，RTOS 用 host/fake_os2.c。
#
#   🔴 但 **CMSIS 的类型和常量一个都不是抄的** —— 用的是工程里那份真头文件
#      `Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.h`。
#      抄一份枚举的坏处很具体：抄错一位（比如把 osMutexPrioInherit 记成 0x04），
#      测试照样全绿 —— 因为它测的是"产品代码和我的抄写一致"，而不是和真值一致。
#
# ── 五处变异，对应五个"少了一行就静默失效"的地方 ────────────────────────────
#   M5 发送前不拷贝 → 线上内容被调用方覆盖，T3 必须拦住
#   M6 周期改成"每轮现算" → 长期漂移，P1/P2 必须拦住
#   M7 不检查 osThreadNew 返回值 → 任务静默消失，A4 必须拦住
#   M8 发送完成回调不清忙标志 → 日志打几条后永久沉默，T2 必须拦住
#   M9 错误回调不 Abort 发送方向 → 一次接收错误后日志永久沉默，T5 必须拦住
#
#   第六条（M3.2 前半，2026-09-26 补）对应 AVE_vision.c 的落地：
#   M10 任务里不排空视觉口 → 模块没人调用，板子上什么都不发生，P5 必须拦住
#
#   ⚠️ 头文件拷贝必须连同同目录邻居一起进变异目录，
#      因为 C 的引号包含 `#include "AVE_board.h"` **先搜包括者自己所在的目录**。
#      这个坑 M1.3 时踩过一次（见 run_runtime.sh 文件头），这里沿用同样的做法，
#      并且每一步都用 `gcc -M` 让编译器自报门庭。
#
# 临时验证脚手架，不进版本库、不进 Keil 工程。

set -u

HERE=/e/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave
D=/e/FREERTOS/18-视觉跟踪二维打靶系统/AVE
PROJ=$D/AVE
HOST=$HERE/host
BUILD=$HERE/build

# CMSIS-RTOS2 的真头文件所在目录（工程自己的那份）
CMSIS=$D/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
CMSIS_INC=$D/Drivers/CMSIS/Include

GCC=${GCC:-/d/mingw64/bin/gcc.exe}

CFLAGS="-std=c99 -Wall -Wextra -O1"

INC="-I$HOST -I$PROJ -I$CMSIS -I$CMSIS_INC"

# 需要复制的产品头文件。**显式列出，不用通配符** ——
# 通配符 `*.h` 将来会把 AVE_secrets.h（含明文凭据）一起卷进来。
HDRS="AVE_config.h AVE_tuning.h AVE_board.h AVE_uart.h AVE_ringbuf.h AVE_log.h AVE_app.h AVE_vision.h AVE_gimbal.h"
#
# ⚠️ 2026-09-29（第 28 次）：**同一个坑第二次踩** —— 第 27 次 `AVE_App_Init()`
#    里接了 `app_boot_stepper()`，它调 `AVE_Stepper_Init()` / `AVE_Stepper_SelfTest()`，
#    于是本脚本又以 `undefined reference` 建不起来，**而且没人在收尾时跑它**，
#    坏了整整一轮才被发现（是在第 28 次跑全套时才炸出来的）。
#    现象：`run_m15.sh` 第一步就报 undefined reference，`EXIT` 仍是 0 ⇒ 看起来"通过"。
#    🔴 教训不是"补一行"，是**改 `AVE_App_Init()` 的调用链时必须跑全部三个套件**
#    （`run_m15.sh` / `run_runtime.sh` / `run_test_stepper.sh`），别只跑改到的那一个。
CSRC="AVE_uart.c AVE_board.c AVE_log.c AVE_app.c AVE_vision.c AVE_stepper.c AVE_gimbal.c"

# 编译单元清单：$1 = 目录。用于变异体的 -M 断言 —— 必须是**变异目录里的**那一份，
# 拿项目里的原文件去问编译器，它当然会说是原文件（M1.3 时正是这么栽的）。
units() {
  printf '%s ' "$1/AVE_app.c" "$1/AVE_log.c" "$1/AVE_uart.c" "$1/AVE_board.c" \
                "$1/AVE_vision.c" "$1/AVE_stepper.c" "$1/AVE_gimbal.c"
}

if [ ! -x "$GCC" ]; then
  echo "找不到宿主编译器：$GCC"
  echo "（可用 GCC=/路径/gcc 覆盖）"
  exit 2
fi

if [ ! -f "$CMSIS/cmsis_os2.h" ]; then
  echo "找不到 CMSIS-RTOS2 头文件：$CMSIS/cmsis_os2.h"
  exit 2
fi

mkdir -p "$BUILD"

ok=0
bad=0
note() { printf '%s\n' "$*"; }
hr()   { note "=============================================================="; }

# ============================================================================
# 一、正常构建 —— 产品代码原样
# ============================================================================
hr; note " 1. 正常运行（产品代码一个字节都没改）"; hr

# 🔴 先把上一轮的可执行文件删掉。
#
#    否则一旦编译失败（例如用例文件里有个笔误），上面那个旧的 exe 还原封不动地躺在那里，
#    下面的 `[ -x ]` 检查照样通过，脚本就会**拿旧二进制跑出满屏绿字** ——
#    而你在改了代码之后看到全绿，会以为改动没问题。
#    这个坑本脚本第一版就踩了：改了用例名却忘了改调用处，编译是失败的，
#    但输出和上一轮一模一样，看着像"改动没生效"，实际是"根本没编译成功"。
#    判据不对会骗人，**判据之前的每一步也会骗人**。
rm -f "$BUILD/run_m15.exe"

$GCC $CFLAGS $INC -o "$BUILD/run_m15.exe" \
     "$HERE/ave_m15_runtime.c" \
     $(units "$PROJ") \
     "$HOST/fake_hal.c" "$HOST/fake_os2.c" "$HOST/host_trace.c" 2>&1 | tee "$BUILD/build_m15.log"

if [ ! -x "$BUILD/run_m15.exe" ]; then
  note "→ 编译失败，见 $BUILD/build_m15.log"
  exit 3
fi
if grep -q "error:" "$BUILD/build_m15.log"; then
  note "→ 编译器报了 error（见上），判据无效"
  exit 3
fi
[ -s "$BUILD/build_m15.log" ] && note "（以上是编译器告警；下方用例才是判据）"

"$BUILD/run_m15.exe"
if [ $? -eq 0 ]; then
  note "→ 正常构建：全部用例通过"
  ok=$((ok + 1))
else
  note "→ 正常构建：**有用例失败** —— 这是真 bug，不是脚手架问题"
  bad=$((bad + 1))
fi

# ============================================================================
# 二、变异测试
# ============================================================================

mk_mut() {
  name="$1"; expr="$2"; target="$3"

  d="$BUILD/m15_mut_$name"
  rm -rf "$d"; mkdir -p "$d"

  for f in $HDRS $CSRC; do
    cp "$PROJ/$f" "$d/$f"
  done

  sed -i "$expr" "$d/$target"

  if cmp -s "$PROJ/$target" "$d/$target"; then
    note "  !! 变异未生效（sed 没匹配到目标文本）—— 这是脚手架自身的 bug，先修它"
    bad=$((bad + 1))
    return 1
  fi

  # 让编译器自报门庭：确认这个翻译单元解析到的确实是变异体
  if ! $GCC -I"$d" $INC -M $(units "$d") 2>/dev/null \
        | tr ' \\' '\n\n' | grep -q "m15_mut_$name/$target"; then
    note "  !! 编译器解析到的 $target **不是**变异体（仍指向原文件）—— 这条变异无效"
    note "     （这正是 M1.3 时 M4 踩过的坑，见文件头说明）"
    bad=$((bad + 1))
    return 1
  fi

  return 0
}

# mut_run <标签> <期望失败的用例前缀> <变异目录>
mut_run() {
  label="$1"; expect="$2"; d="$3"

  exe="$BUILD/m15_mut_$label.exe"
  log="$BUILD/m15_mut_$label.log"

  rm -f "$exe"      # 理由同上面正常构建那一段：绝不拿上一轮的旧二进制当结果

  $GCC $CFLAGS -I"$d" $INC -o "$exe" \
       "$HERE/ave_m15_runtime.c" \
       $(units "$d") \
       "$HOST/fake_hal.c" "$HOST/fake_os2.c" "$HOST/host_trace.c" > "$log" 2>&1
  if [ ! -x "$exe" ] || grep -q "error:" "$log"; then
    note "  !! 变异体编译失败，见 $log → 这条测不了"
    bad=$((bad + 1))
    return
  fi

  "$exe" >> "$log" 2>&1
  if [ $? -eq 0 ]; then
    note "  ✗ 变异后**测试仍然全过** —— 这套用例测不出「$label」"
    bad=$((bad + 1))
    return
  fi

  if grep -q "FAIL \[$expect" "$log"; then
    note "  ✓ 被预期用例拦住：$(grep -m1 "FAIL \[$expect" "$log" | sed 's/^ *//')"
    ok=$((ok + 1))
  else
    note "  ? 测试确实失败了，但**不是预期的 [$expect]**："
    grep "FAIL \[" "$log" | head -5 | sed 's/^/      /'
    note "     （见 $log）"
    bad=$((bad + 1))
  fi
}

hr; note " 2. 变异测试（故意改坏产品代码，确认判据真的会失败）"; hr

# M5：发送前不拷贝，直接把调用方的指针交给 HAL
if mk_mut m5 's|HAL_UART_Transmit_IT(s_huart\[ch\], s_tx_buf\[ch\], (uint16_t)len)|HAL_UART_Transmit_IT(s_huart[ch], buf, (uint16_t)len)|' AVE_uart.c; then
  note "M5 发送前不拷贝 —— 期望 T3 拦住"
  mut_run m5 "T3 " "$BUILD/m15_mut_m5"
fi

# M6：周期改成"每轮用当前 tick 现算"，误差逐轮累积
if mk_mut m6 's|next += AVE_TASK_VISION_PERIOD_MS;|next = osKernelGetTickCount() + AVE_TASK_VISION_PERIOD_MS;|' AVE_app.c; then
  note "M6 周期改成每轮现算（漂移）—— 期望 P1 拦住"
  mut_run m6 "P1 " "$BUILD/m15_mut_m6"
fi

# M7：不检查 osThreadNew 的返回值
#
#   ⚠️ 2026-09-28（第二十六次）锚点换过位置。原来打的是四个内联
#      `if (osThreadNew(VisionTask, 0, &s_vision_attr) == 0)` 里的第一句；
#      重构把四句收进 `app_create_thread()` 一处，那句文本就没了 ——
#      sed 匹配不到，脚本报「变异未生效」并计失败。
#      **这正是那道守卫存在的意义**：换过锚点的变异若默默算通过，
#      它会以"测试没发现"的形式骗人。现在改打公共函数里的那一句，
#      一次干掉全部四个任务的返回值检查 —— 比原来更强（A4 会循环四个 k）。
if mk_mut m7 's|if (osThreadNew(fn, 0, attr) == 0)|if (0)|' AVE_app.c; then
  note "M7 不检查 osThreadNew 返回值 —— 期望 A4 拦住"
  mut_run m7 "A4 " "$BUILD/m15_mut_m7"
fi

# M8：发送完成回调不清忙标志
#     目标行只出现在 TxCpltCallback 与 ErrorCallback 里（都写了两空格缩进），
#     两处一起失效正好就是"忙标志再也没人清了"这个故障整体。
if mk_mut m8 's|^  s_tx_busy\[ch\] = 0U;$|  /* 变异：不清忙标志 */|' AVE_uart.c; then
  note "M8 发送完成回调不清忙标志 —— 期望 T2 拦住"
  mut_run m8 "T2 " "$BUILD/m15_mut_m8"
fi

# M9：错误回调不 Abort 发送方向
if mk_mut m9 's|(void)HAL_UART_AbortTransmit_IT(huart);|/* 变异：不 Abort 发送 */|' AVE_uart.c; then
  note "M9 错误回调不 Abort 发送方向 —— 期望 T5 拦住"
  mut_run m9 "T5 " "$BUILD/m15_mut_m9"
fi

# M10（M3.2 前半，2026-09-26 补）：任务里不再排空视觉口 —— 也就是
# `AVE_vision.c` 写得再好也**没有人在调它**。
#     这是最像"成功"的失败：代码全在、编译零警告、m13 的 V 组**依然全过**
#     （那是纯模块测试，不经过任务），板子上却什么都不发生。
#     只有 P5 这一条接线用例能发现它。
#     ⚠️ 2026-09-28（第二十六次）锚点末尾加了 `.*$`：重构后这一行后面
#        多了一段行内注释，原来以 `;$` 收尾的写法匹配不到了。
#        **只放宽到"行尾任意内容"，没有放宽到"任意出现位置"** ——
#        若去掉行首的 `^    ` 锚，`AVE_vision.c` 里若有同名调用也会被改到。
if mk_mut m10 's|^    AVE_Vision_Poll(now);.*$|    /* 变异：不排空视觉口 */|' AVE_app.c; then
  note "M10 任务里不排空视觉口（模块没人调用）—— 期望 P5 拦住"
  mut_run m10 "P5 " "$BUILD/m15_mut_m10"
fi

# ============================================================================
hr
note " 汇总（1 次正常构建 + 6 项变异）：$ok 符合预期 / $bad 不符合预期"
hr

if [ $bad -eq 0 ]; then
  note "结论：M1.5 的**装配与日志逻辑已在本机验证通过**（上板那部分仍未验）；"
  note "      外加 M3.2 前半的**接线**：VisionTask 确实在调 AVE_Vision_Poll()，"
  note "      并且 [VS] 行确实发得出来、与心跳错相（语义本身由 m13 的 V 组验）。"
  exit 0
fi

note "结论：有 $bad 项不符合预期，**未通过**。"
exit 1
