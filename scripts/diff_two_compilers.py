#!/usr/bin/env python3
"""差分测试：同一棵树，lain0 与 Meta 各出各的模块单元，逐份比对。

这是 `docs/proposal-modules-v2.md` §六 **步 4** 的工具，也是
`docs/plan-selfhost.md` 里「两条编译路径现在还不一致」的日常观测口。

两条路径（都在同一棵树上，所以差异只可能来自编译器本身）：

    lain0（C++）      ./build/lain0/lain0.exe --entry bootstrap/meta.lain --split build/lain0/units/
                      → 每个模块一份，文件名 = 模块键（std_lex.l1 …），共 28 份
    Meta（Lain/VM）   ./build/lain-meta.exe --units-dir build/lain0/units/ --out <临时> --lower 0 <逻辑路径> bootstrap/<路径>.lain
                      → 把**同一份源码**降级成 LAINIR 文本

脚本对每个模块做两件事：

  ① 取 lain0 的该份单元 `build/lain0/units/<模块键>.l1`；
  ② 用 Meta 降级同一份源码 `bootstrap/<路径>.lain`（走驱动、产物写临时文件），
     记录它的 `host_status`、trap 与产物字节。

对 Meta **产出非空产物**的模块，逐份比对两边文本，并把差异**分类**（不只报一个
「不同」）。分类口径见下面 `compare()` 的注释，至少覆盖：

  * 符号命名不一致             —— 被降级单元自己的顶层 `let` 值绑定过程可能仍是扁平名
                                 （`answer()`），lain0 发 `<模块键>__answer`；静态数据
                                 的 `_storage` 后缀也可能差一层；
  * 临时值编号                 —— `%rN` 的编号/出现位置不同；
  * 段布局 / 空行              —— 同一批行（规范化后）顺序或空行结构不同；
  * 缺失 / 截断 / 其他         —— Meta 半途失败（非零 status 或 VM trap）导致的行缺失。

比对给**两个口径**（与 `scripts/split_equiv_check.py` 一致）：先原样比，再把临时值
编号（`%rN` → `%r#`）与模块前缀（`<键>__` → 空）规范化后比。

用法（仓库根目录）：

    python scripts/diff_two_compilers.py                # 全部模块
    python scripts/diff_two_compilers.py --only std_lex  # 只跑一个模块键
    python scripts/diff_two_compilers.py --strict        # 有任何红行就退出码 1

退出码：0 = 脚本跑完（**比对有差异也算 0**，它是观测工具不是闸门）；
        1 = 只在给了 `--strict` 且存在红行时；
        2 = 环境缺件（lain0 单元目录 / Meta 驱动 / bootstrap 目录）。

**多数模块现在还跑不起来是预期的**：本脚本按「规范化 spec 逐字节相等」登记源码，登记
条数受驱动上限（32 条）约束，闭包大的模块喂不进驱动。Meta 的降级进度由
`scripts/selfhost_gate.ps1`（`--root bootstrap` 登记闭包）度量。红行不是脚本坏了，
是两条路径确实不一致。
"""

import argparse
import posixpath
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DRIVER_MAX_SOURCES = 32          # seed/meta_driver.c: DRIVER_MAX_SOURCES

TMPRE = re.compile(rb"%r\d+")
IMPORT_RE = re.compile(r'import\("([^"]*)"\)')
PROC_RE = re.compile(rb"#proc ([^ (]+)")
DATA_RE = re.compile(rb"data ([^ ]+)")


# ---------------------------------------------------------------- 源码侧工具

def _specs_of(boot, rel):
    """单元源码里的 import 说明符（按出现顺序去重；跳过 `//` 注释）。"""
    text = (boot / (rel + ".lain")).read_text(encoding="utf-8", errors="replace")
    out = []
    for line in text.splitlines():
        for m in IMPORT_RE.finditer(line.split("//", 1)[0]):
            if m.group(1) not in out:
                out.append(m.group(1))
    return out


def _norm_spec(spec):
    """Meta 的规范化的名字：`::`→`/`，末尾补 `.lain`（已写就不补）。"""
    s = spec.replace("::", "/")
    return s if s.endswith(".lain") else s + ".lain"


def _resolve(boot, importer_rel, spec):
    """lain0 的模块路径解析（相对导入方目录），返回被导入单元的 rel 或 None。"""
    pos = spec.rfind("::")
    base = "" if pos < 0 else spec[:pos]
    mod = spec if pos < 0 else spec[pos + 2:]
    cands = [posixpath.join(posixpath.dirname(importer_rel), mod + ".lain")]
    if base:
        cands.append(posixpath.join(posixpath.dirname(importer_rel), base, mod + ".lain"))
    for c in cands:
        if (boot / c).exists():
            return posixpath.normpath(c)[:-len(".lain")]
    return None


def closure_regs(boot, entry):
    """目标单元 + 传递 import 闭包的**登记表**（逻辑路径 → 源文件 rel）。

    登记名 = **源码里写的 spec 规范化之后的名字**（Meta 的 import 匹配就是
    「规范化 spec 与登记名逐字节相等」，见 docs/04-bootstrap-dialect.md 的《模块：
    成员位置与符号命名》）。同一个文件会被不同导入方用不同 spec 点到，所以会登记成
    多条，登记条数容易超过驱动上限。
    """
    reg = {}
    seen = {entry}
    queue = [entry]
    while queue:
        cur = queue.pop(0)
        for spec in _specs_of(boot, cur):
            tgt = _resolve(boot, cur, spec)
            if tgt is None:
                continue
            reg.setdefault(_norm_spec(spec), tgt)
            if tgt not in seen:
                seen.add(tgt)
                queue.append(tgt)
    return reg


def module_sources(boot, units_dir):
    """{模块键: bootstrap 相对路径}；键 = 相对路径去 `.lain`、`/`→`_`。

    用「枚举 bootstrap 源码再算键」而不是「把键的 `_` 还原成 `/`」——后者在
    `std/handlers/enum` 这种名字上会有歧义。
    """
    want = {p.stem for p in units_dir.glob("*.l1")}
    out = {}
    for src in boot.rglob("*.lain"):
        rel = src.relative_to(boot).with_suffix("").as_posix()
        key = rel.replace("/", "_")
        if key in want:
            out[key] = rel
    return out


# ---------------------------------------------------------------- 驱动侧

def run_meta(meta_exe, units_dir, boot, out_path, rel, reg):
    """把 `bootstrap/<rel>.lain` 交给 Meta 降级；返回结果字典。"""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.unlink(missing_ok=True)      # 失败的运行不写文件，陈旧产物会假通过
    cmd = [str(meta_exe), "--units-dir", str(units_dir),
           "--out", str(out_path), "--lower", "0", "-q"]
    for logical, tgt in [(rel + ".lain", rel)] + sorted(reg.items()):
        cmd += [logical, "bootstrap/" + tgt + ".lain"]
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True,
                              text=True, timeout=args_timeout())
    except subprocess.TimeoutExpired:
        return dict(rc=None, status=None, trap=None, out_bytes=None,
                    product=None, stdout="", note="timeout")
    product = out_path.read_bytes() if out_path.exists() else b""
    status = trap = out_bytes = None
    for line in proc.stdout.splitlines():
        if "host_status=" in line:
            status = line.split("host_status=")[-1].strip()
        if line.startswith("trap "):
            trap = line[len("trap "):].strip()
        if line.startswith("output bytes="):
            out_bytes = line.split("=")[-1].strip()
    note = ""
    if "源码太多" in proc.stdout:
        note = "driver 源码上限（%d 条位置参数 > %d）" % (len(reg), DRIVER_MAX_SOURCES)
    return dict(rc=proc.returncode, status=status, trap=trap,
                out_bytes=out_bytes, product=product, stdout=proc.stdout, note=note)


_TIMEOUT = [300]


def args_timeout():
    return _TIMEOUT[0]


def check_unit(check_exe, path):
    """产物装得回去吗（lainir_parse + lainir_verify）。缺失工具返回 None。"""
    if not check_exe.exists() or not path.exists():
        return None
    proc = subprocess.run([str(check_exe), str(path)], cwd=str(ROOT),
                          capture_output=True, text=True)
    first = (proc.stdout.strip().splitlines() or [""])[0]
    return first


# ---------------------------------------------------------------- 比对与分类

def _norm_prefix(bs, key):
    return bs.replace(key.encode("utf-8") + b"__", b"")


def _norm_tmp(bs):
    return TMPRE.sub(b"%r#", bs)


def _nonblank(bs):
    return [l for l in bs.split(b"\n") if l.strip()]


def compare(unit, product, key):
    """两边文本的比对：两个口径 + 差异分类 + 首个差异位置。

    返回 (结论, 类别列表, 首个差异 (行号, unit 行, product 行) 或 None,
          (Meta 已发出的行规范化后对上的条数, Meta 已发出的行条数))。
    """
    if unit == product:
        return "逐字相同", [], None, (0, 0)

    nb_u = _nonblank(unit)
    nb_p = _nonblank(product)
    u_b = [_norm_tmp(_norm_prefix(l, key)) for l in nb_u]
    p_b = [_norm_tmp(_norm_prefix(l, key)) for l in nb_p]

    if u_b == p_b:
        return "规范化后相同", [], None, (len(set(p_b)), len(set(p_b)))

    # 逐个规范化层能多对上几行？用来判定「哪一类差异在起主导」
    raw = len(set(nb_u) & set(nb_p))
    pref = len(set(_norm_prefix(l, key) for l in nb_u)
               & set(_norm_prefix(l, key) for l in nb_p))
    tmp = len(set(_norm_tmp(l) for l in nb_u)
              & set(_norm_tmp(l) for l in nb_p))
    both = len(set(u_b) & set(p_b))

    # 首个差异位置：按原样（不规范化）逐行比
    ul = unit.split(b"\n")
    pl = product.split(b"\n")
    first = None
    for i in range(max(len(ul), len(pl))):
        a = ul[i] if i < len(ul) else b"<none>"
        b = pl[i] if i < len(pl) else b"<none>"
        if a != b:
            first = (i + 1, a, b)
            break

    cats = []
    # 前缀/mangle 与临时值编号是两条独立的规范化层：分别看它们各自多对上了几行
    if pref > raw:
        cats.append("符号前缀/mangle 不一致")
    if tmp > raw:
        cats.append("临时值编号")
    if both > max(pref, tmp):
        # 只有把两层都规范化才对上，说明两者叠加
        if "符号前缀/mangle 不一致" not in cats:
            cats.append("符号前缀/mangle 不一致")
        if "临时值编号" not in cats:
            cats.append("临时值编号")

    # 段布局 / 空行：共有的（规范化后）行相对顺序不同，或**首个差异**就是空行 vs 非空行
    common = set(u_b) & set(p_b)
    ord_u = [l for l in u_b if l in common]
    ord_p = [l for l in p_b if l in common]
    blank_first = first is not None and (not first[1].strip() or not first[2].strip())
    if ord_u != ord_p or blank_first:
        cats.append("段布局/空行")

    # 缺失 / 截断：规范化后仍对不上的行（Meta 非零 status 或 VM trap 会截断产物）
    if (set(u_b) - set(p_b)) or (set(p_b) - set(u_b)):
        cats.append("缺失/截断/其他")

    if not cats:
        cats.append("其他")

    return "仍有差异", cats, first, (both, len(set(p_b)))


def _clip(bs, n=72):
    s = bs.decode("utf-8", "replace")
    return s if len(s) <= n else s[:n] + "…"


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser(description="lain0 vs Meta：逐模块单元差分测试")
    ap.add_argument("--units-dir", default=str(ROOT / "build" / "lain0" / "units"),
                    help="lain0 的每模块一份产出目录（默认 build/lain0/units）")
    ap.add_argument("--bootstrap", default=str(ROOT / "bootstrap"),
                    help="bootstrap 源码根（默认 bootstrap/）")
    ap.add_argument("--meta", default=str(ROOT / "build" / "lain-meta.exe"),
                    help="Meta 驱动（默认 build/lain-meta.exe）")
    ap.add_argument("--check-unit", default=str(ROOT / "build" / "check_unit.exe"),
                    help="产物装回闸门（默认 build/check_unit.exe；缺了就不报这一列）")
    ap.add_argument("--out-dir", default=str(ROOT / "build" / "diff-two-compilers"),
                    help="Meta 产物落盘目录（默认 build/diff-two-compilers，gitignore）")
    ap.add_argument("--only", default="", help="只跑这个模块键（如 std_lex）")
    ap.add_argument("--timeout", type=int, default=300, help="单次 Meta 运行超时秒数")
    ap.add_argument("--strict", action="store_true",
                    help="存在红行（未一致）时退出码 1；默认恒 0")
    args = ap.parse_args()

    _TIMEOUT[0] = args.timeout
    units_dir = Path(args.units_dir)
    boot = Path(args.bootstrap)
    meta_exe = Path(args.meta)
    out_dir = Path(args.out_dir)
    check_exe = Path(args.check_unit)

    if not units_dir.is_dir() or not any(units_dir.glob("*.l1")):
        print("找不到 lain0 单元目录（跑 scripts/build_meta_unit.py 或 "
              "lain0 --entry bootstrap/meta.lain --split %s）" % units_dir)
        return 2
    if not boot.is_dir():
        print("找不到 bootstrap 源码根 %s" % boot)
        return 2
    if not meta_exe.exists():
        print("找不到 Meta 驱动 %s（先 cmake --build build）" % meta_exe)
        return 2

    keys = sorted((p.stem for p in units_dir.glob("*.l1")), key=lambda k: k.encode())
    src_of = module_sources(boot, units_dir)
    if args.only:
        keys = [k for k in keys if k == args.only]
        if not keys:
            print("--only %s 不在单元目录里（现有：%s）"
                  % (args.only, ", ".join(sorted(src_of))))
            return 2

    print("差分测试：lain0 单元 %s  ×  Meta 驱动 %s"
          % (units_dir.relative_to(ROOT).as_posix() if units_dir.is_relative_to(ROOT)
             else units_dir, meta_exe.name))
    print()

    rows = []
    for key in keys:
        rel = src_of.get(key)
        if rel is None:
            rows.append(dict(key=key, meta="无对应源码", verdict="未比对",
                             detail="bootstrap/ 下找不到 %s.lain" % key.replace("_", "/"),
                             green=False))
            continue
        reg = closure_regs(boot, rel)
        unit = (units_dir / (key + ".l1")).read_bytes()
        out_path = out_dir / (key + ".out")
        res = run_meta(meta_exe, units_dir, boot, out_path, rel, reg)

        # -- Meta 状态 --
        if res["rc"] == 2 and "源码太多" in res["stdout"]:
            meta_state = "driver rc=2（登记 %d 条 > 上限 %d）" % (len(reg) + 1,
                                                             DRIVER_MAX_SOURCES)
        elif res["rc"] is None:
            meta_state = "超时（>%ds）" % args.timeout
        elif res["status"] is None:
            meta_state = "驱动失败 rc=%s" % res["rc"]
        elif res["trap"] and res["trap"] != "none":
            meta_state = "host_status=%s + trap（产物截断）" % res["status"]
        elif res["status"] == "0":
            meta_state = "成功（host_status=0）"
        else:
            meta_state = "host_status=%s" % res["status"]

        product = res["product"] or b""
        if not product:
            chk = None
            rows.append(dict(key=key, meta=meta_state, verdict="未比对（无产物）",
                             detail=("unit %dB / 产物 0B；%d 条位置参数；%s"
                                     % (len(unit), len(reg) + 1,
                                        res["note"] or meta_state)),
                             green=False))
            continue

        verdict, cats, first, (hit, total) = compare(unit, product, key)
        chk = check_unit(check_exe, out_path)
        detail = "类别=%s" % ("、".join(cats) if cats else "无")
        if total:
            detail += "；Meta 已发出的行规范化后对上 %d/%d" % (hit, total)
        if first is not None:
            detail += "；首个差异 行%d unit=%r product=%r" % (
                first[0], _clip(first[1]), _clip(first[2]))
        detail = ("unit %dB / 产物 %dB；%s" % (len(unit), len(product), detail))
        if chk:
            detail += "；check_unit: %s" % chk
        green = verdict in ("逐字相同", "规范化后相同")
        rows.append(dict(key=key, meta=meta_state, verdict=verdict,
                         detail=detail, green=green))

    # ------------------------------ 表 ------------------------------
    w = max([len(r["key"]) for r in rows] + [6])
    print("%-*s  %-40s  %s" % (w, "模块", "Meta 状态", "与 lain0 的比对结论"))
    print("-" * (w + 2 + 40 + 2 + 24))
    for r in rows:
        print("%-*s  %-40s  %s" % (w, r["key"], r["meta"], r["verdict"]))
    print()

    print("== 逐行细节 ==")
    for r in rows:
        print("  %-*s  %s" % (w, r["key"], r["detail"]))
    print()

    covered = [r for r in rows if r["verdict"].startswith(("逐字", "规范化", "仍有"))]
    green = [r for r in rows if r["green"]]
    identical = [r for r in rows if r["verdict"] == "逐字相同"]
    normed = [r for r in rows if r["verdict"] == "规范化后相同"]

    print("== 总结 ==")
    print("  模块总数 %d；Meta 给出状态并产出非空产物、进入比对 %d；"
          "其中一致（逐字相同 %d / 规范化后相同 %d）共 %d。"
          % (len(rows), len(covered), len(identical), len(normed), len(green)))
    nolower = [r for r in rows if not r["verdict"].startswith(("逐字", "规范化", "仍有"))]
    if nolower:
        from collections import Counter
        c = Counter("driver-limit" if r["meta"].startswith("driver rc=2")
                    else ("无产物" if r["verdict"].startswith("未比对（无产物）")
                          else "其他") for r in nolower)
        print("  未进入比对 %d 个：%s。"
              % (len(nolower),
                 "、".join("%s=%d" % kv for kv in sorted(c.items()))))
    if green:
        print("  一致模块：%s" % ", ".join(r["key"] for r in green))
    else:
        print("  一致模块：无。")
    print("  说明：不一致的两大来源是「Meta 把单元**自己的**顶层名发成扁平名、lain0 发"
          " `<模块键>__` 前缀名（数据符号还多 `_storage`）」与「Meta 降级能力缺口"
          "（非零 host_status / VM trap → 产物截断）」。见 docs/plan-selfhost.md 九步清单。")

    if args.strict and (not green or len(green) != len(rows)):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
