#!/usr/bin/env python3
"""把 bootstrap/ 的 Meta 从入口编成「每模块一份 LAINIR」，这就是默认产出路径。

    python scripts/build_meta_unit.py

一层包装：只是调 `lain0 --entry bootstrap/meta.lain --split build/lain0/units/`。
**不读任何清单、不生成任何清单**（`bootstrap/SOURCE_ORDER` 已在步 3 删除）；
模块集合由入口顺 import 求闭包得到，装载顺序由文件名（= 模块键）排序决定
（docs/proposal-modules-v2.md §二 / §7.1）。

产物（build/ 被 Git 忽略）：

    build/lain0/units/meta.l1
    build/lain0/units/std_declarations.l1
    …                                           共 28 份，文件名 = 模块键

用法（仓库根目录）：

    ./build/lain-meta.exe --units-dir build/lain0/units/ \\
        --out build/arith.out examples/arith.lain bootstrap/lain/examples/arith.lain

验收判据（不是「等于某个历史哈希」）：

  * 连跑两次，**整批指纹**一致 —— `--check-twice` 会实测这一条；
  * 合并装载后 `verify subroutines=870 data=216`、`host_status=0`、`region grants=2`、
    `output bytes=74`，arith 产物与 `build/arith.out` 逐字节相同。

拆分的等价性（28 份 = 一整份）由 `scripts/split_equiv_check.py` 的「规范化逐份比对」证明。
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import units_fingerprint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
LAIN0 = ROOT / "build" / "lain0" / "lain0.exe"
ENTRY = ROOT / "bootstrap" / "meta.lain"
OUT_DIR = ROOT / "build" / "lain0" / "units"


def run_split(entry, out_dir):
    """调 lain0 的拆分产出；返回退出码。先清掉目录里陈旧的 .l1，产物集合才干净。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in out_dir.glob("*.l1"):
        stale.unlink()
    return subprocess.run([str(LAIN0), "--entry", str(entry),
                           "--split", str(out_dir)]).returncode


def report(out_dir, label):
    count, fp, _ = units_fingerprint.fingerprint(out_dir)
    print("%s: units=%d fingerprint=%s" % (label, count, fp))
    return count, fp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entry", default=str(ENTRY))
    ap.add_argument("--units-dir", default=str(OUT_DIR))
    ap.add_argument("--check-twice", action="store_true",
                    help="再拆一次到临时目录，断言整批指纹与第一次相同（文档里的判据）")
    args = ap.parse_args()

    entry = Path(args.entry)
    out_dir = Path(args.units_dir)
    if not entry.exists():
        print("找不到入口 %s" % entry)
        return 2
    if not LAIN0.exists():
        print("找不到 %s（先构建 lain0）" % LAIN0)
        return 2

    rc = run_split(entry, out_dir)
    if rc != 0:
        print("lain0 拆分产出失败 rc=%d" % rc)
        return 1
    count, fp = report(out_dir.relative_to(ROOT).as_posix(), "units")

    if args.check_twice:
        tmp_dir = out_dir.parent / (out_dir.name + ".twice")
        if tmp_dir.exists():
            shutil.rmtree(tmp_dir)
        rc = run_split(entry, tmp_dir)
        if rc != 0:
            print("第二次拆分失败 rc=%d" % rc)
            return 1
        _, fp2 = report(tmp_dir.relative_to(ROOT).as_posix(), "units(第二次)")
        same = fp == fp2
        print("整批指纹跑两次一致: %s" % same)
        shutil.rmtree(tmp_dir, ignore_errors=True)
        if not same:
            return 1

    print()
    print("跑一次（不带 --entry；装载顺序由文件名排序决定）：")
    print("  ./build/lain-meta.exe --units-dir %s \\"
          % out_dir.relative_to(ROOT).as_posix())
    print("      --out build/arith.out examples/arith.lain bootstrap/lain/examples/arith.lain")
    return 0


if __name__ == "__main__":
    sys.exit(main())
