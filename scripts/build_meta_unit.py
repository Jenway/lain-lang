#!/usr/bin/env python3
"""把 bootstrap/ 下的 Lain 源码拼成一份编译单元，编译成 LAINIR，并写出驱动要的单元清单。

    python scripts/build_meta_unit.py

产物（都在 build/lain0/ 下，build/ 被 Git 忽略）：

    bootstrap.lain        28 个单元按 SOURCE_ORDER 拼成的单份 Lain 源码
    bootstrap.l1          上面那份源码经 lain0 编译出的 LAINIR（即 Meta 本体）
    bootstrap_unit.txt    给 lain-meta 的 --unit 清单，内容是一行相对路径

用法（仓库根目录）：

    ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \\
        examples/arith.lain bootstrap/lain/examples/arith.lain

为什么清单里写 `../build/...`：驱动把清单每一行都当 `bootstrap/<行>` 打开（见
tools/meta_driver.c），所以只能这样指到 build/ 下的产物。另外注意驱动的 --unit
默认值是 `bootstrap/SOURCE_ORDER`，而本仓库里那个文件是**源码**顺序表，不是
LAINIR 单元清单，所以跑驱动时请显式传 --unit。
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ORDER = ROOT / "bootstrap" / "SOURCE_ORDER"
OUT_DIR = ROOT / "build" / "lain0"
LAIN0 = ROOT / "build" / "lain0" / "lain0.exe"


def source_order():
    """SOURCE_ORDER 里的路径是相对 bootstrap/ 的 .l1 名；对应的 Lain 源是同名 .lain。"""
    entries = []
    for raw in ORDER.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entries.append(ROOT / "bootstrap" / Path(line).with_suffix(".lain"))
    return entries


def assemble(paths):
    """拼成一份源码：丢掉「被本次包含的单元定义过」的 extern 声明。

    逐片编译时每片都要给跨单元被调过程写 extern；拼成一份后这些声明会和定义撞车
    （verify 2022 duplicate subroutine），所以这里按「谁定义了」把它们摘掉。
    """
    defs, pieces, dropped = set(), [], 0
    for p in paths:
        text = p.read_text(encoding="utf-8")
        defs |= set(re.findall(r"^(?:func|let)\s+(\w+)", text, re.M))

    for p in paths:
        kept = []
        for line in p.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^extern\s+(\w+)", line)
            if m and m.group(1) in defs:
                dropped += 1
                continue
            kept.append(line)
        rel = p.relative_to(ROOT).as_posix()
        pieces.append("// ===== %s =====\n" % rel + "\n".join(kept))
    return "\n".join(pieces) + "\n", len(defs), dropped


def main():
    if not ORDER.exists():
        print("找不到 %s" % ORDER)
        return 2
    paths = source_order()
    missing = [p for p in paths if not p.exists()]
    if missing:
        for p in missing:
            print("缺单元 %s" % p)
        return 2

    merged, defs, dropped = assemble(paths)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    merged_path = OUT_DIR / "bootstrap.lain"
    merged_path.write_text(merged, encoding="utf-8")
    print("units=%d definitions=%d externs dropped=%d -> %s"
          % (len(paths), defs, dropped, merged_path.relative_to(ROOT)))

    if not LAIN0.exists():
        print("找不到 %s（先构建 lain0）" % LAIN0)
        return 2
    # 宿主按平坦名字找 Meta 的三个入口（驱动默认 --entry 就是 lain_std_lower），
    # 所以它们在清单里标成根名：发出平坦名字，不带上 `<模块键>__` 前缀
    # （docs/plan-modules.md §3.2）。
    manifest = OUT_DIR / "bootstrap_manifest.txt"
    manifest.write_text(
        "bootstrap lain_std_abi_version lain_std_initialize lain_std_lower\n",
        encoding="utf-8")
    ir_path = OUT_DIR / "bootstrap.l1"
    with ir_path.open("wb") as fh:
        rc = subprocess.run(
            [str(LAIN0), "--unit", str(manifest)], stdout=fh).returncode
    if rc != 0:
        print("lain0 编译失败 rc=%d" % rc)
        return 1
    print("LAINIR -> %s (%d bytes)" % (ir_path.relative_to(ROOT), ir_path.stat().st_size))

    unit_list = OUT_DIR / "bootstrap_unit.txt"
    unit_list.write_text("../build/lain0/bootstrap.l1\n", encoding="utf-8")
    print("unit list -> %s" % unit_list.relative_to(ROOT))
    print()
    print("跑一次：")
    print("  ./build/lain-meta.exe --unit %s examples/arith.lain "
          "bootstrap/lain/examples/arith.lain"
          % str(unit_list.relative_to(ROOT)).replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
