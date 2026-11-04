#!/bin/sh
# =============================================================================
# AVE_stepper.c 单文件编译检查
#
# 用 Keil 自带的 armcc（ARM Compiler 5），选项与 MDK-ARM/AVE.uvprojx 对齐：
#   <uC99>1</uC99>  -> --c99
#   <Optim>3</Optim> -> -O3
#   <Define>USE_HAL_DRIVER,STM32F407xx</Define>
#   包含路径抄自 uvprojx:336
#
# 🔴 先删旧产物：编译**失败**时若留着上次的 .o，"有没有产物"就区分不出成败，
#    等于拿假绿骗自己。（这条是踩过的坑，见 memory: verify-script-must-delete-stale-binary）
# =============================================================================
set -u

ROOT="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE"
ARMCC="D:/Keil_v5/ARM/ARMCC/bin/armcc.exe"
FROMELF="D:/Keil_v5/ARM/ARMCC/bin/fromelf.exe"
OUTDIR="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave/out"
OBJ="$OUTDIR/AVE_stepper.o"

mkdir -p "$OUTDIR"
rm -f "$OBJ" "$OUTDIR/AVE_stepper.log"

"$ARMCC" --c99 -O3 --cpu=Cortex-M4.fp --apcs=interwork -c \
  -DUSE_HAL_DRIVER -DSTM32F407xx \
  -I"$ROOT/Core/Inc" \
  -I"$ROOT/Drivers/STM32F4xx_HAL_Driver/Inc" \
  -I"$ROOT/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy" \
  -I"$ROOT/Middlewares/Third_Party/FreeRTOS/Source/include" \
  -I"$ROOT/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2" \
  -I"$ROOT/Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F" \
  -I"$ROOT/Drivers/CMSIS/Device/ST/STM32F4xx/Include" \
  -I"$ROOT/Drivers/CMSIS/Include" \
  -I"$ROOT/AVE" \
  -o "$OBJ" \
  "$ROOT/AVE/AVE_stepper.c" > "$OUTDIR/AVE_stepper.log" 2>&1
RC=$?

echo "=== armcc rc=$RC ==="
cat "$OUTDIR/AVE_stepper.log"

if [ "$RC" -ne 0 ]; then
  echo ">>> 判定：编译【失败】。没有产物，不许拿旧 .o 当证据。"
  exit "$RC"
fi
if [ ! -f "$OBJ" ]; then
  echo ">>> 判定：armcc 返回 0 但没生成 .o —— 异常，按失败处理。"
  exit 1
fi

echo "=== 产物 ==="
ls -l "$OBJ"
echo "=== 已定义的符号（只看 AVE_Stepper* / HAL_TIM_PWM_PulseFinishedCallback）==="
"$FROMELF" --text -s "$OBJ" 2>/dev/null | grep -E "AVE_Stepper|PulseFinished" || echo "(没匹配到 —— 见下面的全量符号前 40 行)"
"$FROMELF" --text -s "$OBJ" 2>/dev/null | head -40
echo ">>> 判定：编译【通过】。"
