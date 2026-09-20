#!/usr/bin/env python3
"""编译 Meta 驱动，并跑一条程序证明它真能跑。

    python scripts/build.py

产物：build/meta_boot.exe

只有这两件事。没有参数、没有开关、没有别的脚本。

退出码：0 通过 / 1 失败 / 2 缺前置（没有 C 编译器或缺文件）。
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

DRIVER = "seed/tests/meta_boot.c"
MANIFEST = "bootstrap/SOURCE_ORDER"                      # Meta 的源清单
PRELUDE = "std/prelude.lain=bootstrap/lain/std/prelude.lain"
SMOKE = ("seed/tests/meta_for.lain", "main", "10")       # for 循环：sum(5) = 10


def find_cc():
    if shutil.which("zig"):
        return ["zig", "cc"]
    for name in ("clang", "gcc", "cc"):
        if shutil.which(name):
            return [name]
    return None


def main():
    cc = find_cc()
    if cc is None:
        print("build: 找不到 C 编译器（zig / clang / gcc 都没有）")
        return 2

    sources = sorted(
        str(p.relative_to(ROOT)).replace("\\", "/")
        for p in (ROOT / "seed" / "src").rglob("*.c")
    )
    needed = [DRIVER, SMOKE[0], MANIFEST, PRELUDE.split("=")[1], *sources]
    missing = [p for p in needed if not (ROOT / p).is_file()]
    if missing:
        print("build: 缺文件：" + ", ".join(missing))
        return 2

    out = BUILD / ("meta_boot.exe" if os.name == "nt" else "meta_boot")
    BUILD.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ,
        LAIN_META_MANIFEST=MANIFEST,
        ZIG_LOCAL_CACHE_DIR=str(BUILD / "zig-cache"),
        ZIG_GLOBAL_CACHE_DIR=str(BUILD / "zig-cache-global"),
    )

    cmd = cc + [
        "-std=c11", "-D_CRT_SECURE_NO_WARNINGS",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-Iseed/include", "-o", str(out), DRIVER, *sources,
    ]
    log = BUILD / "build.log"
    with open(log, "wb") as fh:
        done = subprocess.run(cmd, cwd=ROOT, env=env, stdout=fh, stderr=subprocess.STDOUT)
    if done.returncode != 0:
        print(f"build: 编译失败，日志在 {log.relative_to(ROOT)}")
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines()[:10]:
            print("  " + line)
        return 1
    print(f"built  {out.relative_to(ROOT)}   （{len(sources)} 个 seed/src + 驱动，{cc[0]}）")

    # 参数是：<源码> <入口> <期望值> <断言文本> <模式> [逻辑路径=文件]
    run = [str(out), SMOKE[0], SMOKE[1], SMOKE[2], "", "-", PRELUDE]
    done = subprocess.run(run, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    text = done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    if done.returncode != 0:
        print("smoke: 失败 —— 编出来的东西跑不动")
        for line in text.splitlines()[-6:]:
            print("  " + line)
        return 1
    print(f"smoke  {SMOKE[0]} -> {SMOKE[1]}() = {SMOKE[2]}   OK")
    print("BUILD OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
