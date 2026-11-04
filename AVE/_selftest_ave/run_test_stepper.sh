#!/bin/sh
# =============================================================================
# AVE_stepper.c 桌面行为测试
#
# 🔴 先删旧产物：编译**失败**时若留着上次的 exe，"跑出来的绿"和"真绿"分不清。
#    （踩过的坑，见 memory: verify-script-must-delete-stale-binary）
# =============================================================================
set -u

ROOT="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE"
HERE="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave"
OUT="$HERE/out"
EXE="$OUT/test_stepper.exe"
GCC="D:/mingw64/bin/gcc.exe"

[ -x "$GCC" ] || GCC=gcc

mkdir -p "$OUT"
rm -f "$EXE" "$OUT/stepper_host.log"

echo "=== 1/2 编译 ==="
"$GCC" -std=c99 -O0 -g -Wall -Wextra \
  -I"$HERE/host/fake" \
  -I"$HERE/host" \
  -I"$ROOT/AVE" \
  -o "$EXE" \
  "$HERE/host/test_stepper.c" \
  "$ROOT/AVE/AVE_stepper.c" > "$OUT/stepper_host.log" 2>&1
RC=$?
cat "$OUT/stepper_host.log"

if [ "$RC" -ne 0 ] || [ ! -f "$EXE" ]; then
  echo ">>> 编译【失败】—— 不跑旧二进制，直接判负。"
  exit 1
fi
echo "编译通过，产物：$EXE"

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
