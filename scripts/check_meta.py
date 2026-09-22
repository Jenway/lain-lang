#!/usr/bin/env python3
"""Meta 层的验收：手写 LAINIR 的初代 Meta 把源码降级成 LAINIR。

    python scripts/check_meta.py                 # 跑用例表
    python scripts/check_meta.py snapshot OUT    # 全部 .lain 语料的 stdout+退出码快照
    python scripts/check_meta.py compare A B     # 两份快照逐条对比（改动前后用）

为什么要有它：`docs/implementation/meta-parser-plan.md` §6 定的验收是「改动前后各编一份
二进制、全部 `.lain` 语料逐条比」，但那套一直只有手工步骤，脚本不在版本库里。而且
「全语料一致」只证明**没破坏已有行为**，证明不了**新行为对** —— 2026-09-21 修掉的两个
静默错值，就是在 60/60 一致的前提下活着的（语料里没有一条走到那两条路）。所以这里两件
事都要：**用例表**（真实期望值 + 拒绝码 + 产物文本断言）＋ **全语料快照对比**。

前置：`python scripts/build.py`（产物 `build/meta_boot[.exe]`）。
退出码：0 通过 / 1 有失败 / 2 缺前置。
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build" / ("meta_boot.exe" if os.name == "nt" else "meta_boot")
MANIFEST = "bootstrap/SOURCE_ORDER"
PRELUDE = "std/prelude.lain=bootstrap/lain/std/prelude.lain"
# 这几行计数随代码量变（不是行为），逐条对比时滤掉。
COUNTERS = re.compile(r"^(bootstrap|caps|meta|scratch):")

# --- 用例表 -----------------------------------------------------------------
#
# kind = "ok"     期望 exit 0（驱动里的期望值是真断言）
# kind = "reject" 期望 exit != 0；给了 `code` 就同时比诊断码
# `text`          必须出现在**产出的 LAINIR** 里的片段（可以给多条）
#
# 语料文件（`seed/tests/*.lain`）放「值/形状」类，改起来要能一眼看懂；
# 内联的放小回归，不单独占文件。
CASES = [
    # --- 语料文件 ---
    dict(name="body_chain", kind="ok", src="seed/tests/meta_body_chain.lain",
         entry="main", want=12),
    dict(name="nested_args", kind="ok", src="seed/tests/meta_nested_args.lain",
         entry="main", want=9),
    dict(name="let_call", kind="ok", src="seed/tests/meta_let_call.lain",
         entry="main", want=2),
    # 编译期求值的调用：v0 的标记是 `eval` 前缀（**不是** `#eval`，`#` 是注释）。
    # 产物里必须出现 `#eval g(1)`，而且折叠阶段要把它算成常量（跑出来 2）。
    dict(name="eval_call", kind="ok", src="seed/tests/meta_eval_call.lain",
         entry="main", want=2, text=["#eval g(1)"]),
    # `eval` 的实参必须是**编译期已知**的：传参数 → 验证器 2012
    dict(name="reject_eval_unknown_arg", kind="reject", code=2012,
         src="func g(x: i32) -> i32 { return x; }\n"
             "func f(a: i32) -> i32 { let y: i32 = eval g(a); return y; }\n"
             "let main: i32 = f(1);",
         entry="main"),
    # 名字叫 `eval` 的普通函数不能被标记吃掉：括号调用还是普通调用
    dict(name="eval_as_function_name", kind="ok",
         src="func eval(x: i32) -> i32 { return x + 1; }\n"
             "func f() -> i32 { let y: i32 = eval(1); return y; }\n"
             "let main: i32 = f();",
         entry="main", want=2, text=["#call eval(1)"]),
    # `eval g(1)` 混在更长的表达式里本期不支持：必须干净拒，不能静默算错
    dict(name="reject_eval_mixed", kind="reject", code=4,
         src="func g(x: i32) -> i32 { return x + 1; }\n"
             "func f() -> i32 { let y: i32 = 1 + eval g(1); return y; }\n"
             "let main: i32 = f();",
         entry="main"),
    # 返回位置（`return eval g(1);`）本期没做：干净地报 4，不发半截文本
    dict(name="reject_eval_return_pos", kind="reject", code=4,
         src="func g(x: i32) -> i32 { return x + 1; }\n"
             "func f() -> i32 { return eval g(1); }\n"
             "let main: i32 = f();",
         entry="main"),
    dict(name="cmp_param", kind="ok", src="seed/tests/meta_cmp_param.lain",
         entry="main", want=1,
         text=["#slt[#bits<32>](%x, 2)", "#ult[#bits<32>](%x, 2)"]),
    # --- 内联小回归 ---
    dict(name="op_after_paren", kind="ok", src="let main: i32 = 12 / (2 + 1);",
         entry="main", want=4, text=["#sdiv[#bits<32>]"]),
    dict(name="chain_top", kind="ok", src="let main: i32 = 1 + 2 + 3;",
         entry="main", want=6),
    dict(name="paren_nested", kind="ok", src="let main: i32 = 1 + (2 + 3);",
         entry="main", want=6),
    dict(name="call_then_op", kind="ok",
         src="func g(x: i32) -> i32 { return x; }\n"
             "func f() -> i32 { return g(1) + 2; }\n"
             "let main: i32 = f();",
         entry="main", want=3),
    # --- 反例：必须干净地拒（本仓库最看重的方向）---
    dict(name="reject_trailing_op", kind="reject", code=3,
         src="let main: i32 = 1 + 2 + 3 + ;", entry="main"),
    dict(name="reject_hex", kind="reject", code=4,
         src="let main: i32 = 0x10;", entry="main"),
    # 比较结果是 1 位，装不进 i32 —— 这一条在「字面量默认 i32」做完之后**仍须拒**
    dict(name="reject_cmp_as_i32", kind="reject",
         src="let a: i32 = 1 < 2;\nlet main: i32 = 1;", entry="main"),
    # --- 字面量自己的类型（参考经典 C）+ 装不下就拒 ---
    # `1`、`2` 自己就是 i32，不需要上下文；结果 1 位，绑 bool 正好。
    dict(name="cmp_both_literals", kind="ok",
         src="let b: bool = 1 < 2;\nlet main: i32 = 1;", entry="main", want=1,
         text=["#slt[#bits<32>](1, 2)"]),
    # 同一个东西，但直接问**值**：`1 < 2` 得是真（1）。发成 `#slt[#bits<1>](1, 2)`
    # 的话 `2` 被截成 0、结果是假 —— 曾经就是这样，验证器不报。
    # 这个文件在 `meta_conformance/` 里名字还叫 `reject_*`：那是历史（以前确实拒）。
    dict(name="cmp_literal_value", kind="ok",
         src="seed/tests/meta_conformance/reject_cmp_as_value.lain",
         entry="b", want=1),
    dict(name="literal_i8_max", kind="ok", src="let main: i8 = 255;",
         entry="main", want=255),
    dict(name="literal_i64", kind="ok", src="let main: i64 = 5;",
         entry="main", want=5),
    # 越界必须拒（曾经静默截断：300 -> 44、2 -> 0）。三条走的是三个不同的发射点：
    # 顶层常量、函数体的 return、算子的操作数。前两条报 23（字面量装不进声明的类型）；
    # 第三条报验证器的 **2019** —— i8 的 op 表是空的，于是按字面量自己的类型 i32 算，
    # 32 位的结果对不上 bits<8> 的返回类型。理由更准，所以按 2019 钉。
    dict(name="reject_literal_i8", kind="reject", code=23,
         src="let main: i8 = 300;", entry="main"),
    dict(name="reject_literal_bool2", kind="reject", code=23,
         src="let main: bool = 2;", entry="main"),
    dict(name="reject_literal_in_chain", kind="reject", code=2019,
         src="let main: i8 = 1 + 300;", entry="main"),
    dict(name="reject_literal_return", kind="reject", code=23,
         src="func f() -> i8 { return 300; }\nlet main: i32 = 1;", entry="main"),
]

# 还没做的东西，只报现状、不算通过失败（做完就该挪进 CASES）。现在空着。
PENDING: list[dict] = []


def module_args() -> list[str]:
    """prelude + `seed/tests/modules/**` 全部按相对路径注册（import 语料）。"""
    out = [PRELUDE]
    base = ROOT / "seed" / "tests" / "modules"
    for p in sorted(base.rglob("*.lain")):
        out.append(f"{p.relative_to(base).as_posix()}={p.relative_to(ROOT).as_posix()}")
    return out


def run(src: str, entry: str, want: int) -> tuple[int, str]:
    env = dict(os.environ, LAIN_META_MANIFEST=MANIFEST)
    cmd = [str(BIN), src, entry, str(want), "", "-", *module_args()]
    done = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return done.returncode, done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")


def diag_code(text: str):
    """从驱动的输出里把诊断码捞出来。

    先只看 FAIL 行：`meta:` 那行计数里带着 `host status 0`，先匹配它的话，一个被**验证器**
    拒掉的用例会显示成「拒码 0」（显示的错，断言没错）。计数行本来也不该参与解析。
    """
    lines = [ln for ln in text.splitlines() if not COUNTERS.match(ln)]
    pools = ([ln for ln in lines if "FAIL" in ln], lines)
    for pool in pools:
        for pat in (r"Meta returned status (\d+)", r"host status (\d+)", r"verify: (\d+)"):
            for line in pool:
                m = re.search(pat, line)
                if m:
                    return int(m.group(1))
    return None


def inline_path(name: str, src: str) -> str:
    tmp = ROOT / "build" / "tmp-probe"
    tmp.mkdir(parents=True, exist_ok=True)
    path = tmp / f"check_{name}.lain"
    path.write_text(src + "\n", encoding="ascii")
    return path.relative_to(ROOT).as_posix()


def check() -> int:
    if not BIN.is_file():
        print(f"缺前置：{BIN.relative_to(ROOT)} 不存在，先跑 python scripts/build.py")
        return 2
    bad = 0
    for case in CASES:
        src = case["src"]
        if not src.endswith(".lain"):
            src = inline_path(case["name"], src)
        code, out = run(src, case["entry"], case.get("want", 0))
        why = ""
        if case["kind"] == "ok":
            if code != 0:
                why = f"期望通过，实际 exit={code}（码 {diag_code(out)}）"
            for want_text in case.get("text", []):
                if want_text not in out:
                    why = why or f"产出的 LAINIR 里没有 `{want_text}`"
        else:
            if code == 0:
                why = "期望被拒，实际通过了"
            elif case.get("code") is not None and diag_code(out) != case["code"]:
                why = f"期望码 {case['code']}，实际 {diag_code(out)}"
        status = "PASS" if not why else "FAIL"
        detail = why if why else (f"exit={code}" if case["kind"] == "ok" else f"拒码 {diag_code(out)}")
        print(f"{status}  {case['name']:<22} {detail}")
        if why:
            bad += 1
    print(f"--- 用例：{len(CASES) - bad}/{len(CASES)} 通过 ---")

    if PENDING:
        print("--- 还没做的（只报现状，不算失败）---")
        for case in PENDING:
            src = inline_path("pending_" + case["name"], case["src"])
            code, out = run(src, case["entry"], case["want"])
            print(f"      {case['name']:<22} exit={code} 码 {diag_code(out)} —— {case['note']}")
    return 1 if bad else 0


# --- 全语料快照 / 对比（改动前后用）------------------------------------------

def fixtures() -> list[Path]:
    return sorted((ROOT / "seed" / "tests").rglob("*.lain"))


def snapshot(out_path: str) -> int:
    if not BIN.is_file():
        print(f"缺前置：{BIN.relative_to(ROOT)} 不存在，先跑 python scripts/build.py")
        return 2
    data = {}
    for path in fixtures():
        rel = path.relative_to(ROOT).as_posix()
        code, out = run(rel, "main", 0)
        kept = [ln for ln in out.splitlines() if not COUNTERS.match(ln)]
        data[rel] = {"exit": code, "stdout": "\n".join(kept)}
    Path(out_path).write_text(json.dumps(data, ensure_ascii=False, indent=1),
                              encoding="utf-8")
    print(f"snapshot: {len(data)} 个用例 -> {out_path}")
    return 0


def compare(a_path: str, b_path: str) -> int:
    left = json.loads(Path(a_path).read_text(encoding="utf-8"))
    right = json.loads(Path(b_path).read_text(encoding="utf-8"))
    keys = sorted(set(left) | set(right))
    diff = [k for k in keys if left.get(k) != right.get(k)]
    print(f"逐条对比：{len(keys) - len(diff)}/{len(keys)} 逐字节一致")
    for key in diff:
        la, lb = left.get(key), right.get(key)
        print(f"=== 不同：{key}")
        print(f"    exit {la and la['exit']} -> {lb and lb['exit']}")
        ta = (la or {}).get("stdout", "").splitlines()
        tb = (lb or {}).get("stdout", "").splitlines()
        shown = 0
        for i in range(max(len(ta), len(tb))):
            x = ta[i] if i < len(ta) else "<无>"
            y = tb[i] if i < len(tb) else "<无>"
            if x != y:
                print(f"    - {x}\n    + {y}")
                shown += 1
                if shown >= 12:
                    print("    ...（只印前 12 处）")
                    break
    return 0 if not diff else 1


def main() -> int:
    argv = sys.argv[1:]
    if not argv:
        return check()
    what = argv[0]
    if what == "check":
        return check()
    if what == "snapshot" and len(argv) >= 2:
        return snapshot(argv[1])
    if what == "compare" and len(argv) >= 3:
        return compare(argv[1], argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
