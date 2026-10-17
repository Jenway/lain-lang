# 统计 bootstrap 单元里顶层静态 let 的初值形态，并审计模块卫生。
#
# Meta 的导出路径 = bootstrap/std/modules.lain 的 meta_lower_import ->
# meta_emit_module_exports -> meta_emit_module_binding。后者只看初值**头节点**：
#
#   头是数字字面量      -> 发一个字面量；头只是表达式一部分时其余被丢掉（静默错值）
#   头是标识符          -> 去当前模块的作用域表里查：
#                           查到 kind == 3（值绑定）  -> 递归发那条，再发 #call 转接
#                           查到别的 kind（func/类型）-> lain_meta_fail(4)
#                           查不到（内建名 / 未知名） -> lain_meta_fail(13)
#   头是 '['（定长表）  -> 作用域扫描时已预置 emitted != 0，导出路径整条跳过：
#                           不发任何过程（导入方拿 0；成员被引用时发悬空 #call）
#   其余（'-'、'"' 等） -> 当成查不到的标识符 -> 13
#
# meta_lower_import 在 import 声明处把被导入模块全部值绑定发一遍，与它们是否被引用无关。
#
# 用法： python scripts/import_export_scan.py [--tsv]
#        python scripts/import_export_scan.py --audit
#        python scripts/import_export_scan.py --audit --rev <rev>
#
# 模块集合来自入口 bootstrap/meta.lain 顺 import("…") 求的闭包，共 28 个单元，与
# lain0 --entry bootstrap/meta.lain --split build/lain0/units/ 的 28 份一一对应
# （lain0 的路径解析规则见 Sema.cpp 的 resolveModulePath）。
#
# 默认模式按初值头节点分类，给出导出路径对每类的结果（0 / 4 / 13）以及每条被谁
# 跨单元引用；--audit 逐单元数跨单元引用、影子 extern、宿主 extern、同单元取址；
# --rev <rev> 用 git show <rev>:<路径> 读源码。

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOOT = ROOT / "bootstrap"
ENTRY_REL = "meta"          # 入口：bootstrap/meta.lain（相对 BOOT、省略 .lain）

DECL_RE = re.compile(r"^(let|func|extern|struct|enum)\s+([A-Za-z_]\w*)")
IDENT_RE = re.compile(r"^[A-Za-z_]\w*$")

# 头形态 -> (status, 说明)
REACTION = {
    "digit": (0, "发字面量"),
    "ident-let": (0, "递归发同模块值绑定 + #call 转接"),
    "ident-func": (4, "头是 func：kind != 3"),
    "ident-type": (4, "头是 struct/enum：kind != 3"),
    "ident-unknown": (13, "头是查不到的标识符（内建名 / 未知名）"),
    "table": (0, "作用域扫描预置 emitted，导出路径整条跳过（不发过程）"),
    "other": (13, "头不是标识符也不是数字（'-' / '\"' / …）"),
}


IMPORT_LINE = re.compile(r'let\s+\w+\s*=\s*import\("([^"]*)"\)')


def _norm_parts(parts):
    """`/` 分段的纯词法规范化（与 lain0 一致：丢掉 `.`、消掉 `a/../`）。"""
    out = []
    for part in parts:
        if part in ("", "."):
            continue
        if part == "..":
            if out and out[-1] != "..":
                out.pop()
            else:
                out.append("..")
        else:
            out.append(part)
    return "/".join(out)


def _import_specs(text):
    """源码里的 import 说明符（顶层 `let NAME = import("…");`，跳过 `//` 注释）。"""
    for raw in text.splitlines():
        m = IMPORT_LINE.match(raw.split("//", 1)[0])
        if m:
            yield m.group(1)


def _resolve_spec(importer_rel, spec):
    """import("…") 的说明符 -> 被导入单元的相对路径（省略 .lain），lain0 的规则。"""
    if "::" in spec:
        base, mod = spec.rsplit("::", 1)
    else:
        base, mod = "", spec
    d = importer_rel.rsplit("/", 1)[0] if "/" in importer_rel else ""
    cands = [_norm_parts((d + "/" + mod).split("/"))]
    if base:
        cands.append(_norm_parts((d + "/" + base + "/" + mod).split("/")))
    for c in cands:
        if (BOOT / (c + ".lain")).exists():
            return c
    return None


def closure_units():
    """从入口 `bootstrap/meta.lain` 顺 import 求出的模块集合（相对 BOOT、省略 .lain）。

    顺序 = 发现顺序（入口先，随后按 import 语句出现次序），与
    `lain0 --entry bootstrap/meta.lain --split …` 的闭包一致，但**不是**文件的
    模块键排序 —— 需要那个排序的地方（装载、指纹）自己按模块键排。
    """
    seen, order = set(), []

    def visit(rel):
        if rel in seen:
            return
        path = BOOT / (rel + ".lain")
        if not path.exists():
            return
        seen.add(rel)
        order.append(rel)
        for spec in _import_specs(path.read_text(encoding="utf-8")):
            target = _resolve_spec(rel, spec)
            if target is not None:
                visit(target)

    visit(ENTRY_REL)
    return order


def units():
    """28 个单元（相对 bootstrap/ 的 .lain 路径）。

    集合来自入口的 import 闭包（见 closure_units），**不读任何清单**。
    """
    return [BOOT / (rel + ".lain") for rel in closure_units()]


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


# ---------------- 第 4 步闸门 4：改写审计 ----------------
# 逐单元数「跨单元引用 / 影子 extern / 宿主 extern」，用来核对改写后的状态：
#   * 裸跨单元调用、跨单元取址、影子 extern 都应为 0（已改单元）
#   * 跨单元引用一律是 `别名.名字`（别名 = 被导入单元的模块键）
#   * 宿主 extern（名字不是本仓任何顶层定义）逐单元与改造前相同
# `--rev <rev>` 用 `git show <rev>:<路径>` 读源码，于是同一套口径可以量历史版本。

EXTERN_RE = re.compile(r"^\s*extern\s+(\w+)", re.M)
# 顶层**定义**（不含 extern）：影子 extern 与宿主 extern 的判据。
OWNER_RE = re.compile(r"^(func|let|struct|enum)\s+([A-Za-z_]\w*)", re.M)
IMPORT_RE = re.compile(r"^let\s+(\w+)\s*=\s*import\(\"([^\"]*)\"\)", re.M)
BARE_CALL_RE = re.compile(r"(?<![\w.])([A-Za-z_]\w*)\s*\(")
MEMB_RE = re.compile(r"([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)")
SYM_RE = re.compile(r'symbol_addr\("(\w+?)_storage"\)')

UNIT_REL = {}


def norm_path(p):
    out = []
    for part in p.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if out and out[-1] != "..":
                out.pop()
            else:
                out.append("..")
        else:
            out.append(part)
    return "/".join(out)


def resolve_spec(unit_rel, spec):
    """import("…") 的说明符 -> 被导入单元的相对路径（省略 .lain），与 lain0 同规则。"""
    if "::" in spec:
        base, mod = spec.rsplit("::", 1)
    else:
        base, mod = "", spec
    d = unit_rel.rsplit("/", 1)[0] if "/" in unit_rel else ""
    cands = [norm_path(d + "/" + mod), norm_path(d + "/" + base + "/" + mod)]
    for c in cands:
        if c in UNIT_REL:
            return c
    return cands[0]


def load_text(rev, path):
    if not rev:
        return path.read_text(encoding="utf-8")
    rel = path.relative_to(ROOT).as_posix()
    out = subprocess.run(["git", "show", "%s:%s" % (rev, rel)], cwd=str(ROOT),
                         capture_output=True)
    if out.returncode != 0:
        raise SystemExit("git show %s:%s 失败" % (rev, rel))
    return out.stdout.decode("utf-8")


def audit(rev):
    us = units()
    UNIT_REL.clear()
    for p in us:
        UNIT_REL[p.relative_to(BOOT).with_suffix("").as_posix()] = p
    text = {rel: load_text(rev, p) for rel, p in UNIT_REL.items()}

    owner, kind = {}, {}
    for rel in text:
        for m in OWNER_RE.finditer(text[rel]):
            k, n = m.group(1), m.group(2)
            if n in owner:
                continue
            owner[n] = rel
            kind[n] = k

    rows = []
    for rel in text:
        t = text[rel]
        alias = {m.group(1): resolve_spec(rel, m.group(2))
                 for m in IMPORT_RE.finditer(t)}
        bare = memb_call = memb_data = 0
        sym_x = sym_local = sym_unknown = 0
        bad = []
        for i, line in enumerate(t.splitlines(), 1):
            s = line.strip()
            if s.startswith("//") or s.startswith("extern "):
                continue
            for m in BARE_CALL_RE.finditer(line):
                o = owner.get(m.group(1))
                if o is not None and o != rel:
                    bare += 1
            for m in MEMB_RE.finditer(line):
                q, nm = m.group(1), m.group(2)
                o = alias.get(q)
                if o is None:
                    continue
                if line[m.end():].lstrip().startswith("("):
                    memb_call += 1
                else:
                    memb_data += 1
                if owner.get(nm) != o:
                    bad.append((i, q, nm))
            for m in SYM_RE.finditer(line):
                o = owner.get(m.group(1))
                if o is None:
                    sym_unknown += 1
                elif o == rel:
                    sym_local += 1
                else:
                    sym_x += 1
        shadow = sorted(n for n in set(EXTERN_RE.findall(t))
                        if owner.get(n) not in (None, rel))
        host = sorted(n for n in set(EXTERN_RE.findall(t)) if n not in owner)
        rows.append(dict(rel=rel, bare=bare, memb_call=memb_call,
                         memb_data=memb_data, sym_x=sym_x, sym_local=sym_local,
                         sym_unknown=sym_unknown, shadow=shadow, host=host,
                         bad=bad, imports=len(alias)))

    print("审计版本: %s" % (rev or "工作树"))
    print("单元                            import 裸跨单元调用 M.成员调用 M.数据成员 "
          "跨单元取址 影子extern 宿主extern 同单元取址")
    for r in rows:
        print("%-32s %6d %10d %9d %9d %9d %10d %10d %9d"
              % (r["rel"], r["imports"], r["bare"], r["memb_call"],
                 r["memb_data"], r["sym_x"], len(r["shadow"]), len(r["host"]),
                 r["sym_local"]))
    def tot(k):
        return sum(len(r[k]) if isinstance(r[k], list) else r[k] for r in rows)
    print("合计: import=%d 裸跨单元调用=%d M.成员调用=%d M.数据成员=%d "
          "跨单元取址=%d 影子extern=%d 宿主extern=%d 同单元取址=%d"
          % (tot("imports"), tot("bare"), tot("memb_call"), tot("memb_data"),
             tot("sym_x"), tot("shadow"), tot("host"), tot("sym_local")))
    dirty = [r["rel"] for r in rows if r["shadow"] or r["bare"] or r["sym_x"]]
    print("已改单元 %d/%d；未改（还有影子 extern 或裸跨单元引用或跨单元取址）: %s"
          % (len(rows) - len(dirty), len(rows), ", ".join(dirty) or "无"))
    bads = [(r["rel"], b) for r in rows for b in r["bad"]]
    if bads:
        print("M.名字 指向了非被导入单元的声明 %d 处:" % len(bads))
        for rel, b in bads[:20]:
            print("  %s:%d  %s.%s" % (rel, b[0], b[1], b[2]))
    else:
        print("M.名字 全部指向被导入单元自己的顶层声明（0 处错配）")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tsv", action="store_true")
    ap.add_argument("--audit", action="store_true",
                    help="第 4 步闸门 4：逐单元量跨单元引用 / 影子 extern / 宿主 extern")
    ap.add_argument("--rev", default="",
                    help="用 git show <rev>:<路径> 读源码（默认读工作树）")
    args = ap.parse_args()

    if args.audit:
        return audit(args.rev)

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
        print("单元\t行号\t名字\t头形态\tstatus\t被谁跨单元引用")
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
