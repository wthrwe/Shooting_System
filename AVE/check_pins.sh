#!/usr/bin/env bash
# ═════════════════════════════════════════════════════════════════════════════
# check_pins.sh —— 引脚一致性检查（第 24 次新增）
#
# 用法:  ./check_pins.sh [AVE_board.h 路径] [AVE.ioc 路径]
# 退出码: 0 = 全部一致;  1 = 有分歧（逐条打印）
#
# ── 为什么需要这个脚本 ──────────────────────────────────────────────────────
#   第 24 次接线时发现 `AVE_board.h` 写的「PA2 → 水平轴 STEP」是错的（实际 PB10），
#   而 `AVE_ESP_UART_TX_PIN` 写的 PB10 也是错的（实际 PD8）。
#   这类错的**共同特征**：宏全工程零引用 ⇒ 编译不报、运行不报，
#   直到有人照着它去接线才发作，而现场现象（串口乱码、电机不动）
#   看起来全都像别的原因。**没有任何症状**就是它最危险的地方。
#
#   所以把"宏的值 == .ioc 的值"这件事变成一条能跑的命令。
#
# ── 判据方向 ────────────────────────────────────────────────────────────────
#   对每条 (宏, 期望信号)：
#     ① 从 header 的 PORT/PIN 宏拼出脚名（如 GPIOB + GPIO_PIN_10 → PB10）
#     ② 去 .ioc 查这个脚的 Signal
#     ③ 不比"脚名"而比"**这个脚在 .ioc 里承担什么**" —— 这样
#        「宏写了 PA2 但 .ioc 里根本没 PA2」和「宏写了 PB10 但 PB10 是串口」
#        这两种错都能被抓到，而且报错信息直接告诉你冲突在哪。
#
# ── 它管不了什么（⬜ 诚实边界）──────────────────────────────────────────────
#   · 管不了"这个脚有没有引到排针" —— 那只能拿板子看（本板 PA9/PA10 就没引出）。
#   · 管不了 .ioc 与生成代码（tim.c/gpio.c/usart.c）是否同步 —— 后者由 CubeMX 保证。
#   · 管不了"这个脚上接的器件对不对"。
# ═════════════════════════════════════════════════════════════════════════════

set -u

HDR="${1:-AVE/AVE_board.h}"
IOC="${2:-$(ls ./*.ioc 2>/dev/null | head -1)}"

if [ ! -f "$HDR" ]; then echo "找不到头文件: $HDR" >&2; exit 2; fi
if [ ! -f "$IOC" ]; then echo "找不到 .ioc: $IOC" >&2; exit 2; fi

fail=0
n=0

# .ioc 里某个脚的 Signal 值；没分配过就打印空串
#
# ⚠️ 脚名**可能带 CubeMX 的后缀**：PC13 在 .ioc 里叫 `PC13-ANTI_TAMP`
#    （`ANTI_TAMP` 只是它的默认复用标签，`Signal` 仍是 `GPIO_Output`）。
#    第 24 次首跑就栽在这里 —— 漏了后缀 ⇒ 报出「PC13 从未被分配」这种**假 FAIL**。
#    假 FAIL 比漏报更坏：它会让人去改一个本来没错的宏。
ioc_sig() { grep -E "^$1(-[A-Za-z0-9_]+)?\.Signal=" "$IOC" 2>/dev/null | head -1 | cut -d= -f2; }

# header 里某个宏的值；没有这个宏就打印 MISSING
hdr_val() {
  grep -E "^#[[:space:]]*define[[:space:]]+$1[[:space:]]" "$HDR" 2>/dev/null \
    | head -1 | awk '{print $3}'
}

# 由 PORT/PIN 两个宏拼出 "PB10" 形式的脚名
hdr_pin() {
  local p i
  p="$(hdr_val "$1")"; i="$(hdr_val "$2")"
  if [ -z "$p" ] || [ -z "$i" ]; then echo "MISSING"; return; fi
  echo "P${p#GPIO}${i#GPIO_PIN_}"
}

# check <期望的.ioc信号> <PORT宏> <PIN宏> <人话说明>
check() {
  local want="$1" pm="$2" im="$3" desc="$4"
  local pin got
  n=$((n + 1))
  pin="$(hdr_pin "$pm" "$im")"
  if [ "$pin" = "MISSING" ]; then
    echo "FAIL  [$desc] 宏 $pm / $im 在 $HDR 里找不到"; fail=1; return
  fi
  got="$(ioc_sig "$pin")"
  if [ "$got" = "$want" ]; then
    echo "  ok  [$desc] $pin  →  $got"
  else
    echo "FAIL  [$desc] $HDR 说 $pin，但 .ioc 里 $pin = ${got:-（该脚从未被分配）}，期望 $want"
    fail=1
  fi
}

echo "头文件: $HDR"
echo "真源头: $IOC"
echo "──────────────────────────────────────────────────────────────"

check "S_TIM2_CH3"     AVE_PAN_STEP_PORT  AVE_PAN_STEP_PIN   "水平轴 STEP 必须落在 TIM2_CH3 上"
check "S_TIM5_CH4"     AVE_TILT_STEP_PORT AVE_TILT_STEP_PIN  "俯仰轴 STEP 必须落在 TIM5_CH4 上（不能与水平轴共定时器）"
check "GPIO_Output"    AVE_PAN_DIR_PORT   AVE_PAN_DIR_PIN    "水平轴 DIR = 普通 GPIO 输出"
check "GPIO_Output"    AVE_TILT_DIR_PORT  AVE_TILT_DIR_PIN   "俯仰轴 DIR = 普通 GPIO 输出"
check "USART1_TX"      AVE_CAM_UART_TX_PORT AVE_CAM_UART_TX_PIN "视觉口 TX（本工程不发，仅占位）"
check "USART1_RX"      AVE_CAM_UART_RX_PORT AVE_CAM_UART_RX_PIN "视觉口 RX ← MaixCAM 的 TX"
check "USART3_TX"      AVE_ESP_UART_TX_PORT AVE_ESP_UART_TX_PIN "调试口 TX → CH340 的 RX"
check "USART3_RX"      AVE_ESP_UART_RX_PORT AVE_ESP_UART_RX_PIN "调试口 RX ← CH340 的 TX"
check "GPIO_Output"    AVE_LASER_GPIO_PORT AVE_LASER_GPIO_PIN  "激光控制脚 = 普通 GPIO 输出"

echo "──────────────────────────────────────────────────────────────"
if [ "$fail" -eq 0 ]; then
  echo "PASS  $n 条全部一致"
else
  echo "FAIL  上面标 FAIL 的条目与 .ioc 不符（照它接线会接错）"
fi
exit "$fail"
