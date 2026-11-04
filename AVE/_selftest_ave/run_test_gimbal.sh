#!/bin/sh
# =============================================================================
# AVE_gimbal.c 桌面行为测试
#
# 用法：
#   sh run_test_gimbal.sh                    # 测工程里的 AVE_gimbal.c
#   sh run_test_gimbal.sh <另一个 .c 路径>    # 测变异版（mutate_gimbal.sh 用）
#
# 🔴 先删旧产物：编译**失败**时若留着上次的 exe，"跑出来的绿"和"真绿"就分不清。
#    （踩过的坑，见 memory: verify-script-must-delete-stale-binary）
#
# ⚠️ 注意 ROOT 里的 `18-` 前缀：工程目录 2026-10 被改过名，
#    _selftest_ave/ 下几个老脚本（run_test_stepper.sh / compile_stepper.sh /
#    mutate_stepper.sh / run_m24.sh）里写的还是旧路径，现在跑不起来。
#    本脚本用的是核对过的当前路径。
# =============================================================================
set -u

ROOT="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE"
HERE="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave"
OUT="$HERE/out"
EXE="$OUT/test_gimbal.exe"
LOG="$OUT/gimbal_host.log"
GCC="D:/mingw64/bin/gcc.exe"
SRC="${1:-$ROOT/AVE/AVE_gimbal.c}"

[ -x "$GCC" ] || GCC=gcc

mkdir -p "$OUT"
rm -f "$EXE" "$LOG"

echo "被测源码：$SRC"
echo
echo "=== 1/2 编译 ==="
"$GCC" -std=c99 -O0 -g -Wall -Wextra \
  -I"$HERE/host/fake" \
  -I"$HERE/host" \
  -I"$ROOT/AVE" \
  -o "$EXE" \
  "$HERE/host/test_gimbal.c" \
  "$HERE/host/fake_stepper.c" \
  "$SRC" > "$LOG" 2>&1
RC=$?
cat "$LOG"

if [ "$RC" -ne 0 ] || [ ! -f "$EXE" ]; then
  echo ">>> 编译【失败】—— 不跑旧二进制，直接判负。"
  exit 1
fi
echo "编译通过（0 警告即上面无输出），产物：$EXE"

echo
echo "=== 2/2 跑测试 ==="
"$EXE"
RC=$?
echo
if [ "$RC" -eq 0 ]; then
  echo ">>> 测试【全部通过】"
else
  echo ">>> 测试【有失败】，退出码 $RC"
fi
exit "$RC"
