#!/usr/bin/env python3
"""VSpace / LAINVM 边界用例的运行器（交付 A 的入口）。

    python scripts/check_vspace.py                        # 全部用例
    python scripts/check_vspace.py --group region         # 只跑一组
    python scripts/check_vspace.py --case region_valid_read
    python scripts/check_vspace.py --report build/vspace.json

断言机制的负对照（两条都必须退出 1）：

    python scripts/check_vspace.py --case region_valid_read --expect-value 99
    python scripts/check_vspace.py --case load_outside_region --expect-trap 9999

为什么每条用例单开一个子进程：用例会故意撞边界，崩溃和超时都算**失败**，不能把整个
运行器带走。缺用例、输出解析不了，同样算失败。

退出码：0 = 全部通过；1 = 有失败、超时、崩溃或 BLOCKED；2 = 缺前置（没编驱动）或用法错误。
`BLOCKED` 是「契约未定或实现不存在」的用例，单列出来；**含 BLOCKED 的全量运行不算通过**，
这正是方案 §3.2 要求的：不能用一堆绿点掩盖还没决策的部分。

前置：`python scripts/build.py`（产出 build/vspace_checks[.exe]）。
本脚本**不**重建任何二进制 —— 构建入口只有 scripts/build.py 一个。
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / ("vspace_checks.exe" if os.name == "nt" else "vspace_checks")
GROUPS = ("region", "stack", "host", "lifetime", "budget", "lea")
DEFAULT_TIMEOUT = 60.0

LINE = re.compile(
    r"^CASE (?P<id>\S+) group=(?P<group>\S+) expect=(?P<expect>\S+) "
    r"actual=(?P<actual>\S+) result=(?P<result>PASS|FAIL|BLOCKED)"
    r"(?: detail=(?P<detail>.*))?$"
)


def decode(raw: bytes) -> str:
    return raw.decode("utf-8", "replace").replace("\r\n", "\n")


def list_cases() -> list[tuple[str, str]]:
    done = subprocess.run([str(EXE), "--list"], cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=DEFAULT_TIMEOUT)
    if done.returncode != 0:
        raise RuntimeError(f"驱动 --list 失败（exit={done.returncode}）")
    cases: list[tuple[str, str]] = []
    for line in decode(done.stdout).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == "CASE":
            cases.append((parts[1], parts[2]))
    return cases


def run_case(case_id: str, group: str, override: list[str],
             timeout: float) -> dict:
    cmd = [str(EXE), "--case", case_id, *override]
    row = {"id": case_id, "group": group, "expect": None, "actual": None,
           "result": "FAIL", "detail": "", "exit": None, "note": ""}
    try:
        done = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        row["note"] = f"超时（>{timeout:g}s）"
        return row
    text = decode(done.stdout)
    row["exit"] = done.returncode
    for line in text.splitlines():
        m = LINE.match(line)
        if m and m.group("id") == case_id:
            row.update({k: v for k, v in m.groupdict().items() if v is not None})
            row["detail"] = m.group("detail") or ""
            break
    else:
        tail = " / ".join(text.strip().splitlines()[-2:]) or "(没有输出)"
        row["note"] = f"输出解析不了或驱动没跑到这条（exit={done.returncode}）：{tail}"
    if override:
        row["override"] = " ".join(override)
    return row


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--group", choices=GROUPS)
    ap.add_argument("--case")
    ap.add_argument("--report")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    ap.add_argument("--expect-value", type=int)
    ap.add_argument("--expect-trap", type=int)
    args = ap.parse_args()

    if not EXE.is_file():
        print(f"缺前置：{EXE.relative_to(ROOT)} 不存在，先跑 python scripts/build.py")
        return 2
    if args.expect_value is not None and args.expect_trap is not None:
        print("--expect-value 与 --expect-trap 互斥")
        return 2
    override: list[str] = []
    if args.expect_value is not None:
        override = ["--expect-value", str(args.expect_value)]
    elif args.expect_trap is not None:
        override = ["--expect-trap", str(args.expect_trap)]
    if override and not args.case:
        print("--expect-value / --expect-trap 只能和 --case 一起用（负对照），不能用于全量运行")
        return 2

    try:
        cases = list_cases()
    except (RuntimeError, subprocess.TimeoutExpired) as why:
        print(f"缺前置：{why}")
        return 2
    if args.case:
        cases = [c for c in cases if c[0] == args.case]
        if not cases:
            print(f"没有这条用例：{args.case}")
            return 2
    elif args.group:
        cases = [c for c in cases if c[1] == args.group]

    rows = [run_case(cid, group, override, args.timeout) for cid, group in cases]

    pass_n = sum(1 for r in rows if r["result"] == "PASS")
    fail_n = sum(1 for r in rows if r["result"] == "FAIL")
    blocked_n = sum(1 for r in rows if r["result"] == "BLOCKED")
    width = max((len(r["id"]) for r in rows), default=8)

    for r in rows:
        print(f"{r['result']:<7} {r['id']:<{width}} expect={r['expect'] or '-':<12} "
              f"actual={r['actual'] or '-':<12} {r['detail'] or r['note']}")
    blocked_ids = [r["id"] for r in rows if r["result"] == "BLOCKED"]
    if blocked_ids:
        print(f"未定（BLOCKED，等决策或未实施）：{', '.join(blocked_ids)}")
    print(f"合计 {len(rows)}：通过 {pass_n}，失败 {fail_n}，未定 {blocked_n}")

    exit_code = 0 if (fail_n == 0 and blocked_n == 0) else 1
    if args.report:
        report = {
            "runner": "scripts/check_vspace.py",
            "driver": str(EXE.relative_to(ROOT)),
            "filter": {"group": args.group, "case": args.case},
            "override": override,
            "counts": {"total": len(rows), "pass": pass_n, "fail": fail_n,
                       "blocked": blocked_n},
            "exit_code": exit_code,
            "cases": rows,
        }
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(
            json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"报告写入 {args.report}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
