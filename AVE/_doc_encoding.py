#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AVE 四份中文文档的「重编码」检测 / 无损修复工具。

背景：本工程的四份 .md 会被某个外部进程整体按 OEM 代码页重编码（文件仍是合法
UTF-8，`file` 看不出异常，内容也没丢，只是字符变了）。已实测 7 次。

用法（在工程根 AVE/ 下跑）：
    python _doc_encoding.py check     # 只检测，退出码 1 = 有损坏
    python _doc_encoding.py repair    # 检测 + 按已知代码页无损修复（每份先备份）

判据为什么用 Unicode 区段而不是枚举字符集：
    早期版本手工枚举了 48 个「CP737 特征字符」，**漏了带音标的希腊字母**
    （`ή` U+03AE、`ί`、`ς`…）⇒ 有静默漏报的风险。区段版既更宽也更短。
    ⚠️ 故意**不含制表符区段** `─-╿`：文档里的 ASCII 流程图
    （`┌────┴────┐`）会让它大面积误报；乱码里的制表符是夹在字母之间的，
    靠字母区段就抓得到。
    ⚠️ 用「连续 ≥2」排除两类别无必要的误报：`Δ`（当增量符号用的单字符）、
    以及**故意记录乱码样例**的那几行。
"""
import io
import os
import re
import shutil
import sys

DOCS = ["plan.md", "verify.md", "ROADMAP.md", "temporary.md"]

# 实测配对：同一个文件每次都坏成同一个代码页（verify.md 已 3 次 CP737、
# ROADMAP.md 已 3 次 CP855）。先试这一张表，再走全表探测。
KNOWN = {"verify.md": "cp737", "ROADMAP.md": "cp855"}
CODECS = ["cp737", "cp855", "cp866", "cp437", "cp850", "cp869", "cp862", "cp1251"]

RANGE = re.compile(r"[Ͱ-ϿЀ-ӿ]{2,}")


def read_lines(path):
    return io.open(path, encoding="utf-8").read().split("\n")


def bad_lines(lines):
    return [i + 1 for i, l in enumerate(lines) if RANGE.search(l)]


def probe(lines, cp):
    """返回 (还原出中文的行数, 抛异常的行数)。正确代码页 ⇒ 前者 > 0 且后者 = 0。

    ⚠️ 别用「能无损往返的行数」判代码页：纯 ASCII 行对任何代码页都能往返，
    各代码页会给出完全相同的数字，那个指标毫无判别力。
    """
    ok = fail = 0
    for l in lines:
        try:
            r = b"".join(c.encode(cp) for c in l).decode("utf-8")
            if any('一' <= ch <= '鿿' for ch in r):
                ok += 1
        except Exception:
            fail += 1
    return ok, fail


def pick_codec(name, lines):
    order = ([KNOWN[name]] if name in KNOWN else []) + CODECS
    for cp in dict.fromkeys(order):          # 去重且保序
        ok, fail = probe(lines, cp)
        if ok > 0 and fail == 0:
            return cp, ok
    return None, 0


def do_check(quiet=False):
    total = 0
    for f in DOCS:
        if not os.path.exists(f):
            continue
        lines = read_lines(f)
        bad = bad_lines(lines)
        total += len(bad)
        if not quiet or bad:
            print("  %-14s lines=%-4d corrupt=%-3d %s"
                  % (f, len(lines) - 1, len(bad), bad[:4]))
    return total


def do_repair():
    for f in DOCS:
        if not os.path.exists(f):
            continue
        lines = read_lines(f)
        bad = bad_lines(lines)
        if not bad:
            print("  %-14s OK（%d 行，0 损坏）" % (f, len(lines) - 1))
            continue
        cp, _ = pick_codec(f, lines)
        if cp is None:
            print("  %-14s 🔴 探测不出代码页，跳过（先别手改）" % f)
            continue
        # 先备份再动手：备份后缀递增，不覆盖旧备份
        n = 1
        while os.path.exists("%s.bak%d" % (f, n)):
            n += 1
        bak = "%s.bak%d" % (f, n)
        shutil.copyfile(f, bak)
        out, fixed = [], 0
        for l in lines:
            try:
                r = b"".join(c.encode(cp) for c in l).decode("utf-8")
                if r != l:
                    fixed += 1
                out.append(r)
            except Exception:
                out.append(l)            # 失败行原样保留，别把能救的一起毁掉
        io.open(f, "w", encoding="utf-8", newline="\n").write("\n".join(out))
        print("  %-14s 用 %s 修复 %d 行（备份 %s），行数 %d"
              % (f, cp, fixed, os.path.basename(bak), len(out) - 1))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    print("== %s ==" % mode)
    if mode == "repair":
        do_repair()
        print("== 修复后复查 ==")
        print("TOTAL corrupt =", do_check())
    else:
        total = do_check()
        print("TOTAL corrupt =", total)
        sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
