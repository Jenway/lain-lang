#!/usr/bin/env python3
"""`#eval` 的验收入口（IR 层）。

    python scripts/check_eval.py                  # 跑全部用例
    python scripts/check_eval.py --list           # 列用例名
    python scripts/check_eval.py --case NAME      # 只跑一条
    # 负对照：把期望改成错的，必须失败（否则说明断言没生效）
    python scripts/check_eval.py --case block_ok --expect-verify 9999
    python scripts/check_eval.py --case block_addr_result --expect-verify 2019
    python scripts/check_eval.py --case block_engine_rejects --expect-trap 1099

每条用例**单独起一个进程**并带超时：崩溃、超时、无法解析输出都算失败，
不跟「用例真的失败了」混在一起。

退出码：0 全通过 / 1 有失败 / 2 缺前置（先跑 python scripts/build.py）。
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "build" / ("eval_checks.exe" if sys.platform == "win32" else "eval_checks")
TIMEOUT = 60


def driver(*args: str):
    return subprocess.run([str(DRIVER), *args], cwd=ROOT,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=TIMEOUT)


def case_names() -> list:
    done = driver("--list")
    if done.returncode != 0:
        return []
    text = done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    return [line.strip() for line in text.splitlines() if line.strip()]


def run_case(name: str, extra: list):
    """跑一条用例，返回 (是否通过, 说明)。"""
    try:
        done = driver("--case", name, *extra)
    except subprocess.TimeoutExpired:
        return False, f"超时（>{TIMEOUT}s）"
    text = done.stdout.decode("utf-8", "replace").replace("\r\n", "\n").strip()
    last = text.splitlines()[-1] if text else "(没有任何输出)"
    if done.returncode == 0 and last.startswith("PASS"):
        return True, last[5:].strip()
    if done.returncode == 2:
        return False, f"参数错误：{last}"
    return False, last


def main(argv: list) -> int:
    if not DRIVER.is_file():
        print(f"check_eval: 找不到 {DRIVER.relative_to(ROOT)}，先跑 python scripts/build.py")
        return 2

    if "--list" in argv:
        for name in case_names():
            print(name)
        return 0

    only = None
    extra = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--case" and i + 1 < len(argv):
            only = argv[i + 1]
            i += 2
        elif arg in ("--expect-verify", "--expect-trap", "--expect-text") and i + 1 < len(argv):
            extra += [arg, argv[i + 1]]
            i += 2
        else:
            print(f"check_eval: 不认识的参数 {arg}")
            return 2

    names = case_names()
    if not names:
        print("check_eval: 驱动没能列出用例（它自己坏了？）")
        return 2
    if only is not None:
        if only not in names:
            print(f"check_eval: 没有这个用例 {only}（--list 看全部）")
            return 2
        names = [only]

    failed = []
    for name in names:
        ok, note = run_case(name, extra)
        print(f"{'PASS' if ok else 'FAIL'}  {name:<22} {note}")
        if not ok:
            failed.append(name)

    print(f"--- 用例：{len(names) - len(failed)}/{len(names)} 通过 ---")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
