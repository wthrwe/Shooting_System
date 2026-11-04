#!/bin/sh
# =============================================================================
# AVE_gimbal.c 变异测试 —— 规则 2 的硬要求
#
# 【为什么必须做】
#   "测试全部通过"本身不是证据：一套什么都测不出来的测试也会全绿。
#   只有把被测代码**故意改坏**、确认测试**能红**，才证明测试真有判别力。
#
# 【怎么做的】
#   **绝不改动工程里的 AVE_gimbal.c**。每次把源码复制到 out/AVE_gimbal_mut.c，
#   在副本上 sed 一刀，再拿副本去跑套件。跑完删副本。
#
# 🔴 每刀之后先 `cmp` 一下：如果 sed 的模式没匹配上，副本和原件一模一样，
#    那"变异存活"其实是我的模式写错了 —— 必须当场报出来，
#    否则会拿一条假结论去找半天代码。
# =============================================================================
set -u

ROOT="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE"
HERE="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave"
OUT="$HERE/out"
MUT="$OUT/AVE_gimbal_mut.c"
ORIG="$ROOT/AVE/AVE_gimbal.c"
LOG="$OUT/gimbal_mutate.log"

mkdir -p "$OUT"

if [ ! -f "$ORIG" ]; then
  echo ">>> 找不到被测源码 $ORIG —— 路径又变了？先核对再跑。"
  exit 1
fi

TOTAL=0
CAUGHT=0

# run_mut <名字> <这是什么改动> <sed 表达式>
run_mut() {
  NAME="$1"; DESC="$2"; EXPR="$3"
  TOTAL=$((TOTAL + 1))

  cp "$ORIG" "$MUT"
  sed -i "$EXPR" "$MUT"

  if cmp -s "$MUT" "$ORIG"; then
    echo "  [模式失配] $NAME —— sed 没改动任何东西，这一刀无效。"
    echo "             表达式：$EXPR"
    echo "             ⇒ 按【未捕获】记账，但根因是我的模式，不是测试。"
    rm -f "$MUT"
    return
  fi

  # 先证明变异体本身还能编译 —— 编不过的变异体测不出任何东西
  sh "$HERE/run_test_gimbal.sh" "$MUT" > "$LOG" 2>&1
  RC=$?

  if grep -q ">>> 编译【失败】" "$LOG"; then
    echo "  [编不过]   $NAME —— 变异体编译失败，这一刀无效（不计入捕获）。"
    grep -m3 "error" "$LOG" | sed 's/^/             /'
    rm -f "$MUT"
    return
  fi

  if [ "$RC" -ne 0 ]; then
    CAUGHT=$((CAUGHT + 1))
    NF=$(grep -c "\[FAIL\]" "$LOG")
    echo "  [已捕获]   $NAME —— $DESC"
    echo "             测试红了 $NF 处，首条："
    grep -m1 "\[FAIL\]" "$LOG" | sed 's/^/             /'
  else
    echo "  [🟥 存活] $NAME —— $DESC"
    echo "             🔴 测试没红！说明这套测试【盖不住】这个改动，是测试的缺口。"
  fi

  rm -f "$MUT"
}

echo "=================================================================="
echo " 变异测试：每一刀改坏 AVE_gimbal.c 的一处，看套件能不能红"
echo " 被测源码：$ORIG"
echo "=================================================================="

# ---- 基准：原版必须绿，否则后面全部无意义 ----
echo
echo "=== 0. 基准（未变异）==="
sh "$HERE/run_test_gimbal.sh" > "$LOG" 2>&1
if [ $? -ne 0 ]; then
  echo ">>> 🟥 基准都没过，变异测试无从谈起。先看 $LOG"
  exit 1
fi
echo "  [基准绿]   原版通过 —— 下面每一刀才有意义。"

echo
echo "=== 1. 舍入 → 截断 ==="
# 90 度 → 800.01 脉冲：四舍五入 800，截断也是 800，太钝；用 1 度（8.889→9 vs 8）
run_mut "M1-舍入改截断" "1度应给9脉冲，截断后给8" \
  's/(int32_t)(f + 0.5f);/(int32_t)(f);/'

echo
echo "=== 2. 换算系数 ×6 → ×360 ==="
run_mut "M2-慢6倍" "60转/分应给3200Hz，错6倍后给19200" \
  's/rpm \* 6.0f \* steps_per_deg/rpm * 360.0f * steps_per_deg/'

echo
echo "=== 3. 拆掉「忙则让路」==="
run_mut "M3-仲裁失效" "忙时应EBUSY且不调后端，拆掉后会把定位打断" \
  's/AVE_Stepper_IsBusy(ax) != 0/0/g'

echo
echo "=== 4. 拆掉「空动作不打扰后端」==="
run_mut "M4-空动作下发" "0.05度应完全不调后端，拆掉后会白发一次调用" \
  's/if (delta == 0)/if (0)/'

echo
echo "=== 5. 放宽转速上限 ==="
run_mut "M5-上限形同虚设" "200.5转/分应ERANGE，放宽10倍后就放行了" \
  's/rpm <= AVE_GIMBAL_MAX_RPM/rpm <= (AVE_GIMBAL_MAX_RPM * 10.0f)/'

echo
echo "=== 6. 把「被门挡住」谎报成成功 ==="
# map_stepper 里 EGATE 改报 EOK ⇒ 薄壳以为后端受理了 ⇒ 会去累加脉冲。
# 这一刀同时打两处：状态码映射的完整性，以及"没受理就一个字都不许动"这条不变量。
run_mut "M6-失败当成功" "被门挡住时应EGATE且累计不动，改坏后会假报成功并记上脉冲" \
  's/return AVE_GIMBAL_EGATE;/return AVE_GIMBAL_EOK;/'

echo
echo "=================================================================="
echo " 结果：$CAUGHT / $TOTAL 刀被捕获"
if [ "$CAUGHT" -eq "$TOTAL" ]; then
  echo " >>> 全部捕获 —— 测试对每一处改动都有判别力。"
  exit 0
else
  echo " >>> 有存活变异 ⇒ 测试有缺口，见上面 [🟥 存活] 那几行。"
  exit 1
fi
