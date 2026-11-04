#!/bin/sh
#
# M1.3 运行时自检 —— 构建 + 运行 + **变异测试**
#
# ── 为什么要有变异测试这一段 ────────────────────────────────────────────────
#   "全部通过"本身不构成证据：一套恒真的判据也永远全部通过，而它比不做测试更糟，
#   因为它给出**虚假的安心**。（这条教训本工程已经吃过两次 ——
#   M1.3 那条 `sizeof(s_huart)/sizeof(s_huart[0]) == AVE_UART_CH_NUM` 的检查，
#   因为数组声明带长度而恒等于右式，永远不可能失败。）
#
#   所以本脚本除了跑一遍正常构建，还会**故意把产品代码改坏十四处**，
#   逐一确认对应用例真的会失败。改坏后仍然全过 = 那套判据是假的。
#
#   十四处变异，对应十四条最要紧的结论：
#     M1 漏掉重新武装      → 串口静默死亡，U1 必须拦住
#     M2 删掉逃生路径      → 满且无换行时永久卡死，U6 必须拦住
#     M3 删掉 Abort 再武装 → 出错后再也收不到数据，U10 必须拦住
#     M4 激光极性两支对调  → 上电就出光（安全攸关），L1 必须拦住
#
#   后四条（M1.6，2026-09-26 补）对应 `Error_Handler()` 从"静默死循环"改成
#   "记码 + 返回（降级运行）"那次改造 —— 它们的共同点是**改坏了既没有编译告警、
#   也没有任何运行症状**，只有把判据真的跑一遍才发现得了：
#     M5 故障码后到覆盖先到 → 根因被症状顶掉，B2 必须拦住
#     M6 删掉"0 不是故障码"的守卫 → 板子被误标成健康的，B1 必须拦住
#     M7 故障计数不再累加  → "先到先得"把次数也吞了，B3 必须拦住
#     M8 激光联锁失效      → 启动失败也照出光（安全攸关），B4 必须拦住
#
#   后六条（M3.2 前半，2026-09-26 补）对应 `AVE/AVE_vision.c` 的落地 ——
#   让 `AVE_Uart_ReadLine()` 第一次有真正的消费者。这一组错法的共同点是：
#   **照字面写验收一条都测不出来**（居中帧的偏差恒为 0；单发一次无目标帧
#   永远来不及超时；`[VS]` 行在桌面上只有几十秒，永远撞不到 98 字节预算）：
#     M9  范围校验打掉      → 分辨率填错再无任何症状，V3 必须拦住
#     M10 只取最新→只认第一帧 → 一次排空里 9 行被静默忽略，V5 必须拦住
#     M11 断流每轮都计数    → "一直断着"看起来像"疯狂断线"，V6 必须拦住
#     M12 无目标帧不刷新计时 → 目标丢失被当成链路丢失，V4 必须拦住
#     M13 位数上限 9→99     → 十位数字回绕，整数溢出 UB 的入口，V9 必须拦住
#     M14 计数打印不封顶    → [VS] 行宽随计数增长（90 > 钉住的 82），V11 必须拦住
#
#   ⚠️ B 组用例（启动故障 + 联锁）排在 L1 **之后**，顺序即判据 ——
#      故障码置位后没有清除接口，放在 L1 前面会让 L1 的 Set(1) 被联锁按回关电平。
#      详细理由见 ave_m13_runtime.c 里 B 组那一节的文件头注释。
#
# ── 一个踩过的坑：**文件改了 ≠ 文件被用了** ─────────────────────────────────
#   变异用的是 `sed` 改一份拷贝。头文件的拷贝必须**连同它的同目录邻居一起**
#   放进变异目录，并且该目录要排在 `-I` 最前面。原因是 C 的引号包含
#   `#include "AVE_board.h"` 会**先搜包括者自己所在的目录**：
#     AVE_config.h 在项目目录里，它写的是引号包含，
#     于是 `AVE_board.h` 永远从项目目录解析 —— 变异目录里的那份**看都没被看过**。
#   这个坑第一次跑就现形了：M4（激光极性对调）"变异后测试仍然全过"。
#   注意那**不是**"用例太弱"，而是变异根本没生效 —— 两者必须分开。
#   所以下面每一步都用 `-M` 让编译器自报门庭：确认它解析到的确实是变异体。
#
# 临时验证脚手架，不进版本库、不进 Keil 工程。

set -u

HERE=/e/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave
PROJ=/e/FREERTOS/18-视觉跟踪二维打靶系统/AVE/AVE
HOST=$HERE/host
BUILD=$HERE/build

# 宿主编译器：默认 MinGW-w64，可用环境变量覆盖。
#
# ⚠️ 路径**不能带空格**：下面的调用历史上写成 `$GCC ...`（没加引号），
#    `C:\Program Files\...` 会被 shell 拆成两个词。本轮已给所有的 `$GCC`
#    补上引号，但**保留**这条"不要空格"的提醒 —— 引号只挡住当前这几处，
#    以后往这里加调用的人未必记得加。
#
# 🔴 2026-10-13（第六十一次）实测：文档里一直记着的 `D:\mingw64` **在本机已不存在**
#    （换了机器/盘符），脚本因此连正常构建都跑不起来 —— 而它报的是
#    "找不到宿主编译器"，不是"测试失败"，所以**不会**被误读成产品有问题。
#    本机实际可用、且路径纯 ASCII 的两支：
#      /c/PROGRA~1/RedPanda-Cpp/MinGW64/bin/gcc.exe   （gcc 11.4.0 ← 本轮全部结论出自它）
#      /c/PROGRA~2/Dev-Cpp/MinGW64/bin/gcc.exe        （gcc 4.9.2，较旧，未用）
#    `PROGRA~1` 是 Windows 8.3 短名，用途就是给带空格的目录名去空格。
GCC=${GCC:-/c/PROGRA~1/RedPanda-Cpp/MinGW64/bin/gcc.exe}

CFLAGS="-std=c99 -Wall -Wextra -O1"

# 需要复制的产品头文件。**显式列出，不用通配符** ——
# 通配符 `*.h` 将来会把 AVE_secrets.h（含明文凭据）一起卷进来。
HDRS="AVE_config.h AVE_tuning.h AVE_board.h AVE_uart.h AVE_ringbuf.h AVE_vision.h"
CSRC="AVE_uart.c AVE_board.c AVE_vision.c"

if [ ! -x "$GCC" ]; then
  echo "找不到宿主编译器：$GCC"
  echo "（可用 GCC=/路径/gcc 覆盖）"
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

# 🔴 先把上一轮的可执行文件删掉 —— 否则编译失败时旧 exe 还在，
#    下面的 `[ -x ]` 照样通过，脚本会**拿旧二进制跑出满屏绿字**。
#    （这个坑在 M1.5 的脚本里真踩了一次：改了用例名忘了改调用处，
#      编译是失败的，输出却和上一轮一字不差，看着像"改动没生效"。）
rm -f "$BUILD/run.exe"

# ⚠️ 2026-09-27（M2.4）：`fake_hal.c` 里加了事件轨迹的调用（`host_trace("txit")` 等），
#    所以这里必须再链上 `host_trace.c`，否则 undefined reference。
#    **这一条与本套用例有没有测云台无关** —— 替身共享，动一处，三个脚本全受影响。
"$GCC" $CFLAGS -I"$HOST" -I"$PROJ" -o "$BUILD/run.exe" \
     "$HERE/ave_m13_runtime.c" "$PROJ/AVE_uart.c" "$PROJ/AVE_board.c" \
     "$PROJ/AVE_vision.c" "$HOST/fake_hal.c" "$HOST/host_trace.c" 2>&1 | tee "$BUILD/build.log"

if [ ! -x "$BUILD/run.exe" ]; then
  note "→ 编译失败，见 $BUILD/build.log"
  exit 3
fi
if grep -q "error:" "$BUILD/build.log"; then
  note "→ 编译器报了 error（见上），判据无效"
  exit 3
fi
[ -s "$BUILD/build.log" ] && note "（以上是编译器告警；下方用例才是判据）"

"$BUILD/run.exe"
if [ $? -eq 0 ]; then
  note "→ 正常构建：全部用例通过"
  ok=$((ok + 1))
else
  note "→ 正常构建：**有用例失败** —— 这是真 bug，不是脚手架问题"
  bad=$((bad + 1))
fi

# ============================================================================
# 一之二、🔴 基线闸门 —— 基线不绿就**不许**进变异段（2026-10-13 第六十一次加）
#
# ── 为什么必须有这道闸 ──────────────────────────────────────────────────────
#   `mut_run` 的判据是「期望的那条用例**失败**了吗」。这句话只有在
#   **基线是绿的**时候才有信息量：基线本来就红时，那条判据**恒真**，
#   于是所有变异都会报「✓ 被预期用例拦住」—— 输出与真绿**一字不差**。
#
#   这不是假设，是实测：2026-10-08 画面 `640×480 → 224×224` 之后，
#   V 组九条用例因为写死了 320/240 而全部越界、全部变红，而这套用例
#   此后**没被跑过**。于是 M9（期望 V3）、M10（期望 V5）、M11（期望 V6）
#   整整 5 天**假绿**，文档里还挂着「M3.2 前半已通过运行时验证」的结论。
#
#   ⇒ 只要正常构建有失败，立刻停在这里。**宁可不跑，也不出一份假绿的报告。**
#   ⚠️ 它管的是"基线"，不是"变异体"——变异体编译失败已经由 `mut_run` 单独拦了。
# ============================================================================
if [ $bad -ne 0 ]; then
  hr
  note " 🔴 基线不绿 ⇒ **不跑变异测试**。"
  note "    理由：基线红时，「变异有没有生效」与「基线本来就是坏的」在输出上"
  note "    无法区分，14 项变异会全报通过 —— 那份报告比不做测试更糟。"
  note "    先修基线，再回来跑本脚本。"
  hr
  note "汇总（仅正常构建）：$ok 符合预期 / $bad 不符合预期 —— **未通过**"
  exit 1
fi

# ============================================================================
# 二、变异测试
# ============================================================================

# 造一个变异目录：**整份产品代码的拷贝**（头文件+源码），只改其中一个文件。
# 整份拷贝是必要的，不能只放被改的那一个 —— 原因见文件头的"引号包含"那一段。
mk_mut() {
  name="$1"; expr="$2"; target="$3"

  d="$BUILD/mut_$name"
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

  # 让编译器自报门庭：确认这个翻译单元解析到的确实是变异体，不是原文件
  if ! "$GCC" -I"$d" -I"$HOST" -I"$PROJ" -M \
        "$HERE/ave_m13_runtime.c" "$d/AVE_uart.c" "$d/AVE_board.c" "$d/AVE_vision.c" 2>/dev/null \
        | tr ' \\' '\n\n' | grep -q "mut_$name/$target"; then
    note "  !! 编译器解析到的 $target **不是**变异体（仍指向原文件）—— 这条变异无效"
    note "     （这正是 M4 第一次跑时踩的坑，见文件头说明）"
    bad=$((bad + 1))
    return 1
  fi

  return 0
}

# mut_run <标签> <期望失败的用例前缀> <变异目录>
# 判据必须能失败，而且必须是**指定的那条**判据失败 ——
# 只检查"有没有失败"不够：用错了判据也算失败，但那说明不了问题。
mut_run() {
  label="$1"; expect="$2"; d="$3"

  exe="$BUILD/mut_$label.exe"
  log="$BUILD/mut_$label.log"

  rm -f "$exe"      # 理由同上面正常构建那一段：绝不拿上一轮的旧二进制当结果

  "$GCC" $CFLAGS -I"$d" -I"$HOST" -I"$PROJ" -o "$exe" \
       "$HERE/ave_m13_runtime.c" "$d/AVE_uart.c" "$d/AVE_board.c" \
       "$d/AVE_vision.c" "$HOST/fake_hal.c" "$HOST/host_trace.c" > "$log" 2>&1
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

if mk_mut m1 's|HAL_UART_Receive_IT(s_huart\[ch\], &s_rx_byte\[ch\], 1U) != HAL_OK|0|' AVE_uart.c; then
  note "M1 漏掉重新武装 —— 期望 U1 拦住"
  mut_run m1 "U1 " "$BUILD/mut_m1"
fi

if mk_mut m2 's|if (used >= rb->size)|if (0 \&\& used >= rb->size)|' AVE_uart.c; then
  note "M2 删掉逃生路径 —— 期望 U6 拦住"
  mut_run m2 "U6 " "$BUILD/mut_m2"
fi

if mk_mut m3 's|(void)HAL_UART_AbortReceive_IT(huart);|/* 变异：不 Abort */|' AVE_uart.c; then
  note "M3 删掉 Abort 再武装 —— 期望 U10 拦住"
  mut_run m3 "U10" "$BUILD/mut_m3"
fi

if mk_mut m4 's|((on) ? GPIO_PIN_SET : GPIO_PIN_RESET)|((on) ? GPIO_PIN_RESET : GPIO_PIN_SET)|' AVE_board.h; then
  note "M4 激光极性对调 —— 期望 L1 拦住"
  mut_run m4 "L1 " "$BUILD/mut_m4"
fi

# ── M1.6：启动故障记录 + 激光联锁（2026-09-26 补）────────────────────────────
#
# 这四条变异对应"Error_Handler 从静默死循环改成记码 + 降级运行"那次改造。
# 它们的共同点是：**改坏了不会有任何编译告警、也不会有任何运行症状**，
# 只有把判据真的跑一遍才发现得了。

# M5：先到先得改成"后到覆盖先到"（根因被症状顶掉）
if mk_mut m5 's|if (s_boot_fail_code == (uint32_t)AVE_BOARD_BOOT_FAIL_NONE)|if (1)|' AVE_board.c; then
  note "M5 故障码改成后到覆盖先到 —— 期望 B2 拦住"
  mut_run m5 "B2 " "$BUILD/mut_m5"
fi

# M6：删掉"0 不是故障码"的守卫（调用方的笔误会把板子标成健康的）
if mk_mut m6 's|if (code == (uint32_t)AVE_BOARD_BOOT_FAIL_NONE)|if (0)|' AVE_board.c; then
  note "M6 删掉故障码 0 的守卫 —— 期望 B1 拦住"
  mut_run m6 "B1 " "$BUILD/mut_m6"
fi

# M7：计数不再累加（"先到先得"顺手把次数也吞了）
if mk_mut m7 's|^  s_boot_fail_count++;$|  s_boot_fail_count += 0;|' AVE_board.c; then
  note "M7 故障计数不再累加 —— 期望 B3 拦住"
  mut_run m7 "B3 " "$BUILD/mut_m7"
fi

# M8：激光联锁失效（启动失败也照出光 —— 这条是安全攸关的）
if mk_mut m8 's|if (AVE_Board_BootFailCode() != (uint32_t)AVE_BOARD_BOOT_FAIL_NONE)|if (0)|' AVE_board.c; then
  note "M8 启动故障后激光联锁失效 —— 期望 B4 拦住"
  mut_run m8 "B4 " "$BUILD/mut_m8"
fi

# ── M3.2 前半：视觉行解析（2026-09-26 补）─────────────────────────────────
#
# 这一组的共同点：错法**全都不会让编译报错**，也不会让"正常喂一帧"的
# 用例失败。照字面写验收（`320,240,1` → 偏差 0）时它们一个都测不出来：
# 居中帧的偏差恒为 0、单发一次无目标帧永远来不及超时。

# M9：范围校验打掉（分辨率填错就再没有任何症状）
if mk_mut m9 's|return (x >= 0) && (x < AVE_VISION_SCREEN_W) && (y >= 0) && (y < AVE_VISION_SCREEN_H);|{ (void)x; (void)y; return 1; }|' AVE_vision.c; then
  note "M9 范围校验失效 —— 期望 V3 拦住"
  mut_run m9 "V3 " "$BUILD/mut_m9"
fi

# M10：只取最新 → 只认本轮第一帧（性能宣称变成一句空话）
if mk_mut m10 's|^  s_latest = \*f;.*$|  if (s_latest.tick_ms != now_ms) { s_latest = *f; }|' AVE_vision.c; then
  note "M10 只取最新改成只认本轮第一帧 —— 期望 V5 拦住"
  mut_run m10 "V5 " "$BUILD/mut_m10"
fi

# M11：断流每轮都计数（"一直断着"看起来像"疯狂断线"）
if mk_mut m11 's|if ((stale_now != 0U) && (s_state.stale == 0U))|if (stale_now != 0U)|' AVE_vision.c; then
  note "M11 断流每轮都计数 —— 期望 V6 拦住"
  mut_run m11 "V6 " "$BUILD/mut_m11"
fi

# M12：无目标帧不刷新断流计时（§6.3 硬约束 3 —— 目标丢失被当成链路丢失）
#
# 🔴 2026-09-29（第 28 次）修锚点：原式是
#    `s|^    s_state.last_frame_ms = now_ms;  /\* 无目标帧同样刷新.*$|…|`
#    —— 它要求那一行**同行**带 `/* 无目标帧同样刷新` 注释。但那句注释后来被挪进了
#    函数头文档块，现文本（`AVE_vision.c:283`）只剩两个尾随空格，**锚点对不上**。
#    症状：本项打印"变异未生效（sed 没匹配到目标文本）"，V4 判据**根本没跑**
#    —— 这就是"变异没生效"冒充"测试通过"的那类假绿（见 memory:
#    mutation-test-can-fail-silently）。**好在脚本自己把这条报了出来**，没吞掉。
#
# 改法：不再锚同行注释，改用**地址区间** `no_target++` → 下一条 `last_frame_ms = now_ms;`。
#   为什么这样是**对**的而不只是"能跑"：
#   ① `no_target++`（`AVE_vision.c:278`）全文件**唯一**，grep -c = 1；
#   ② 区间起点在 :278，而 OK 分支那次赋值在 :274 —— **在起点之前**，
#      所以绝不会误改到 OK 分支（那样就变成另一个变异了，且同样测不出东西）；
#   ③ 区间对**空白不敏感**，下次再有人调格式不会第二次踩这个坑。
if mk_mut m12 '/s_state.no_target++;/,/s_state.last_frame_ms = now_ms;/s|s_state\.last_frame_ms = now_ms;|/* 变异：无目标帧不刷新 */|' AVE_vision.c; then
  note "M12 无目标帧不刷新 last_frame_ms —— 期望 V4 拦住"
  mut_run m12 "V4 " "$BUILD/mut_m12"
fi

# M13：位数上限 9 → 99（十位数字开始回绕，溢出进入未定义行为的地盘）
if mk_mut m13 's|if (digits > AVE_VISION_INT_MAX_DIGITS)|if (digits > 99)|' AVE_vision.c; then
  note "M13 位数上限 9 → 99 —— 期望 V9 拦住"
  mut_run m13 "V9 " "$BUILD/mut_m13"
fi

# M14：去掉 8 位封顶 —— 行宽从此随计数增长（实测 90 > 钉住的 82）。
#      ⚠️ 注意这条变异拦住的**不是** 98 字节预算（90 也塞得下），而是"行宽与
#         运行时长无关"这条静态性质。早先的方案里我写过"不封顶 = 99 必被截断"，
#         那是**算错了**（把 0/1 标志 st 当成了 8 位计数）；见 AVE_vision.h 里的更正。
if mk_mut m14 's|if (v > AVE_VISION_PRINT_CAP)|if (0)|' AVE_vision.c; then
  note "M14 计数打印不封顶 —— 期望 V11 拦住"
  mut_run m14 "V11" "$BUILD/mut_m14"
fi

# ============================================================================
hr
note " 汇总（1 次正常构建 + 14 项变异）：$ok 符合预期 / $bad 不符合预期"
hr

if [ $bad -eq 0 ]; then
  note "结论：M1.3（串口层与板级）、M1.6（启动故障记录 + 激光联锁）以及"
  note "      M3.2 前半（视觉行解析 / 只取最新 / 偏差 / 断流）的运行时行为"
  note "      **已在本机验证通过**。"
  note "      仍未验：HAL 真把 NVIC 使能了吗、线接对了吗、误码率 —— 那些必须上板；"
  note "      以及\"时钟真的配坏之后能不能降级跑起来\"（桌面上造不出时钟故障）。"
  exit 0
fi

note "结论：有 $bad 项不符合预期，**未通过**。"
exit 1
