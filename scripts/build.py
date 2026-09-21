#!/usr/bin/env python3
"""编译 seed 的测试驱动，并跑一条程序证明它真能跑。

    python scripts/build.py

产物：build/meta_boot.exe（Meta 驱动）、build/vspace_checks.exe（VM 边界用例驱动）、
build/eval_checks.exe（`#eval` 用例驱动）

只有「编译 + 冒烟」这一件事。没有参数、没有开关 —— 验收在 `check_meta.py`（Meta 用例）、
`check_vspace.py`（VSpace 边界用例）和 `check_eval.py`（`#eval` 用例）里，跟这一步分开：
那三个脚本要用这里的产物，这里不依赖它们，也不替它们跑。

退出码：0 通过 / 1 失败 / 2 缺前置（没有 C 编译器或缺文件）。
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

META_DRIVER = "seed/tests/meta_boot.c"
VSPACE_DRIVER = "seed/tests/vspace_checks.c"
EVAL_DRIVER = "seed/tests/eval_checks.c"
MANIFEST = "bootstrap/SOURCE_ORDER"                      # Meta 的源清单
PRELUDE = "std/prelude.lain=bootstrap/lain/std/prelude.lain"
SMOKE = ("seed/tests/meta_for.lain", "main", "10")       # for 循环：sum(5) = 10

# 严格告警：驱动是被测代码之外的第一个消费者，它自己先得干净。
CFLAGS = ["-std=c11", "-D_CRT_SECURE_NO_WARNINGS", "-Wall", "-Wextra",
          "-Wpedantic", "-Werror", "-Iseed/include"]


def find_cc():
    if shutil.which("zig"):
        return ["zig", "cc"]
    for name in ("clang", "gcc", "cc"):
        if shutil.which(name):
            return [name]
    return None


def sources():
    return sorted(
        str(p.relative_to(ROOT)).replace("\\", "/")
        for p in (ROOT / "seed" / "src").rglob("*.c")
    )


def exe(name):
    return BUILD / (name + ".exe" if os.name == "nt" else name)


def compile_driver(cc, env, driver, out, srcs):
    """编一个驱动。失败时把日志前几行打出来。"""
    cmd = cc + [*CFLAGS, "-o", str(out), driver, *srcs]
    log = BUILD / f"build-{out.stem}.log"
    with open(log, "wb") as fh:
        done = subprocess.run(cmd, cwd=ROOT, env=env, stdout=fh,
                              stderr=subprocess.STDOUT)
    if done.returncode != 0:
        print(f"build: 编译 {driver} 失败，日志在 {log.relative_to(ROOT)}")
        for line in log.read_text(encoding="utf-8",
                                  errors="replace").splitlines()[:12]:
            print("  " + line)
        return False
    print(f"built  {out.relative_to(ROOT)}   （{len(srcs)} 个 seed/src + "
          f"{Path(driver).name}，{cc[0]}）")
    return True


def main():
    cc = find_cc()
    if cc is None:
        print("build: 找不到 C 编译器（zig / clang / gcc 都没有）")
        return 2

    srcs = sources()
    needed = [META_DRIVER, VSPACE_DRIVER, EVAL_DRIVER, SMOKE[0], MANIFEST,
              PRELUDE.split("=")[1], *srcs]
    missing = [p for p in needed if not (ROOT / p).is_file()]
    if missing:
        print("build: 缺文件：" + ", ".join(missing))
        return 2

    BUILD.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ,
        LAIN_META_MANIFEST=MANIFEST,
        ZIG_LOCAL_CACHE_DIR=str(BUILD / "zig-cache"),
        ZIG_GLOBAL_CACHE_DIR=str(BUILD / "zig-cache-global"),
    )

    meta_out = exe("meta_boot")
    if not compile_driver(cc, env, META_DRIVER, meta_out, srcs):
        return 1
    if not compile_driver(cc, env, VSPACE_DRIVER, exe("vspace_checks"), srcs):
        return 1
    if not compile_driver(cc, env, EVAL_DRIVER, exe("eval_checks"), srcs):
        return 1

    # 参数是：<源码> <入口> <期望值> <断言文本> <模式> [逻辑路径=文件]
    run = [str(meta_out), SMOKE[0], SMOKE[1], SMOKE[2], "", "-", PRELUDE]
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
