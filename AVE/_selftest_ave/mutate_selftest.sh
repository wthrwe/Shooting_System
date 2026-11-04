#!/bin/sh
# =============================================================================
# test_stepper.c 的 T8.c 循环（s_bad_yaw[] / s_bad_pitch[]）**判别力**复核
#
# 为什么要单独量：T8.c 的判据是「弄坏一样 → 断言 bad[axis] **恰好等于**那一位」。
#   这条在**基线已经脏**的时候会失效 —— 第四十九次的 M8 就撞上了：基线已带
#   CHK_OCMODE，再"弄坏"一次值不变 ⇒ 那条用例反而"通过"。
#   所以不能只凭"它平时是绿的"就说它有判别力。
#
# 做法：把每条 break_* 帮手**改成空操作**，看对应的 T8.P<i>/T8.T<i> 是否**真的红**。
#   空操作 ⇒ 这一项不再被弄坏 ⇒ bad[axis] 应为 0 ≠ 那一位 ⇒ 该用例**必修**变红。
#     红了 = 对这条破口**有判别力**；全绿 = 用例是摆设（假绿）。
# 另外②：把某轴的 break 帮手**顺手也弄坏另一轴** ⇒ 验「└ 另一轴不受影响」是否真会红。
# ③ **反向**检查（最要紧的一条）：把变异打进**产品源**让基线变脏 ⇒ 问"绝对值相等"
#   这条判据还认不认得出。预期是**认不出**（per-case 假绿），但 T8.9/T8.10 会同时红。
#
# 🔴 首轮实测又翻出一个坑（本脚本自己的，不是被测代码的）：
#   `cp: cannot create regular file ...: Device or resource busy` —— 刚被 gcc 读过的
#   源文件会被 Windows 杀软/索引器短暂占住，那次 cp 静默失败 ⇒ 下一个变异是**叠在
#   上一个变异体上**跑的，"这条用例红了"可能是上一条的功劳，而日志里只多一行 cp 报错。
#   对策：hard_cp 重试 20 次 + 每次搬完 `cmp` 断言真的落到基线 + 结尾做字节级还原比对。
# 🔴 第二轮又翻出**更阴的一条**，而且根因是**本脚本自己造的**：
#   我把临时文件名变量起成了 `TMP` —— 而 Windows 本来就导出 `TMP`/`TEMP` 指向临时目录，
#   sh 里给"**已导出**的变量"赋值**不会**去掉导出属性 ⇒ 子进程 gcc.exe 继承
#   `TMP=<某个文件路径>`，mingw gcc 拿 `TMP` 当**临时目录**用 ⇒
#       Cannot create temporary file in ...\test_stepper.c.tmp\: No such file or directory
#   ⇒ 13 个变异"齐刷刷编译不过"、连干净的基线都被说成"源码是红的"。**全是假象**。
#   改叫 `SRCTMP` 即好。教训：**在 sh 里别用 TMP / TEMP / TMPDIR / PATH / HOME 当自己的变量名**。
#   附带发现（保留）：`run_test_stepper.sh:32` 用 `RC≠0 || ! -f exe` 判负 ——
#   gcc 因任何 I/O 原因写不出产物时，它**也**打印 `>>> 编译【失败】`，把人往"源码坏了"上带。
#   所以这里按 **gcc 有没有吐 `error:`** 分流（见 run_suite）：没有 ⇒ I/O 问题，整轮作废。
#
# 结束一定还原（trap EXIT/INT/TERM）。控制台输出 UTF-8。
# =============================================================================
set -u

HERE="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/_selftest_ave"
SRC="$HERE/host/test_stepper.c"
SRCTMP="$HERE/out/test_stepper.c.tmp"
BAK="$HERE/out/test_stepper.c.orig"
RUN="$HERE/run_test_stepper.sh"
LOG="$HERE/out/mutself.log"
EVLOG="$HERE/out/mutself_envfail.log"
# ③ 段要动**产品源**（复现 M8"基线已脏"那一幕），所以单独再备一份
PSRC="E:/FREERTOS/18-视觉跟踪二维打靶系统/AVE/AVE/AVE_stepper.c"
PBAK="$HERE/out/AVE_stepper.c.orig_self"
TOUCHED_P=0

bad=0

# 🔴 首轮实测翻出来的坑：`cp: cannot create regular file ...: Device or resource busy`
#   —— 刚被 gcc 读过的源文件会被 Windows 的杀软/索引器**短暂占住**，这一次 cp 就静默失败。
#   后果极毒：`mut` 开头的"回到基线"没做成 ⇒ 下一个变异是**叠在上一个变异体上**跑的，
#   于是"这条用例红了"可能是上一条的功劳。日志里只多一行 cp 报错，很容易看漏。
#   对策两条：① 重试 20 次；② **每次 cp 完立刻 cmp 断言真的回到基线**，对不上就作废本轮。
retry_cp()
{
  i=0
  while [ "$i" -lt 20 ]; do
    if cp "$1" "$2" 2>/dev/null; then return 0; fi
    i=$((i + 1))
    sleep 1
  done
  return 1
}

hard_cp()
{
  retry_cp "$1" "$2" && return 0
  echo "!!! cp 被占住：$1 -> $2（重试 20 次仍失败）—— 本轮结论不可信"
  bad=1
  exit 1
}

# 还原走 retry_cp：trap 里不能再调 hard_cp（它 exit 会再触发 trap）
restore()
{
  retry_cp "$BAK" "$SRC" || echo "!!! 还原失败 —— 测试源还停在变异体上，去查 $SRC"
  # 产品源只在③段动过时才还原（$PBAK 存在且已标记）
  if [ "$TOUCHED_P" -eq 1 ]; then
    retry_cp "$PBAK" "$PSRC" || echo "!!! 还原失败 —— **产品源**还停在变异体上，去查 $PSRC"
  fi
}

hard_cp "$SRC" "$BAK"
trap 'restore' EXIT INT TERM

# 🔴 第二轮实测又翻出一个**误诊**（比第一轮的 cp 还阴）：
#   `run_test_stepper.sh:32` 的判负条件是 `RC≠0 **或** exe 不存在` —— 也就是说
#   **gcc 写不出 exe（被占住）时，它照样打印 `>>> 编译【失败】`**。
#   于是这里会把"环境占文件"读成"变异体编译不过"，甚至把干净的基线报成"源码是红的"。
#   （症状：13 个变异**齐刷刷**全报编译不过、且一条 gcc 诊断都没有 ⇒ 那不是源码问题）
#   分开的办法：看 gcc 到底有没有吐 `error:`。
#     · 有 error:      → 真·编译错误，变异/源码确实坏了        → 返回 1
#     · 没有 error:    → I/O 层就写不出文件（被占住 / 临时目录不对），
#                        重试 6 次；仍不行                   → 返回 2（整轮作废）
run_suite()
{
  t=0
  while [ "$t" -lt 6 ]; do
    sh "$RUN" > "$LOG" 2>&1
    RUNC=$?
    if ! grep -q "编译【失败】" "$LOG"; then return 0; fi
    if grep -q "error:" "$LOG"; then return 1; fi
    t=$((t + 1))
    cp "$LOG" "$EVLOG" 2>/dev/null      # 留证据：环境失败那一刻的原始日志
    sleep 2
  done
  return 2
}

# 字面替换（awk index/substr，不含元字符语义 —— 同 mutate_stepper.sh 那条教训）
# apply_mut <干净的副本> <要改的文件>   —— 参数化是为了③段能改**产品源**
apply_mut()
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
  ' "$1" > "$SRCTMP"
  # 先写临时文件再搬过去：直接 `> "$SRC"` 一旦被占住会**把源文件截成空**，
  # 那比没改还糟（下一个变异体是从空文件上跑）。临时文件落在 out/，不碰 host/。
  #
  # 🔴🔴 这个变量**千万不能叫 `TMP`** —— Windows 本来就导出了 `TMP`/`TEMP` 指向临时目录，
  #   而 sh 里给"已导出的变量"赋值**不会**去掉导出属性 ⇒ 子进程 gcc.exe 会继承
  #   `TMP=<这里这个文件路径>`，mingw gcc 拿 `TMP` 当**临时目录**用 ⇒ 每次编译都报
  #       Cannot create temporary file in ...\out\test_stepper.c.tmp\: No such file or directory
  #   ⇒ 13 个变异齐刷刷"编译不过"、基线也"源码是红的"，全是**自己造的**假象。
  hard_cp "$SRCTMP" "$2"
}

# mut <编号> <被替换的语句> <替换成> <预期变红的用例串>
mut()
{
  name="$1"; from="$2"; to="$3"; expect="$4"

  hard_cp "$BAK" "$SRC"

  # 起点必须是干净的基线，否则"这条用例红了"可能是**上一条变异**的功劳。
  if ! cmp -s "$BAK" "$SRC"; then
    echo "[$name] 起点没回到基线（cp 没成功）—— 本轮结论作废"
    bad=1
    echo
    return
  fi

  n=$(grep -c -F "$from" "$SRC")
  if [ "$n" -ne 1 ]; then
    echo "[$name] 锚点匹配 $n 次（要求恰好 1 次）—— **变异无效**，结论作废"
    bad=1
    echo
    return
  fi

  apply_mut "$BAK" "$SRC"

  if cmp -s "$BAK" "$SRC"; then
    echo "[$name] 改完和原文一模一样 —— 变异没生效，结论作废"
    bad=1
    echo
    return
  fi

  run_suite
  rcs=$?
  rc=$RUNC

  if [ "$rcs" -eq 2 ]; then
    echo "[$name] **编译在 I/O 层就失败了**（连试 6 次写不出文件）—— 整轮结论作废，别信"
    echo "        原始日志留证：$EVLOG"
    echo "        gcc 那行怎么说："
    tail -3 "$HERE/out/stepper_host.log" | sed 's/^/        · /'
    bad=1
    exit 1                              # trap 会把源码还原
  fi
  if [ "$rcs" -eq 1 ]; then
    echo "[$name] 变异体**编译不过**（gcc 真报了 error）—— 变异本身无效，结论作废"
    grep -E "error:" "$LOG" | head -3
    bad=1
    echo
    return
  fi

  fails=$(grep -c "  FAIL" "$LOG")
  hit=$(grep "  FAIL" "$LOG" | grep -c "$expect")

  echo "[$name]  退出码 $rc，FAIL 条数 $fails"
  grep "  FAIL" "$LOG" | head -3 | sed 's/^/        · /'
  if [ "$rc" -eq 0 ]; then
    echo "        ✗ 测试**全绿** —— 这条破口没人管，用例是摆设"
    bad=1
  elif [ "$hit" -ge 1 ]; then
    echo "        ✓ 对应用例 $expect 真的红了（有判别力）"
  else
    echo "        ⚠ 红了，但**不是** $expect —— 抓到的是别的"
    bad=1
  fi
  echo
}

echo "=============================================================="
echo " 基线：未变异必须全绿（否则后面结论无意义）"
echo "=============================================================="
hard_cp "$BAK" "$SRC"
run_suite
rcs=$?
if [ "$rcs" -eq 2 ]; then
  echo "**环境占住了产物文件**（基线都编不出来）—— 先关掉占文件的进程再跑，本轮到此为止"
  bad=1
  exit 1
fi
if [ "$rcs" -eq 1 ]; then
  echo "基线源码**真的编不过**（gcc 报了 error）—— 先修源码，本轮到此为止"
  bad=1
  exit 1
fi
if [ "$RUNC" -eq 0 ]; then
  echo "基线全绿 ✓  ($(grep -o '通过 [0-9]* 项' "$LOG" | head -1))"
else
  echo "基线就是红的 —— 先修基线"
  bad=1
fi
echo

echo "--- ① YAW 的 8 条：break_* 改空操作 ⇒ T8.P<i> 必须红 ---"
mut "空操作 yaw-arpe"   "fake_TIM5.CR1  &= ~TIM_CR1_ARPE;"        "(void)0;"  "T8.P1"
mut "空操作 yaw-cms"    "fake_TIM5.CR1  |= TIM_CR1_CMS;"          "(void)0;"  "T8.P2"
mut "空操作 yaw-ocmode" "fake_TIM5.CCMR2 = (fake_TIM5.CCMR2 & ~TIM_CCMR2_OC4M) | (0x7U << TIM_CCMR2_OC4M_Pos);" "(void)0;" "T8.P3"
mut "空操作 yaw-ocpe"   "fake_TIM5.CCMR2 &= ~TIM_CCMR2_OC4PE;"    "(void)0;"  "T8.P4"
mut "空操作 yaw-ccs"    "fake_TIM5.CCMR2 |= TIM_CCMR2_CC4S;"      "(void)0;"  "T8.P5"
mut "空操作 yaw-pol"    "fake_TIM5.CCER  &= ~TIM_CCER_CC4P;"      "(void)0;"  "T8.P6"
mut "空操作 yaw-cce"    "fake_TIM5.CCER  &= ~TIM_CCER_CC4E;"      "(void)0;"  "T8.P7"
mut "空操作 yaw-psc"    "fake_TIM5.PSC    = 98U;"                 "(void)0;"  "T8.P8"

echo "--- ① PITCH 的 3 条：break_* 改空操作 ⇒ T8.T<i> 必须红 ---"
mut "空操作 pitch-ocmode" "fake_TIM2.CCMR2 = (fake_TIM2.CCMR2 & ~TIM_CCMR2_OC3M) | (0x7U << TIM_CCMR2_OC3M_Pos);" "(void)0;" "T8.T1"
mut "空操作 pitch-ocpe"   "fake_TIM2.CCMR2 &= ~TIM_CCMR2_OC3PE;"  "(void)0;"  "T8.T2"
mut "空操作 pitch-pol"    "fake_TIM2.CCER  &= ~TIM_CCER_CC3P;"    "(void)0;"  "T8.T3"

echo "--- ② 跨轴护栏：break 帮手顺手也弄坏**另一轴** ⇒ 「└ 不受影响」必须红 ---"
mut "yaw-pol 顺手弄坏 PITCH" \
    "fake_TIM5.CCER  &= ~TIM_CCER_CC4P;" \
    "fake_TIM5.CCER  &= ~TIM_CCER_CC4P; fake_TIM2.CCER &= ~TIM_CCER_CC3P;" \
    "PITCH 不受影响"
mut "pitch-pol 顺手弄坏 YAW" \
    "fake_TIM2.CCER  &= ~TIM_CCER_CC3P;" \
    "fake_TIM2.CCER  &= ~TIM_CCER_CC3P; fake_TIM5.CCER &= ~TIM_CCER_CC4P;" \
    "YAW 不受影响"

echo "--- ③ 反向检查：**基线已经脏**时会怎样（复现第四十九次的 M8）---"
# ①②只证明"破口有人管"。这一条问相反的问题：
#   如果 **产品源本身**已经坏了（基线脏），那条"恰好等于某一位"的断言还认得出来吗？
#   预期（据第四十九次的实测）：**认不出来** —— T8.T1 会**假绿**。
#   但同一轮里 T8.10（PITCH bad == 0）必然红 ⇒ 整套的结论仍然是对的，
#   假绿的只是"哪条抓到的"这个**标签**。这一条就是要把这个边界钉死。
hard_cp "$PSRC" "$PBAK"
TOUCHED_P=1
from="= TIM_CCMR2_OC3M_Pos,"
to="= TIM_CCMR2_OC4M_Pos,"
n=$(grep -c -F "$from" "$PSRC")
if [ "$n" -ne 1 ]; then
  echo "产品源锚点匹配 $n 次（要求 1）—— ③段作废"; bad=1
else
  apply_mut "$PBAK" "$PSRC"
  if cmp -s "$PBAK" "$PSRC"; then
    echo "产品源没改动 —— ③段作废"; bad=1
  else
    run_suite; rcs=$?
    if [ "$rcs" -ne 0 ]; then
      echo "③段编译/环境异常（$rcs）—— 作废"; bad=1
    else
      echo "        脏基线下这几条的判定（PITCH 那三条是关键）："
      grep -E "T8\.(9|10|11|12|T1|T2|T3|P1|P3) " "$LOG" | sed 's/^/        /'
      dirty_fail=$(grep "  FAIL" "$LOG" | grep -c "T8.10")
      t1_pass=$(grep "  PASS" "$LOG" | grep -c "T8.T1")
      if [ "$dirty_fail" -ge 1 ]; then
        echo "        ✓ 安全网在：T8.10（PITCH bad == 0）**红了** ⇒ 这一整套没被脏基线蒙过"
      else
        echo "        ✗ T8.10 竟然没红 —— 脏基线能骗过整套，**这是真问题**"
        bad=1
      fi
      if [ "$t1_pass" -ge 1 ]; then
        echo "        ⬜ 已知局限（如实记）：T8.T1 **通过**了 —— 它的断言是绝对值，"
        echo "           分不出'我刚弄坏的'和'本来就坏的' ⇒ 单看这一条会误以为 PITCH.OCMODE 没事"
      else
        echo "        （本轮 T8.T1 是红的 —— 与第四十九次的记录不同，值得复查）"
      fi
    fi
  fi
fi
retry_cp "$PBAK" "$PSRC" && echo "        产品源已还原" || { echo "!!! 产品源没还原干净"; bad=1; }
TOUCHED_P=0
echo

echo "=============================================================="
restore
echo "源码已还原。先做**字节级**比对，再重跑一遍确认回到全绿："
if cmp -s "$BAK" "$SRC"; then
  echo "还原比对 ✓  与备份逐字节相同"
else
  echo "!!! 还原后与备份**不一致** —— 立刻去查 $SRC"; bad=1
fi
run_suite
rcs=$?
if [ "$rcs" -eq 0 ] && [ "$RUNC" -eq 0 ]; then
  echo "还原后全绿 ✓  ($(grep -o '通过 [0-9]* 项' "$LOG" | head -1))"
else
  echo "!!! 还原后不是绿的（环境占文件也算）—— 去查 $SRC，另有日志 $LOG"
  bad=1
fi
echo "=============================================================="
[ "$bad" -eq 0 ] && echo ">>> 判别力复核【通过】：每条破口都能被对应用例抓到" \
                 || echo ">>> 判别力复核【有问题】，见上面标 ✗ / ⚠ / 作废 的行"
exit "$bad"
