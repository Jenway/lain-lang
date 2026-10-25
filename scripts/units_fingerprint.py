#!/usr/bin/env python3
"""整批指纹：把一份「每模块一份 LAINIR」的产出目录约成一个 SHA-256。

拆成多份产出之后，「一整份的字节数与 SHA-256」不再是有效判据（每份文件的内容只取决于
该模块自己，顺序无关）。替代它的是**整批指纹**（docs/proposal-modules-v2.md §7.2）：

    fingerprint = SHA-256( 按模块键排序后逐行拼接 "<模块键>  <该份文件的 SHA-256>" )

定义（逐字节精确，改动即改判据）：

  * 键 = 文件名去掉 `.l1`（= 模块键），按字节序排序；
  * 每行 = `<键>` + **两个空格** + 该份文件的 64 位小写十六进制 SHA-256；
  * 行间以 `\\n` 连接，**末尾再补一个 `\\n`**；
  * 对这串 UTF-8 字节取 SHA-256，就是指纹。

判据是「同一二进制 + 同一棵树 → 同一指纹（跑两次一致）」，不是「等于某个历史哈希」。

用法：

    python scripts/units_fingerprint.py                  # 默认 build/lain0/units/
    python scripts/units_fingerprint.py --list           # 另打每份的键与 SHA-256
    python scripts/units_fingerprint.py --units-dir <目录>
"""

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DIR = ROOT / "build" / "lain0" / "units"


def unit_files(units_dir):
    """目录里的 (模块键, 路径)，按模块键排序（= 装载顺序）。"""
    out = []
    for p in Path(units_dir).glob("*.l1"):
        out.append((p.stem, p))
    out.sort(key=lambda kv: kv[0].encode("utf-8"))
    return out


def fingerprint(units_dir):
    """返回 (份数, 指纹, [(键, sha256), …])；目录里没有 .l1 返回 (0, None, [])。"""
    pairs = []
    for key, path in unit_files(units_dir):
        pairs.append((key, hashlib.sha256(path.read_bytes()).hexdigest()))
    if not pairs:
        return 0, None, []
    blob = ("\n".join("%s  %s" % kv for kv in pairs) + "\n").encode("utf-8")
    return len(pairs), hashlib.sha256(blob).hexdigest(), pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--units-dir", default=str(DEFAULT_DIR))
    ap.add_argument("--list", action="store_true",
                    help="另打每份的模块键与 SHA-256（也就是指纹的输入）")
    args = ap.parse_args()

    count, fp, pairs = fingerprint(args.units_dir)
    if count == 0:
        print("目录里没有 .l1：%s" % args.units_dir)
        return 2
    if args.list:
        for key, sha in pairs:
            print("%s  %s" % (key, sha))
    print("units=%d" % count)
    print("fingerprint=%s" % fp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
