#!/usr/bin/env python3
"""拆分等价性：「每模块一份」的 28 份 = 一整份（规范化逐份比对）。

拆成多份之后，「按模块键排序拼起来 == 今天的 bootstrap.l1」这条**逐字节**判据不再成立，
而且本来就不该成立：整份产出里临时值编号（`%rN`）是**全局**的，而每份各自从 1 起；
根名在整份里可能带 `meta__` 前缀（旧 `--unit` 口径），在单元里是平坦名。
（原因与逐条实测见 docs/proposal-modules-v2.md §六 步 1 与 §7.2。）

因此等价性的判据改为**逐模块比对**：把整份里属于模块 K 的那一段切出来，与
`units/K.l1` 比；先原样比，再把临时值编号规范化（`%rN` → `%r#`）后比，再把根名
（`meta__lain_std_*` → `lain_std_*`）规范化后比。**全部 28 个模块的三层都相等**
才算等价。

用法（仓库根目录）：

    python scripts/split_equiv_check.py            # 用现成的两份产物
    python scripts/split_equiv_check.py --build    # 先按入口重新产出两份

产物来源（都由 `--entry bootstrap/meta.lain` 出发，没有任何清单）：

    units/K.l1      lain0 --entry bootstrap/meta.lain --split build/lain0/units/
    whole.l1        lain0 --entry bootstrap/meta.lain -o build/lain0/whole.l1

退出码 0 = 28 个模块规范化后逐份相同；1 = 有不同（并打印首个不同行）。
"""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import units_fingerprint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
LAIN0 = ROOT / "build" / "lain0" / "lain0.exe"
ENTRY = ROOT / "bootstrap" / "meta.lain"
UNITS = ROOT / "build" / "lain0" / "units"
WHOLE = ROOT / "build" / "lain0" / "whole.l1"

TMP = re.compile(rb"%r\d+")
ROOTNAME = re.compile(rb"\bmeta__(lain_std_(?:abi_version|initialize|lower))\b")

norm_tmp = lambda b: TMP.sub(b"%r#", b)
norm_all = lambda b: ROOTNAME.sub(rb"\1", norm_tmp(b))


def sha(b):
    return hashlib.sha256(b).hexdigest()


def build(entry, units_dir, whole):
    """按入口重新产出两份（拆分 + 整份）。都从同一个入口出发，无清单。"""
    if not LAIN0.exists():
        print("找不到 %s（先构建 lain0）" % LAIN0)
        return 1
    units_dir.mkdir(parents=True, exist_ok=True)
    for stale in units_dir.glob("*.l1"):
        stale.unlink()
    rc = subprocess.run([str(LAIN0), "--entry", str(entry),
                         "--split", str(units_dir)]).returncode
    if rc != 0:
        print("lain0 --split 失败 rc=%d" % rc)
        return 1
    rc = subprocess.run([str(LAIN0), "--entry", str(entry),
                         "-o", str(whole)]).returncode
    if rc != 0:
        print("lain0 -o（整份）失败 rc=%d" % rc)
        return 1
    return 0


def split_unit(text):
    """print.c 的形态：`data …` 行们 + 一个空行 + `#proc` 们。返回 (数据段, 代码段)。"""
    if text.startswith(b"#proc "):
        return b"", text
    i = text.find(b"\n#proc ")
    if i == -1:
        return text, b""
    return text[:i], text[i + 1:]


def whole_slices(whole, keys):
    """整份 -> {模块键: 该模块那一段}。

    数据行按 `<键>__` 前缀分组；过程按「平坦名（宿主 extern）跟随**后一个**带键的
    过程所属模块」分组 —— 源码里 extern 声明写在模块头部。
    """
    wlines = whole.split(b"\n")
    hdrs = [(j, l) for j, l in enumerate(wlines) if l.startswith(b"#proc ")]

    def proc_key(hdr):
        name = hdr.split(b" ")[1].split(b"(")[0].decode()
        return name.split("__")[0] if "__" in name else None

    owner = [None] * len(hdrs)
    nxt = None
    for i in range(len(hdrs) - 1, -1, -1):
        k = proc_key(hdrs[i][1])
        if k is not None:
            nxt = k
        owner[i] = nxt
    blocks = {}
    for i, k in enumerate(owner):
        blocks.setdefault(k, []).append(i)

    data_by_key = {}
    for line in wlines:
        if line.startswith(b"data "):
            sym = line[5:].split(b" ")[0]
            data_by_key.setdefault(sym.split(b"__")[0].decode(), []).append(line)

    out = {}
    for key in keys:
        d = b""
        if key in data_by_key:
            d = b"\n".join(data_by_key[key]) + b"\n"
        chunks = []
        for bi in blocks.get(key, []):
            start = hdrs[bi][0]
            end = hdrs[bi + 1][0] if bi + 1 < len(hdrs) else len(wlines)
            seg = wlines[start:end]
            while seg and seg[-1] == b"":
                seg.pop()
            chunks.append(b"\n".join(seg))
        p = (b"\n\n".join(chunks) + b"\n") if chunks else b""
        out[key] = d + (b"\n" if d and p else b"") + p
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entry", default=str(ENTRY))
    ap.add_argument("--units-dir", default=str(UNITS))
    ap.add_argument("--whole", default=str(WHOLE))
    ap.add_argument("--build", action="store_true",
                    help="先按入口重新产出 28 份与整份（否则用现成产物）")
    args = ap.parse_args()

    entry = Path(args.entry)
    units_dir = Path(args.units_dir)
    whole_path = Path(args.whole)

    if args.build:
        if build(entry, units_dir, whole_path) != 0:
            return 1
    if not whole_path.exists():
        print("找不到整份 %s（加 --build，或先 lain0 --entry … -o %s）"
              % (whole_path, whole_path))
        return 1

    keys = sorted((p.stem for p in units_dir.glob("*.l1")), key=lambda k: k.encode())
    if not keys:
        print("目录里没有 .l1：%s" % units_dir)
        return 1
    whole = whole_path.read_bytes()

    print("== 结构 ==")
    units = {}
    for k in keys:
        t = (units_dir / (k + ".l1")).read_bytes()
        d, p = split_unit(t)
        units[k] = (t, d, p)
    for k in keys:
        t, d, p = units[k]
        print("  %s/%s  bytes=%7d data=%4d proc=%4d"
              % (units_dir.relative_to(ROOT).as_posix(), k + ".l1", len(t),
                 d.count(b"data "), p.count(b"#proc ")))
    print("  合计 bytes=%d（整份 %d）" % (sum(len(units[k][0]) for k in keys), len(whole)))
    print()

    print("== 规范化逐份比对（等价性的判据）==")
    slices = whole_slices(whole, keys)
    bad = []
    for k in keys:
        t = units[k][0]
        ts = slices[k]
        same = ts == t
        same_n = norm_tmp(ts) == norm_tmp(t)
        same_all = norm_all(ts) == norm_all(t)
        if not same_all:
            bad.append(k)
        print("  %-26s 原样=%-5s 规范化编号=%-5s 再规范化根名=%-5s 字节 %d/%d"
              % (k, same, same_n, same_all, len(t), len(ts)))
    print("  28 个模块规范化后逐份相同: %s" % (not bad))
    if bad:
        for k in bad:
            ta, tb = norm_all(slices[k]).split(b"\n"), norm_all(units[k][0]).split(b"\n")
            print("    --- %s 不同：行数 整份=%d 单元=%d" % (k, len(ta), len(tb)))
            for j in range(min(len(ta), len(tb))):
                if ta[j] != tb[j]:
                    print("      行 %d 整份=%r" % (j, ta[j]))
                    print("      行 %d 单元=%r" % (j, tb[j]))
                    break
    print()

    print("== 为什么不做整份逐字节比对 ==")
    print("  整份的临时值编号是全局的、单元各自从 1 起（%%rN 引用数：整份 %d、28 份合计 %d）"
          % (len(TMP.findall(whole)), len(TMP.findall(b"".join(units[k][0] for k in keys)))))
    print("  整份按模块键顺序拼数据段也**不**等于整份的文本：两者的模块发射顺序不同。")
    print("  所以判据是逐模块 + 规范化，不是整份逐字节（docs/proposal-modules-v2.md §六 步 1）。")
    print()

    count, fp, _ = units_fingerprint.fingerprint(units_dir)
    print("== 整批指纹（日常闸门）==")
    print("  units=%d fingerprint=%s" % (count, fp))
    print("  整份参考：%s bytes=%d sha256=%s"
          % (whole_path.relative_to(ROOT).as_posix(), len(whole), sha(whole)))

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
