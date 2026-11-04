#!/usr/bin/env bash
# ============================================================================
# board_check.py 的【变异测试】—— 证明它的自测真的判得错
#
# ── 为什么需要这个文件 ──────────────────────────────────────────────────────
#   `board_check.py --selftest` 报"9/9 + 8/8 全绿"。
#   **全绿本身不是证据** —— 一个永远报绿的判据和一个不可能失败的判据，
#   在输出上长得一模一样。本工程为此栽过一次（V11 把计数填成正好等于封顶值，
#   去掉封顶的变异体安然全过）。
#
#   所以这里故意把 board_check.py 弄坏三处，逐一确认【它的自测真的会红】。
#   坏的地方全部落在 **run_board() 的拆行逻辑**上 —— 也就是真上板跑的那一段；
#   `analyze()` 一个字节都不动（它由那 9 份伪造回显守着，是另一半）。
#
# ── 三处变异 ────────────────────────────────────────────────────────────────
#   B1  buf = data（不跨 read 攒半行）→ 半行被丢，拼不回整行
#   B2  不剥行尾 '\r'                → 抓包文本带 '\r'，正则吃不下
#   B3  空行不过滤                   → 空行混进 records，污染判定窗口
#
# ── 硬约束 ──────────────────────────────────────────────────────────────────
#   🔴 每一步都要先确认【变异真的生效】。改错地方 = 用例"通过" =
#      给出"这个判据拦不住"的**错误结论**。本工程已犯过两次
#      （见 memory: armcc-negative-test-matching）。
#   🔴 **匹配一律用 `grep -F`（定长串）**。判据里全是 `[HB]` / `[VS]` 这种方括号，
#      走正则会被当成**字符类** —— `[HB]` 变成"H 或 B"，于是随便一行都能匹配上。
#      本脚本第一版正是这么错的：变异 B1 明明被拦住了，却被报成"红的不是预期那条"。
#      **判据必须取自输出原文，且不许经过正则这层翻译。**
#   🔴 用完就删临时文件，绝不把变异体留在盘上被人当成"最新版"。
# ============================================================================

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/board_check.py"
TMP="$HERE/build/bcmut"

# 先在盘上确认这三个待变异片段各出现且只出现一次 —— 否则 sed 会改错或什么都改不到
for pat in 'buf += data' 'errors="replace").strip()' 'if line:'; do
  n=$(grep -c -F -- "$pat" "$SRC")
  if [ "$n" != "1" ]; then
    echo "🔴 待变异片段 '$pat' 在 board_check.py 里出现 $n 次（期望 1 次）。"
    echo "   源码改过了 —— 先更新本脚本的变异表达式，再跑。"
    exit 9
  fi
done

rm -rf "$TMP"
mkdir -p "$TMP"

pass=0
total=0
bad=0

mut_run () {
  name="$1"; shift
  expr="$1"; shift
  must_contain="$1"; shift
  expect_check="$1"; shift
  total=$((total + 1))

  echo
  echo "──────────────────────────────────────────────────────────────────────"
  echo "变异 $name ：$*"
  echo "──────────────────────────────────────────────────────────────────────"

  cp "$SRC" "$TMP/$name.py"
  sed -i "$expr" "$TMP/$name.py"

  # 🔴 第一步：变异真的生效了吗？
  if grep -q -F -- "$must_contain" "$TMP/$name.py"; then
    echo "🔴 变异没生效 —— 源码里出现了本该被改掉的内容 '$must_contain'。"
    echo "   此时跑出来的『测试仍然全过』【不能】推出『判据拦不住』，"
    echo "   只能推出『我改错了地方』。"
    bad=$((bad + 1))
    return
  fi

  out=$(python "$TMP/$name.py" --selftest 2>&1)
  rc=$?

  if [ "$rc" -eq 0 ]; then
    echo "🔴 变异后自测【仍然全绿】—— 这条判据拦不住 $name。"
    echo "   要么是对应用例太弱，要么是这条性质根本没被断言。"
    bad=$((bad + 1))
    return
  fi

  # 必须是【主循环那半】红的，不能是 analyze 那半 —— 那半我们没动过。
  if ! printf '%s\n' "$out" | grep -q -F "主循环自测：" ; then
    echo "🔴 自测红了，但不是主循环那半 —— 变异跑到别的地方去了。"
    printf '%s\n' "$out" | tail -20 | sed 's/^/      /'
    bad=$((bad + 1))
    return
  fi

  # 两段式：先取出所有 [BAD] 行，再在这些行里找片段。
  # 不能写成 `grep -F "[BAD] $expect_check"` —— 片段往往在**行中间**
  # （如 `[BAD] 被切成两半的 [HB] 拼回【一条】完整行`），拼上前缀就永远匹配不到。
  # 本脚本第二版正是这么错的，于是"已拦住"被报成"红的不是预期那条"。
  badlines=$(printf '%s\n' "$out" | grep -F "[BAD]")
  if printf '%s\n' "$badlines" | grep -q -F "$expect_check"; then
    echo "  ✓ 被预期断言拦住："
    printf '%s\n' "$badlines" | grep -F "$expect_check" | sed 's/^/      /'
    printf '%s\n' "$out" | grep -F "主循环自测：" | sed 's/^/      /'
    pass=$((pass + 1))
  else
    echo "🔴 自测红了，但红的【不是】预期的那条（期望含 '$expect_check'）。"
    printf '%s\n' "$badlines" | sed 's/^/      /'
    bad=$((bad + 1))
  fi
}

#   判据串一律取**不含方括号、不含反斜杠**的片段 —— 见文件头那条硬约束。
#                       名  sed 表达式                                 变异后应消失的串            期望变红的断言
mut_run B1 's/buf += data/buf = data/'                              'buf += data'               '拼回【一条】'
mut_run B2 's/errors="replace").strip()/errors="replace")/'          'errors="replace").strip()'  '被剥掉'
mut_run B3 's/if line: /if True: /'                                  'if line: '                 '没有空行混进来'

echo
echo "══════════════════════════════════════════════════════════════════════"
if [ "$bad" -eq 0 ]; then
  echo "变异测试：$pass/$total 全部被预期断言拦住 ✅"
  echo "结论：board_check.py 的注入/抓包自测【判得对，也判得错】，"
  echo "      可以拿它的结果去判真板子。"
  rm -rf "$TMP"
  exit 0
fi
echo "🔴 有 $bad 项不符合预期（通过 $pass/$total）"
echo "   ⚠️ 先分清两种可能：『变异没生效』与『用例太弱』—— 上面已经分开报了。"
echo "   变异体保留在 $TMP 供查看。"
exit 1
