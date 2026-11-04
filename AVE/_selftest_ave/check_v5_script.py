# -*- coding: utf-8 -*-
"""
check_v5_script.py -- V5 交付脚本的静态判据（规则 2 自测）
==========================================================
被测物：AVE/maixpy/v5_send.py（MaixCAM 侧 MaixPy 脚本）

为什么只能静态检查？
  我跑不了 MaixPy，也没有 MaixCAM —— `from maix import ...` 在桌面上必然 ImportError。
  所以本脚本【不 import 被测脚本】，只做两件事：
    ① py_compile 语法检查（能抓出语法错，且不会执行 import）
    ② AST 结构检查，把 plan.md §6.3 十条里【能在源码层面判定】的几条钉死

本脚本能证明什么 / 不能证明什么（诚实边界，别当成已覆盖）：
  ✅ 能证明：脚本语法可编译；行格式串以 \\n 结尾；写串口只有一处且不在 if 分支里；
             无目标分支确实赋 -1,-1,0；中心坐标是"左上角 + 半个宽高"；波特率 115200；
             正常取值下行长 ≤32；存在运行时行长守卫。
  ⬜ 不能证明：它真的能在 MaixCAM 上跑起来、真的出框、真的发到 PB7、FPS 达标。
             那几条要上硬件（串口助手 / STM32 [VS] 行），见 verify.md。

用法：
  python check_v5_script.py               # 检查真实脚本
  python check_v5_script.py --mutate M1   # 变异测试：改坏一处，判据【必须】失败
"""
import ast
import io
import os
import py_compile
import sys
import tempfile
import tokenize

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "..", "maixpy", "v5_send.py")

results = []   # (检查名, 是否通过, 说明)


def code_only(src):
    """去掉注释与所有字符串字面量后的源码。

    🔴 这一步是必需的，而且是被变异测试逼出来的：第一版检查直接在整份源文件上做
    `"o.x + o.w / 2" in src`，而**本文件头部的注释里恰好写着同样的字样** ⇒
    把代码里的 `o.x + o.w / 2` 改掉、注释没动 ⇒ 判据照样通过（变异体 M3/M6 存活）。
    即：判据被**注释**满足了，而注释不是被测物。
    ⇒ 教训与 §4.3 同款：**观测量的语义必须追到赋值链，判据必须落在被测物上。**
    """
    out = []
    try:
        toks = list(tokenize.generate_tokens(io.StringIO(src).readline))
    except tokenize.TokenError:
        return src
    for tok in toks:
        if tok.type in (tokenize.COMMENT, tokenize.NL, tokenize.INDENT,
                        tokenize.DEDENT, tokenize.NEWLINE):
            continue
        if tok.type == tokenize.STRING:
            out.append('""')      # 占位：保留语法形状，抹掉内容
        else:
            out.append(tok.string)
    return " ".join(out)


def chk(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def find_call(node, funcname):
    """在 node 的子节点里找 funcname(...) 调用语句（含表达式语句）。"""
    out = []
    for n in ast.walk(node):
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) \
                and n.func.attr == funcname:
            out.append(n)
    return out


def const_int(node):
    """把语法节点还原成整数常量。

    🔴 这里踩过一次坑：Python 把 `-1` 解析成 UnaryOp(USub, Constant(1))，
    不是 Constant(-1)。只判 Constant 会让 `-1,-1,0` 整条判据【恒假】——
    即"看起来在检查，其实永远不通过"，正是本项目反复吃过的假判据。
    """
    if isinstance(node, ast.Constant) and isinstance(node.value, int) \
            and not isinstance(node.value, bool):
        return node.value
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        inner = const_int(node.operand)
        return None if inner is None else -inner
    return None


def run_checks(src, filename):
    del results[:]

    # ---- ① 语法可编译（不执行 import）----
    try:
        compile(src, filename, "exec")
        chk("syntax", True)
    except SyntaxError as e:
        chk("syntax", False, "%s" % e)
        return

    tree = ast.parse(src)

    # ---- ② 顶层必须有 uart.UART(...) 且波特率 115200（§6.3 第 5 条）----
    uart_ctor = [c for c in find_call(tree, "UART")]
    chk("uart.UART exists", len(uart_ctor) == 1,
        "found %d" % len(uart_ctor))
    baud_ok = False
    if uart_ctor:
        args = uart_ctor[0].args
        # 允许第二参数是字面量 115200，或一个名为 BAUD 的常量
        if len(args) >= 2 and isinstance(args[1], ast.Constant):
            baud_ok = (args[1].value == 115200)
        else:
            baud_ok = "BAUD = 115200" in src
    chk("baud == 115200", baud_ok)

    # ---- ③ while 主循环 ----
    whiles = [n for n in tree.body if isinstance(n, ast.While)]
    chk("one module-level while", len(whiles) == 1, "found %d" % len(whiles))
    if not whiles:
        return
    loop = whiles[0]
    body = loop.body

    # ---- ④ ser.write 恰好一处，且是 while 的直接子语句（§6.3 第 3 条）----
    writes = []
    for n in ast.walk(loop):
        if isinstance(n, ast.Expr) and isinstance(n.value, ast.Call) \
                and isinstance(n.value.func, ast.Attribute) \
                and n.value.func.attr == "write":
            writes.append(n)
    chk("exactly one ser.write", len(writes) == 1, "found %d" % len(writes))

    # ---- ⑤ if objs: 的 else 分支必须赋 -1,-1,0（§6.3 第 3/8 条）----
    if_objs = [n for n in body
               if isinstance(n, ast.If) and ast.dump(n.test).find("objs") >= 0]
    chk("if objs: in while body", len(if_objs) >= 1, "found %d" % len(if_objs))
    else_ok = False
    if if_objs:
        for stmt in if_objs[0].orelse:
            if isinstance(stmt, ast.Assign):
                tg = stmt.targets[0]
                if isinstance(tg, ast.Tuple) and len(tg.elts) == 3:
                    vals = ([const_int(e) for e in stmt.value.elts]
                            if isinstance(stmt.value, ast.Tuple) else [])
                    if vals == [-1, -1, 0]:
                        else_ok = True
    chk("no-target branch = -1,-1,0", else_ok)

    # ---- ④b ser.write 必须在 if objs: 块【之外】（§6.3 第 3 条的真意：
    #        发送不能以"这一帧有目标"为前提；放在别的小 if 里无所谓）----
    write_line = writes[0].lineno if len(writes) == 1 else None
    in_objs_block = False
    if write_line is not None and if_objs:
        st = if_objs[0]
        end = max([getattr(b, "lineno", 0) for b in st.body] or [st.lineno])
        in_objs_block = (st.lineno <= write_line <= end)
    chk("ser.write outside the 'if objs' block",
        write_line is not None and not in_objs_block,
        "write_line=%s in_objs_block=%s" % (write_line, in_objs_block))

    # ---- ⑥ 中心 = 左上角 + 半个宽高（§6.3 第 1 条）----
    #    🔴 必须看【代码】不看注释（code_only），否则头部注释里的同款字样会把判据喂饱。
    code = code_only(src)
    flat = code.replace(" ", "")
    chk("center uses x+w/2 and y+h/2",
        "o.x+o.w/2" in flat and "o.y+o.h/2" in flat)

    # ---- ⑦ 行格式串必须以 \n 结尾（§6.3 第 4 条）----
    #    ⚠️ 不限定"赋给变量 line"：用户实跑版把格式串直接内联在 write 的参数里
    #    （`ser.write(("%d,%d,%d\n" % (cx, cy, flag)).encode())`），没有中间变量。
    fmt = None
    for n in ast.walk(tree):
        if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Mod) \
                and isinstance(n.left, ast.Constant) and isinstance(n.left.value, str):
            fmt = n.left.value
    chk("line format found", fmt is not None, "fmt=%r" % (fmt,))
    if fmt is not None:
        chk("line ends with \\n", fmt.endswith("\n"), "fmt=%r" % (fmt,))
        # ---- ⑧ 正常取值下行长 ≤32（§6.3 第 10 条）----
        worst = 0
        for cx in (-1, 0, 112, 223, 255):
            for cy in (-1, 0, 112, 223, 255):
                for flag in (0, 1):
                    worst = max(worst, len((fmt % (cx, cy, flag)).encode()))
        chk("worst-case line <= 32B", worst <= 32, "worst=%d" % worst)

    # ---- ⑨ 行长必须有界：钳位坐标 或 运行时守卫，至少要有一个（§6.3 第 10 条）----
    #    钳位（min(cx, W-1)）给的是【静态上界】；运行时守卫是兜底。两者有其一即成立，
    #    但"两个都没有"就得报 —— 否则坐标跑飞时发超长行，两端看起来都像成功。
    bounded = (("LINE_MAX" in code and "len(line)" in code)
               or ("W - 1" in code and "H - 1" in code))
    chk("line length bounded (clamp to W-1/H-1 or runtime guard)", bounded)

    # ---- ⑩ flag 不能取 -1（§6.3 第 8 条）----
    bad_flag = False
    for n in ast.walk(tree):
        if isinstance(n, ast.Assign):
            tg = n.targets[0]
            if isinstance(tg, ast.Name) and tg.id == "flag":
                v = const_int(n.value)
                if v is not None and v not in (0, 1):
                    bad_flag = True
            if isinstance(tg, ast.Tuple):
                for t, v in zip(tg.elts, getattr(n.value, "elts", [])):
                    if isinstance(t, ast.Name) and t.id == "flag":
                        iv = const_int(v)
                        if iv is not None and iv not in (0, 1):
                            bad_flag = True
    chk("flag is 0 or 1 only", not bad_flag)


# 变异表：把源码改坏一处，判据【必须】失败（"判据必须能失败，否则它不是判据"）
MUTATIONS = {
    # 规则③：往 if len(objs) > 0: 里再塞一个发送（= 首版脚本的原病）
    "M1": ("        o = max(objs, key=lambda x: x.score)\n",
           "        o = max(objs, key=lambda x: x.score)\n        ser.write(b\"x\")\n",
           "exactly one ser.write"),
    # 规则④：行尾少了 \n
    "M2": ('"%d,%d,%d\\n"', '"%d,%d,%d"', "line ends with \\n"),
    # 规则①：把左上角当中心
    "M3": ("int(o.x + o.w / 2)", "int(o.x)", "center uses x+w/2 and y+h/2"),
    # 规则⑧：拿 -1 当 flag
    "M4": ("min(int(o.y + o.h / 2), H - 1), 1", "min(int(o.y + o.h / 2), H - 1), -1",
           "flag is 0 or 1 only"),
    # 规则③：无目标分支不赋合法值
    "M5": ("cx, cy, flag = -1, -1, 0", "cx, cy, flag = -1, -1, -1",
           "no-target branch = -1,-1,0"),
    # 规则⑩：拆掉钳位 —— 坐标不再有上界
    "M6": ("min(int(o.x + o.w / 2), W - 1)", "int(o.x + o.w / 2)",
           "line length bounded (clamp to W-1/H-1 or runtime guard)"),
}


def main():
    with open(TARGET, "r", encoding="utf-8") as f:
        src = f.read()

    if len(sys.argv) >= 3 and sys.argv[1] == "--mutate":
        name = sys.argv[2]
        old, new, expect = MUTATIONS[name]
        if old not in src:
            print("MUTATION-ANCHOR-MISSING %s" % name)
            return 3
        mutated = src.replace(old, new, 1)
        run_checks(mutated, "<mutant %s>" % name)
        failed = [n for (n, ok, d) in results if not ok]
        if expect in failed:
            print("MUTANT %s CAUGHT by '%s'  (other failures: %s)"
                  % (name, expect, [f for f in failed if f != expect]))
            return 0
        print("MUTANT %s SURVIVED  -- expected '%s' to fail; failures=%s"
              % (name, expect, failed))
        return 1

    # ---- 真实源码 ----
    try:
        fd, tmp = tempfile.mkstemp(suffix=".py")
        os.close(fd)
        py_compile.compile(TARGET, cfile=tmp, doraise=True)
        print("[py_compile] OK (syntax only; 'from maix import ...' NOT executed)")
        os.remove(tmp)
    except Exception as e:
        print("[py_compile] FAIL: %s" % e)

    run_checks(src, TARGET)

    nfail = 0
    for name, ok, detail in results:
        tag = "PASS" if ok else "FAIL"
        if not ok:
            nfail += 1
        extra = ("  <- %s" % detail) if (detail and not ok) else ""
        print("  [%s] %s%s" % (tag, name, extra))
    print("RESULT: %d passed / %d failed" % (len(results) - nfail, nfail))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
