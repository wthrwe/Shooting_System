#!/bin/sh
# =============================================================================
# AVE_stepper.c 变异测试
#
# 为什么必须做：一套「119 项全过」的测试，如果它**永远不会失败**，那它什么都没证明。
#   做法是把源码改坏（变异），看测试是否**真的红**，并且红在**预期的那一条**上。
#   两个都要查：
#     · 测试没红        → 用例太弱（假绿）
#     · 锚点没匹配/改完没变 → **变异根本没生效**，那不是"测试没抓到"，是白做了
#   第二种最容易骗自己，所以下面每次都断言"文件内容确实变了"。
#   （见 memory: mutation-test-can-fail-silently）
#
# 🔴 第四十九次修了两处**脚本自身的**假绿：
#   ① `hit=$(grep -c "$expect" "$LOG")` 会把 **PASS 行**也算命中 ⇒ 全绿日志也能报"✓"；
#      改成 `grep "  FAIL" "$LOG" | grep -c "$expect"`（先筛 FAIL 行再数）。
#   ② "红在别处"（⚠）原来**不置 bad** ⇒ 汇总照样报"【通过】"，与它自己那句
#      "红在预期用例上"自相矛盾；现在 ⚠ 计入失败。
#   更早的锚点事故（sed 的 `/`、`&`）见下面 apply_mutation 的注释。
#
# 结束一定还原源码（trap EXIT）。
# =============================================================================
set -u

SRC="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/AVE/AVE_stepper.c"
HERE="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave"
BAK="$HERE/out/AVE_stepper.c.orig"
RUN="$HERE/run_test_stepper.sh"
LOG="$HERE/out/mutate.log"

cp "$SRC" "$BAK" || { echo "备份失败，中止"; exit 1; }
restore() { cp "$BAK" "$SRC"; }
trap 'restore' EXIT INT TERM

bad=0

# -----------------------------------------------------------------------------
# 把锚点**字面**替换掉，写回 $SRC。
#
# 🔴 刻意不用 sed。本轮真踩了这个坑：
#    · `sed "s/$from/$to/"` 里 `/` 是分隔符 —— 锚点里出现 `/` 直接报
#      `unknown option to 's'`（M5 就是这么废掉的）；
#    · 替换串里的 `&` 意思是"整个匹配" —— M10 的替换串含 `&`，生成的代码是
#      `if ((ccmr (ccmr & cfg->ccs_msk) != 0U cfg->ccs_msk) == 1U)`
#      ⇒ **编译失败** ⇒ 退出码 1 但 FAIL 条数 0 ⇒ 脚本一本正经地报"测试变红了"。
#      那不是产线代码的问题、也不是用例太弱，**是变异体自己坏了**。
#
# awk 的 index/substr 是纯字面匹配，不含任何元字符语义，从此没有这一类坑。
# -----------------------------------------------------------------------------
apply_mutation()
{
  awk -v from="$from" -v to="$to" '
    {
      line = $0; out = "";
      while ((p = index(line, from)) > 0)
      {
        out  = out substr(line, 1, p - 1) to;
        line = substr(line, p + length(from));
      }
      print out line;
    }
  ' "$BAK" > "$SRC"
}

# mutate <编号> <锚点> <替换> <预期该红的用例号>
mutate()
{
  name="$1"; from="$2"; to="$3"; expect="$4"

  cp "$BAK" "$SRC"

  n=$(grep -c -F "$from" "$SRC")
  if [ "$n" -ne 1 ]; then
    echo "[$name] 锚点匹配 $n 次（要求恰好 1 次）—— **变异无效**，结论作废"
    bad=1
    return
  fi

  apply_mutation

  if cmp -s "$BAK" "$SRC"; then
    echo "[$name] 改完和原文一模一样 —— **变异没生效**，结论作废"
    bad=1
    return
  fi

  sh "$RUN" > "$LOG" 2>&1
  rc=$?

  # 🔴 三态必须分开，否则会把"变异体坏了"读成"测出来了"：
  #    ① 编译不过      → 变异体无效，结论作废（M10 的 sed 事故就落在这里）
  #    ② 全绿          → 用例太弱，这个 bug 没人管
  #    ③ 红了但对不上 → 抓到的是别的问题
  if grep -q "编译【失败】" "$LOG"; then
    echo "[$name] 变异体**编译不过** —— 不是"用例没抓到"，是变异本身无效，结论作废"
    grep -E "error:|错误" "$LOG" | head -5
    bad=1
    echo
    return
  fi

  fails=$(grep -c "  FAIL" "$LOG")

  # 🔴 第四十九次修 —— 原写法 `hit=$(grep -c "$expect" "$LOG")` 是**假绿**：
  #    `grep -c` 数的是"**含该串的行**"，而 PASS 行同样含用例号 ⇒ 在一个 0 条 FAIL
  #    的**全绿**日志上，它照样能报"✓ 预期用例就在失败列表里"。
  #    实测证据（全绿的 out/mutate.log）：`grep -c "T8.9"` = 1、`grep -c "  FAIL"` = 0
  #    —— 那 1 次命中的正是 `PASS T8.9`。
  #    正确判据 = **先在 FAIL 行里筛，再数**：PASS 行永远进不来。
  hit=$(grep "  FAIL" "$LOG" | grep -c "$expect")

  echo "[$name] 注入：$from"
  echo "        → 退出码 $rc，FAIL 条数 $fails"
  # 无论如何都把头几条 FAIL 打出来：① 重定 expect 标签时可直接照抄；
  # ② 判"✓"也留痕 —— 免得以后又只凭一个字回信（这正是上面那处假绿的教训）。
  grep "  FAIL" "$LOG" | head -3 | sed 's/^/        · /'
  if [ "$rc" -eq 0 ]; then
    echo "        ✗ 测试**全绿**了 —— 这个 bug 没人管，用例太弱"
    bad=1
  elif [ "$hit" -ge 1 ]; then
    echo "        ✓ 测试变红，且预期用例 $expect 就在失败列表里"
  else
    # 🔴 第四十九次修：这里原来**没有** bad=1 ⇒ ⚠ 只是打印一行，汇总仍报"【通过】"。
    #    但 ⚠ 的含义正是"标签跟实际对不上"，是**脚本自己的**问题 ⇒ 必须计入失败。
    echo "        ⚠ 测试变红了，但**不是** $expect —— 抓到的是别的问题（**标签要重定**）"
    bad=1
  fi
  echo
}

echo "=============================================================="
echo " 变异测试开始（每个变异跑一整套 166 项）"
echo "=============================================================="
echo

echo "--- 基线：未变异必须全绿 ---"
cp "$BAK" "$SRC"
if sh "$RUN" > "$LOG" 2>&1; then
  echo "基线全绿 ✓  ($(grep -o '通过 [0-9]* 项' "$LOG" | head -1))"
else
  echo "基线就是红的 —— 后面的变异结论都没意义，先修基线"
  bad=1
fi
echo

# ---- M1：同向改频改成"重启计数器"（硬约束五，静默失效）----
mutate "M1 同向改频改为重启计数器" \
       "pwm_reprogram(axis, mag);" \
       "pwm_run(axis, mag, 0U);" \
       "T4.1"

# ---- M2：位置模式多走一步（比较中断 vs 更新中断那条约束的等价变异）----
mutate "M2 位置模式多走一步" \
       "if (st->remain <= 0)" \
       "if (st->remain < 0)" \
       "T3."

# ---- M3：门控默认开着（破坏 fail-safe）----
mutate "M3 上电门控默认开着" \
       "st->gate       = 0U;" \
       "st->gate       = 1U;" \
       "T1."

# ---- M4：走完 N 步后不停（脉冲无限跑）----
mutate "M4 走完不停脉冲" \
       "pwm_stop((AVE_StepperAxis)a);" \
       "(void)a;" \
       "T3."

# ---- M5..M14：专盯本轮新增的定时器配置自检（T8）----
# 这些变异的共同点：改坏之后**编译照过、板子上也不报错**，只是"结论悄悄变成错的"。

# ---- M5：应然 PSC 差一（分频整体偏一档，所有速度成比例错）----
mutate "M5 应然 PSC 少减 1" \
       "/ (uint32_t)AVE_STEPPER_TIMEBASE_HZ) - 1U);" \
       "/ (uint32_t)AVE_STEPPER_TIMEBASE_HZ));" \
       "T8.5"

# ---- M6：APB1 不分频时也乘 2（时钟推算的边界分支）----
mutate "M6 不分频时也乘 2" \
       "ppre1 < 4U" \
       "ppre1 < 0U" \
       "T8.15"

# ---- M7：APB1 分频了却不乘 2 ----
mutate "M7 分频了却不乘 2" \
       "ppre1 < 4U" \
       "ppre1 < 9U" \
       "T8.4"

# ---- M8：s_cfg[1]（PITCH）的 ocm_pos 写错 ----
# 🔴 第四十九次实测更正（旧标签 "T8.9"、旧描述 "YAW 去读 bit14:12" 都错）：
#   · 锚点 `= TIM_CCMR2_OC3M_Pos,` 只出现 1 次，在 **s_cfg[1]（PITCH）** 里 ⇒ 动的是 PITCH，不是 YAW；
#   · 期望标签改 `T8.10`（★★ PITCH bad = 0，实测 bad=2 = CHK_OCMODE）。
#   · ⚠️ 更该记的一条：**T8.12 没红**（它读原始 CCMR2 的 bit6:4，值仍然对）⇒
#     `ocm_pos` **只被 SelfTest 消费**（`AVE_stepper.c:726`），`Init` 写寄存器与它无关
#     ⇒ 本变异的后果是"**自检期望算错 ⇒ 误报**"，**不是**"配置真的漂了"。
#   · ⚠️ 而专为它写的 T8.T1（PITCH OCMODE 被弄坏）反而**通过** —— 基线已带 CHK_OCMODE，
#     再"弄坏"一次值不变 ⇒ 该用例对"基线已坏"的情形**没有判别力**（M9 的 T8.T3 同理）。
mutate "M8 PITCH 的 ocm_pos 写错（自检期望错位 ⇒ 误报）" \
       "= TIM_CCMR2_OC3M_Pos," \
       "= TIM_CCMR2_OC4M_Pos," \
       "T8.10"

# ---- M9：s_cfg[1]（PITCH）的极性掩码 ccxp 写错 ----
# 🔴 第四十九次实测更正：锚点同样在 **s_cfg[1]（PITCH）**；期望标签改 `T8.10`
#   （实测 bad=16 = CHK_POL）；专抓它的 T8.T3 反而通过（基线已带 CHK_POL，同 M8 的道理）。
mutate "M9 PITCH 的极性掩码 ccxp 写错（自检判据指向错通道）" \
       "= TIM_CCER_CC3P," \
       "= TIM_CCER_CC4P," \
       "T8.10"

# ---- M10：CCS 判据取反（通道被配成输入捕获时不再报警）----
mutate "M10 CCS 判据取反" \
       "(ccmr & cfg->ccs_msk) != 0U" \
       "(ccmr & cfg->ccs_msk) == 1U" \
       "T8.P5"

# ---- M11：自检永远返回 EOK（最危险的一种：报告全绿）----
mutate "M11 自检永远返回 EOK" \
       "(all == 0U) ? AVE_STEPPER_EOK : AVE_STEPPER_ECONFIG;" \
       "AVE_STEPPER_EOK;" \
       "T8.1"

# ---- M12：Init 不再补置 OCxPE（硬约束六，静默失效）----
# 🔴 第四十九次实测更正：旧标签 `T8.2`（Init **之前**的两位置检查）**永远抓不到它** ——
#   那是 sim 预置的状态、与 Init 无关。真正为该 bug 写的用例是 **T1.7 / T1.8**
#   （"YAW/PITCH 的 OCxPE 已由模块补上"）⇒ 改 `T1.7`。
mutate "M12 Init 不补置 OCxPE" \
       "inst->CCMR2 |= cfg->ocpe;" \
       "inst->CCMR2 |= 0U;" \
       "T1.7"

# ---- M13：Init 不打开 CCxE（引脚悬空当 40 kHz 脉冲线旁边的天线）----
# 🔴 第四十九次实测更正：同 M12，旧标签 `T8.2` 抓不到；真正对应的是 **T1.5 / T1.6**
#   （"YAW/PITCH 通道输出使能开着"）⇒ 改 `T1.5`。
mutate "M13 Init 不打开 CCxE" \
       "inst->CCER |= cfg->ccxe;" \
       "inst->CCER |= 0U;" \
       "T1.5"

# ---- M14：下溢护栏取反（时钟低于时间基准时绕成 0xFFFFFFFF）----
mutate "M14 下溢护栏取反" \
       "(clk < (uint32_t)AVE_STEPPER_TIMEBASE_HZ)" \
       "(clk >= (uint32_t)AVE_STEPPER_TIMEBASE_HZ)" \
       "T8.5"

echo "=============================================================="
restore
echo "源码已还原。重跑一遍确认回到全绿："
sh "$RUN" > "$LOG" 2>&1 && echo "还原后全绿 ✓  ($(grep -o '通过 [0-9]* 项' "$LOG" | head -1))" || {
  echo "!!! 还原后不是绿的 —— 源码可能没还原干净，去查 $SRC"; bad=1; }

echo "=============================================================="
[ "$bad" -eq 0 ] && echo ">>> 变异测试【通过】：测试确实会红，且红在预期用例上" \
                 || echo ">>> 变异测试【有问题】，见上面标 ✗ / ⚠ / 作废 的行"
exit "$bad"
