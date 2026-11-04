#!/bin/sh
# 自测：AVE_VISION_TR_LOG_PERIOD_MS（第七十四次新增，[TR] 独立周期）
#
# 验什么：
#   1) 默认值 2000 下，AVE_tuning.h 整套编译期断言全部通过（正向基线）；
#   2) 三条新增断言**真的会开火**：50 / 3000 / 130 各一个变异体，逐条必须报出
#      对应的那条 #error 文案 —— 只判"编译失败"不够，别的错也会让它失败；
#   3) 变量没接错线：AVE_app.c 里 [TR] 的闸门用新宏、[VS] 的闸门仍用旧宏；
#   4) 已有的 [TR] 行宽测试仍然通过（改宏不该动行宽）。
#
# 为什么要变异测试：断言从没开过火 = 等于没写。第七十四次之前已经有两次
# "守卫其实坏了、但测试报绿"的前科。

GCC="/c/PROGRA~1/RedPanda-Cpp/MinGW64/bin/gcc.exe"
[ -x "$GCC" ] || { echo ">>> 找不到 gcc: $GCC"; exit 1; }

TMP="_selftest_ave/_trp"
rm -rf "$TMP"; mkdir -p "$TMP"
cp AVE/AVE_tuning.h "$TMP/AVE_tuning.h"
printf '#include "AVE_tuning.h"\nint main(void){return (int)AVE_VISION_TR_LOG_PERIOD_MS;}\n' > "$TMP/t.c"

pass=0; fail=0
ok()   { echo "PASS  $1"; pass=$((pass+1)); }
bad()  { echo "FAIL  $1"; fail=$((fail+1)); }

# ---- 1) 正向基线 ----
"$GCC" -I"$TMP" -fsyntax-only "$TMP/t.c" >"$TMP/pos.log" 2>&1
if [ $? -eq 0 ]; then ok "基线：默认 2000 编译通过"; else
  bad "基线：默认 2000 编译通过"; sed -n '1,10p' "$TMP/pos.log"; fi

# ---- 2) 三条断言逐条开火 ----
mutate() {  # $1=新值  $2=期望文案片段  $3=标签
  sed -i "s/^#define AVE_VISION_TR_LOG_PERIOD_MS .*/#define AVE_VISION_TR_LOG_PERIOD_MS     $1/" \
      "$TMP/AVE_tuning.h"
  "$GCC" -I"$TMP" -fsyntax-only "$TMP/t.c" >"$TMP/m.log" 2>&1
  rc=$?
  hit=$(grep -c "$2" "$TMP/m.log")
  if [ $rc -ne 0 ] && [ "$hit" -ge 1 ]; then ok "$3（$1 ⇒ 断言开火）"; else
    bad "$3（$1 ⇒ rc=$rc hit=$hit）"; sed -n '1,6p' "$TMP/m.log"; fi
  cp AVE/AVE_tuning.h "$TMP/AVE_tuning.h"   # 还原，避免变异叠加
}

mutate 50   "不能小于 100 ms"                "下界"
mutate 3000 "不能大于 \[VS\] 周期"            "上界"
mutate 130  "必须.*整数倍"                    "非整倍"

# ---- 3) 接线检查 ----
n1=$(grep -c "last_ms) < AVE_VISION_TR_LOG_PERIOD_MS)" AVE/AVE_app.c)
n2=$(grep -c "s_vs_last_log_ms) < AVE_VISION_VS_LOG_PERIOD_MS)" AVE/AVE_app.c)
[ "$n1" -eq 1 ] && ok "[TR] 闸门用新宏（1 处）" || bad "[TR] 闸门用新宏（实测 $n1 处）"
[ "$n2" -eq 1 ] && ok "[VS] 闸门仍用旧宏（1 处）" || bad "[VS] 闸门仍用旧宏（实测 $n2 处）"

# ---- 4) 行号没被顶掉（文档按行号引用本文件）----
l1=$(grep -n "^#define AVE_VISION_TRACK_MAX_RPM" AVE/AVE_tuning.h | cut -d: -f1)
l2=$(grep -n "^#define AVE_VISION_YAW_SIGN"      AVE/AVE_tuning.h | cut -d: -f1)
[ "$l1" = "143" ] && ok "AVE_tuning.h:143 未被顶掉" || bad "AVE_tuning.h:143 → $l1"
[ "$l2" = "172" ] && ok "AVE_tuning.h:172 未被顶掉" || bad "AVE_tuning.h:172 → $l2"

# ---- 5) 既有 [TR] 行宽测试 ----
if [ -x "_selftest_ave/test_tr_line.exe" ]; then
  if _selftest_ave/test_tr_line.exe >"$TMP/tr.log" 2>&1; then
    ok "test_tr_line 仍通过（行宽未变）"
  else
    bad "test_tr_line 失败"; sed -n '1,15p' "$TMP/tr.log"
  fi
else
  echo "SKIP  test_tr_line.exe 不存在"
fi

echo "----"
echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]
