# 从 IR 头文件生成 src/ir_codes.lain。
#
# 用途：src 里的编译器要发 #add/#load/#return 这些算子，枚举值必须和
# seed/kernel/include/lain/ir/core.h 里的真值一致。手抄一份会在头文件增删
# 算子之后静默错位，所以这里按出现顺序数逗号取值。
#
# 用法：
#   python scripts/gen_v2_abi.py           写入 src/ir_codes.lain
#   python scripts/gen_v2_abi.py --check   只校验，不同步就返回 1
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE_H = os.path.join(ROOT, "seed", "kernel", "include", "lain", "ir", "core.h")
VERIFY_H = os.path.join(ROOT, "seed", "kernel", "include", "lain", "ir", "verify.h")
OUT = os.path.join(ROOT, "src", "ir_codes.lain")

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def strip_comments(text):
    return COMMENT.sub(" ", text)


def enum_blocks(text):
    # 返回 [(块内标识符列表, 每项是否显式赋值)]，顺序即出现顺序。
    blocks = []
    for m in re.finditer(r"typedef\s+enum\s*\{(.*?)\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;",
                         strip_comments(text), re.S):
        body = m.group(1)
        items = []
        for raw in body.split(","):
            item = raw.strip()
            if not item:
                continue
            if "=" in item:
                name, value = item.split("=", 1)
                items.append((name.strip(), value.strip()))
            else:
                items.append((item, None))
        blocks.append((m.group(2), items))
    return blocks


CONST_EXPR = re.compile(r"^[0-9a-fA-FxX\s()<>|+*\-]+$")


def const_value(text):
    """算 C 常量表达式：只允许整数、括号与 << | + * -（枚举里就是这么写的）。"""
    expr = re.sub(r"(\d+|0[xX][0-9a-fA-F]+)[uUlL]+", r"\1", text.strip())
    if not CONST_EXPR.match(expr):
        raise SystemExit("不认识的枚举常量表达式: " + text)
    return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - 字符集已限定


def assign(items):
    """按 C 规则推值：显式值为基准，其后隐式项递增。"""
    out = []
    next_value = 0
    for name, explicit in items:
        if explicit is None:
            value = next_value
        else:
            value = const_value(explicit)
        out.append((name, value))
        next_value = value + 1
    return out


def collect():
    core = open(CORE_H, encoding="utf-8").read()
    verify = open(VERIFY_H, encoding="utf-8").read()
    groups = []
    for type_name, items in enum_blocks(core):
        values = assign(items)
        if type_name == "L1TypeKind":
            groups.append(("物理类型（L1TypeKind）", [v for v in values if v[0].startswith("TY_")]))
        elif type_name == "L1OperandKind":
            groups.append(("操作数种类（L1OperandKind）", values))
        elif type_name == "L1MemOrder":
            groups.append(("内存序（L1MemOrder）", [v for v in values if v[0].startswith("ORDER_")]))
        elif type_name == "L1InstKind":
            groups.append(("指令（L1InstKind）", [v for v in values if v[0].startswith("INST_")]))
    checks = []
    for m in re.finditer(r"\b(L1V_[A-Z0-9_]+)\s*=\s*(-?\d+)", strip_comments(verify)):
        checks.append((m.group(1), int(m.group(2))))
    if not groups:
        raise SystemExit("core.h 里没有找到任何枚举")
    return groups, checks


def render(groups, checks):
    lines = [
        "// 此文件由 scripts/gen_v2_abi.py 从 seed/kernel/include/lain/ir/*.h 生成。",
        "// 不要手改：头文件里的枚举一改，就重跑脚本（verify_all.py 会校验是否同步）。",
        "",
    ]
    # 一律发成零参过程：常量以调用形式出现。
    for title, values in groups:
        lines.append("// " + title)
        for name, value in values:
            lines.append("let %s = func() -> u64 { return %d; };" % (name, value))
        lines.append("")
    lines.append("// 验证器诊断码（verify.h）")
    for name, value in checks:
        lines.append("let %s = func() -> u64 { return %d; };" % (name, value))
    lines.append("")
    return "\n".join(lines)


def main(argv):
    groups, checks = collect()
    text = render(groups, checks)
    check_only = "--check" in argv[1:]
    old = None
    if os.path.exists(OUT):
        old = open(OUT, encoding="utf-8").read()
    if check_only:
        if old != text:
            print("src/ir_codes.lain 与头文件不同步，运行 python scripts/gen_v2_abi.py")
            return 1
        print("ir_codes.lain 同步")
        return 0
    if old == text:
        print("ir_codes.lain 已是最新")
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("已写入 src/ir_codes.lain")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
