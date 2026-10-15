#!/usr/bin/env python3
"""第 4 步前置统计：bootstrap 单元里「Meta 的 import 导出不支持初值形态」的顶层静态 let。

规则不是从源码文本猜的，是**实测**出来的（见 docs/step4-import-exposure.md
的边界矩阵：模块只放一条 let、探针只声明 import 不取成员）。

Meta 的导出路径 = bootstrap/std/modules.lain 的 meta_lower_import ->
meta_emit_module_exports -> meta_emit_module_binding。后者只看初值**头节点**：

  头是数字字面量      -> 发一个字面量；头只是表达式一部分时其余被丢掉（静默错值）
  头是标识符          -> 去当前模块的作用域表里查：
                           查到 kind == 3（值绑定）  -> 递归发那条，再发 #call 转接
                           查到别的 kind（func/类型）-> lain_meta_fail(4)
                           查不到（内建名 / 未知名） -> lain_meta_fail(13)
  头是 '['（定长表）  -> 作用域扫描时已预置 emitted != 0，导出路径**整条跳过**：
                           不发任何过程（导入方拿 0；成员被引用时发悬空 #call）
  其余（'-'、'"' 等） -> 当成查不到的标识符 -> 13

而 meta_lower_import 在 import 声明处就会把被导入模块**全部**值绑定发一遍，
与它们有没有被引用无关。

用法： python scripts/import_export_scan.py [--tsv]
"""

import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOOT = ROOT / "bootstrap"
ORDER = BOOT / "SOURCE_ORDER"

DECL_RE = re.compile(r"^(let|func|extern|struct|enum)\s+([A-Za-z_]\w*)")
IDENT_RE = re.compile(r"^[A-Za-z_]\w*$")

# 头形态 -> (实测 status, 说明)
REACTION = {
    "digit": (0, "发字面量"),
    "ident-let": (0, "递归发同模块值绑定 + #call 转接"),
    "ident-func": (4, "头是 func：kind != 3"),
    "ident-type": (4, "头是 struct/enum：kind != 3"),
    "ident-unknown": (13, "头是查不到的标识符（内建名 / 未知名）"),
    "table": (0, "作用域扫描预置 emitted，导出路径整条跳过（不发过程）"),
    "other": (13, "头不是标识符也不是数字（'-' / '\"' / …）"),
}


def units():
    """SOURCE_ORDER 里的 28 个单元（相对 bootstrap/ 的 .lain 路径）。

    清单格式见 docs/plan-modules.md §3.3：每行 `<相对路径> [<根名> …]`，
    路径省略 `.lain` 后缀、`#` 起注释 —— 这里只看每行的第一段。
    """
    out = []
    for raw in ORDER.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        out.append(BOOT / (line.split()[0] + ".lain"))
    return out


def split_decls(text):
    """按「行首且列 0」切顶层声明：(kind, name, 行号, 声明全文)。"""
    lines = text.splitlines(keepends=True)
    starts = []
    for idx, ln in enumerate(lines):
        m = DECL_RE.match(ln)
        if m:
            starts.append((idx, m.group(1), m.group(2)))
    out = []
    for k, (idx, kind, name) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(lines)
        out.append((kind, name, idx + 1, "".join(lines[idx:end])))
    return out


def _scan(s, i, stop_at_semi):
    """从 i 起找深度 0 的 '='（stop_at_semi=False）或 ';'（True）；找不到回 -1。"""
    depth = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i += 1
            while i < n and s[i] != '"':
                i += 2 if s[i] == "\\" else 1
            i += 1
            continue
        if c == "/" and i + 1 < n and s[i + 1] == "/":
            while i < n and s[i] != "\n":
                i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "=" and depth == 0 and not stop_at_semi:
            return i
        elif c == ";" and depth == 0:
            return i
        i += 1
    return -1


def init_of(body):
    """等号后、深度 0 的 ';' 之前的初值文本；字符串与注释感知。"""
    eq = _scan(body, 0, False)
    if eq < 0:
        return None
    end = _scan(body, eq + 1, True)
    if end < 0:
        end = len(body)
    return re.sub(r"//[^\n]*", " ", body[eq + 1:end]).strip()


def head_of(init):
    """初值的**头节点**文本：标识符 / 数字 / 首个非空字符。"""
    if not init:
        return ""
    m = re.match(r"\d+", init)
    if m:
        return m.group(0)
    m = re.match(r"[A-Za-z_]\w*", init)
    if m:
        return m.group(0)
    return init[0]


def classify(init, local_kinds):
    """返回 REACTION 的键。local_kinds: 本单元顶层名 -> kind。"""
    if not init:
        return "other"
    if init.startswith("import("):
        return "other"      # import 声明：登记 kind 1，不在导出路径上
    if init[0] == "[":
        return "table"
    h = head_of(init)
    if h.isdigit():
        return "digit"
    if IDENT_RE.match(h):
        k = local_kinds.get(h)
        if k == "let":
            return "ident-let"
        if k == "func":
            return "ident-func"
        if k in ("struct", "enum"):
            return "ident-type"
        return "ident-unknown"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tsv", action="store_true")
    args = ap.parse_args()

    us = units()
    text = {p: p.read_text(encoding="utf-8") for p in us}
    decls = {p: split_decls(text[p]) for p in us}

    owner, kind = {}, {}
    for p in us:
        for k, name, _, _ in decls[p]:
            if name not in owner:
                owner[name] = p
                kind[name] = k

    local_kinds = {p: {n: k for k, n, _, _ in decls[p]} for p in us}

    rows = []   # (单元, 行号, 名字, 头形态, 初值)
    for p in us:
        for k, name, line, body in decls[p]:
            if k != "let":
                continue
            init = init_of(body)
            rows.append((p, line, name, classify(init, local_kinds[p]), init))

    # 跨单元引用：调用点 / symbol_addr / 裸标识符
    use = collections.defaultdict(set)   # 名字 -> {引用方}
    for p in us:
        for line in text[p].splitlines():
            s = line.strip()
            if s.startswith("//") or s.startswith("extern "):
                continue
            for m in re.finditer(r"([A-Za-z_]\w*)\s*\(", line):
                if owner.get(m.group(1), p) != p:
                    use[m.group(1)].add(p)
            for m in re.finditer(r'symbol_addr\("(\w+?)_storage"\)', line):
                if owner.get(m.group(1), p) != p:
                    use[m.group(1)].add(p)
            for m in re.finditer(r"\b([A-Za-z_]\w*)\b", line):
                nm = m.group(1)
                if owner.get(nm, p) != p and kind.get(nm) == "let":
                    use[nm].add(p)

    edges = collections.defaultdict(set)   # 引用方 -> 被引用单元
    for nm, refs in use.items():
        for p in refs:
            edges[p].add(owner[nm])

    by_unit = collections.defaultdict(list)
    for p, line, name, cat, init in rows:
        by_unit[p].append((line, name, cat, init))

    n13 = sum(1 for r in rows if REACTION[r[3]][0] == 13)
    n4 = sum(1 for r in rows if REACTION[r[3]][0] == 4)
    ntbl = sum(1 for r in rows if r[3] == "table")

    print("单元数 %d，顶层静态 let %d 条" % (len(us), len(rows)))
    print("依赖边（跨单元调用 + 跨单元取址）%d" % sum(len(v) for v in edges.values()))
    print("头形态统计: " + ", ".join(
        "%s=%d" % (k, v) for k, v in collections.Counter(r[3] for r in rows).most_common()))
    print("→ 会触发 13 的 %d 条；会触发 4 的 %d 条；定长表（导出路径跳过）%d 条"
          % (n13, n4, ntbl))
    print()
    print("单元                          顶层let  数字  同模块标识符  定长表  触发4  触发13")
    for p in us:
        items = by_unit.get(p, [])
        if not items:
            continue
        c = collections.Counter(x[2] for x in items)
        print("%-28s %7d %5d %11d %6d %6d %7d" % (
            p.relative_to(BOOT).as_posix(), len(items), c["digit"], c["ident-let"],
            c["table"], c["ident-func"] + c["ident-type"],
            c["ident-unknown"] + c["other"]))
    print()

    if args.tsv:
        print("单元\t行号\t名字\t头形态\t实测status\t被谁跨单元引用")
        for p, line, name, cat, init in sorted(rows, key=lambda r: (str(r[0]), r[1])):
            refs = sorted(u.relative_to(BOOT).as_posix() for u in use.get(name, ()))
            print("%s\t%d\t%s\t%s\t%d\t%s" % (
                p.relative_to(BOOT).as_posix(), line, name, cat, REACTION[cat][0],
                ",".join(refs) or "-"))
        return 0

    for p in us:
        items = by_unit.get(p, [])
        if not items:
            continue
        print("=== %s（%d 条）" % (p.relative_to(BOOT).as_posix(), len(items)))
        for line, name, cat, init in sorted(items):
            refs = sorted(u.relative_to(BOOT).as_posix() for u in use.get(name, ()))
            print("  %4d  %-32s %-14s status=%d  跨单元引用: %s" % (
                line, name, cat, REACTION[cat][0],
                ", ".join(refs) if refs else "无"))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
