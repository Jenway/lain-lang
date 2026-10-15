#!/usr/bin/env python3
"""把 bootstrap/ 的模块清单交给 lain0 编成一份 LAINIR（即 Meta 本体），并写出驱动要的单元清单。

    python scripts/build_meta_unit.py

产物（都在 build/lain0/ 下，build/ 被 Git 忽略）：

    bootstrap.l1           `lain0 --unit bootstrap/SOURCE_ORDER` 的产物（Meta 本体）
    bootstrap_unit.txt     给 lain-meta 的 --unit 清单，内容是一行相对路径

用法（仓库根目录）：

    ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \\
        examples/arith.lain bootstrap/lain/examples/arith.lain

为什么清单里写 `../build/...`：驱动把清单每一行都当 `bootstrap/<行>` 打开（见
tools/meta_driver.c），所以只能这样指到 build/ 下的产物。另外注意驱动的 --unit
默认值是 `bootstrap/SOURCE_ORDER`，而本仓库里那个文件是**Lain 源码**的模块清单
（`<相对路径> [<根名> …]`），不是 LAINIR 单元清单，所以跑驱动时请显式传 --unit。

自 `bootstrap/SOURCE_ORDER` 改成模块清单之后，这里不再拼接源码：清单本身就是
lain0 的编译单元（docs/plan-modules.md §3.3/§3.7），模块顺序与根名都由它承载。
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "bootstrap" / "SOURCE_ORDER"
OUT_DIR = ROOT / "build" / "lain0"
LAIN0 = ROOT / "build" / "lain0" / "lain0.exe"


def manifest_units():
    """清单里的 (相对路径, [根名 …])；`#` 起注释，路径省略 .lain 后缀。"""
    entries = []
    for raw in MANIFEST.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        entries.append((fields[0], fields[1:]))
    return entries


def main():
    if not MANIFEST.exists():
        print("找不到 %s" % MANIFEST)
        return 2
    units = manifest_units()
    if not units:
        print("%s 里没有模块" % MANIFEST)
        return 2
    missing = [rel for rel, _ in units
               if not (ROOT / "bootstrap" / (rel + ".lain")).exists()]
    if missing:
        for rel in missing:
            print("缺单元 bootstrap/%s.lain" % rel)
        return 2

    if not LAIN0.exists():
        print("找不到 %s（先构建 lain0）" % LAIN0)
        return 2

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ir_path = OUT_DIR / "bootstrap.l1"
    roots = [(rel, names) for rel, names in units if names]
    print("清单 %s: %d 个模块，根名 %s" % (
        MANIFEST.relative_to(ROOT), len(units),
        ", ".join("%s:%s" % (rel, " ".join(names)) for rel, names in roots)
        or "无"))
    # 入口过程 lain_std_lower 等三个名字在清单里标成根名（发出平坦名字），
    # 所以清单直接交给 lain0，不再另生成一份根名清单（docs/plan-modules.md §3.2/§3.3）。
    rc = subprocess.run([str(LAIN0), "--unit", str(MANIFEST),
                         "-o", str(ir_path)]).returncode
    if rc != 0:
        print("lain0 编译失败 rc=%d（整份清单要全部单元改完才能通过；"
              "中间状态下的诊断见上面的 stderr）" % rc)
        return 1
    print("LAINIR -> %s (%d bytes)"
          % (ir_path.relative_to(ROOT), ir_path.stat().st_size))

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
