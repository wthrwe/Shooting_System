#!/bin/sh
#
# 跨端契约测试 —— 构建 + 运行
#
# 两件事：
#   1. 把固件侧回放程序 `ave_vision_replay.c` 与**产品代码原样**链接成 `replay.exe`
#      （与 `run_runtime.sh` 同一套仿真 HAL / 同一份宿主 shim，不复制、不改写产品码）；
#   2. 交给 `test_contract_vision.py` —— 它跑 `maixpy/v5_send.py` **本体**拿到
#      真实字节，再驱动 `replay.exe`，两侧各自独立判分。
#
# ── 为什么这里**不**做变异测试 ──────────────────────────────────────────────
#   本测试的被测对象是**两侧的接缝**，不是某一个函数。能改坏解析器的那些点
#   （范围校验、只取最新、位数上限、行宽封顶……）已经由 `run_runtime.sh` 的
#   M9~M14 逐条钉住；在这里再复制一套只会更慢，不会多证明什么。
#   ⚠️ 这是**判断**，不是省事：将来若真出现"只有跨端才会错"的那类错法
#      （例如两侧对 flag 的取值域理解不一致），再补跨端变异。
#
# ── 环境 ────────────────────────────────────────────────────────────────────
#   GCC 路径**不能带空格**（脚本里是 `$GCC ...`，虽然本轮已加引号，
#   但保持"不要空格"这条约定能少一类坑）。`PROGRA~1` 是 Windows 8.3 短名。
#   Python 需要 3.x；可用 PYTHON=... 覆盖。
#
# 临时验证脚手架，不进版本库、不进 Keil 工程。

set -u

HERE=/e/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave
PROJ=/e/FREERTOS/18-视觉跟踪二维打靶系统/AVE/AVE
HOST=$HERE/host
BUILD=$HERE/build

GCC=${GCC:-/c/PROGRA~1/RedPanda-Cpp/MinGW64/bin/gcc.exe}
PYTHON=${PYTHON:-python}
CFLAGS="-std=c99 -Wall -Wextra -O1"

if [ ! -x "$GCC" ]; then
  echo "找不到宿主编译器：$GCC"
  echo "（可用 GCC=/路径/gcc 覆盖；注意路径不要带空格）"
  exit 2
fi

mkdir -p "$BUILD"

# 🔴 先删旧 exe：编译失败时旧文件还在的话，下面的 `[ -x ]` 照样通过，
#    脚本会**拿上一轮的二进制跑出满屏绿字**。`run_runtime.sh` 里为此留过痕。
rm -f "$BUILD/replay.exe"

echo "=============================================================="
echo " 1. 构建回放程序（产品代码一个字节都没改）"
echo "=============================================================="
"$GCC" $CFLAGS -I"$HOST" -I"$PROJ" -o "$BUILD/replay.exe" \
     "$HERE/ave_vision_replay.c" "$PROJ/AVE_uart.c" "$PROJ/AVE_board.c" \
     "$PROJ/AVE_vision.c" "$HOST/fake_hal.c" "$HOST/host_trace.c"
if [ ! -x "$BUILD/replay.exe" ]; then
  echo "→ 编译失败（上面是编译器输出）"
  exit 3
fi
echo "→ ok"

echo "=============================================================="
echo " 2. 跨端契约测试"
echo "=============================================================="
cd "$HERE" || exit 2
"$PYTHON" "$HERE/test_contract_vision.py"
rc=$?
echo "=============================================================="
if [ $rc -eq 0 ]; then
  echo " 结论：跨端契约**通过**（视觉端的真实字节被固件端全部收下、零投诉；"
  echo "       越界帧被丢弃且未失去同步）"
else
  echo " 结论：跨端契约**失败**（exit=$rc）——上面 FAIL 那几行就是判据"
fi
echo "=============================================================="
exit $rc
