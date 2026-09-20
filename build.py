#!/usr/bin/env python3
"""构建第二代 seed 的驱动，并跑一条冒烟程序证明它真的能跑。

**这是 `scripts/` 里唯一保留的脚本。** 它自包含：不 import 其它脚本、不读 fixtures、
不依赖 `build/` 里已有的任何产物。把别的都删掉之后它照样能用。

它做的事只有两件：

  1. 用本地 C 编译器把 `seed/src/**/*.c` + `seed/tests/meta_boot.c` 编成
     `build/meta_boot[.exe]`；
  2. 拿 `seed/tests/meta_source.lain`（`let main = 42;`）跑一遍，确认产物真的能跑出 42。

第 2 步不是装饰：**没有它，"编译成功"只等于"clang 没报错"**，不证明 Meta 能被装载、
能读源码、能产出 LAINIR、能被执行。

用法::

    python scripts/build.py
    python scripts/build.py --cc clang        # 换编译器
    python scripts/build.py --out build/x.exe # 换输出路径

退出码：0 = 构建并冒烟通过，1 = 失败，2 = 缺前置（没有 C 编译器 / 源文件缺失）。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DRIVER_SRC = "seed/tests/meta_boot.c"
SMOKE_SRC = "seed/tests/meta_source.lain"
SMOKE_ENTRY = "main"
SMOKE_WANT = 42

CFLAGS = [
    "-std=c11",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Iseed/include",
]

EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def pick_cc(explicit: str | None) -> list[str] | None:
    """挑一个 C 编译器。仓库约定是 `zig cc`；没有 zig 就退回系统 clang/gcc。"""
    if explicit:
        return explicit.split()
    if shutil.which("zig"):
        return ["zig", "cc"]
    for candidate in ("clang", "gcc", "cc"):
        if shutil.which(candidate):
            return [candidate]
    return None


def cc_env() -> dict:
    """`zig cc` 的缓存目录固定到 build/ 下，别写进用户目录。"""
    env = dict(os.environ)
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(BUILD / "zig-cache"))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(BUILD / "zig-cache-global"))
    return env


def seed_sources() -> list[str]:
    """`seed/src/**/*.c`，按路径排序（确定性的来源：同一份源码给出同一条命令）。"""
    return sorted(
        str(p.relative_to(ROOT)).replace("\\", "/")
        for p in (ROOT / "seed" / "src").rglob("*.c")
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cc", default=None, help='覆盖 C 编译器，例如 --cc clang')
    ap.add_argument("--out", default=None, help="输出路径，默认 build/meta_boot[.exe]")
    args = ap.parse_args()

    cc = pick_cc(args.cc)
    if cc is None:
        print("build: 找不到 C 编译器（zig / clang / gcc 都没有）")
        return 2

    sources = [DRIVER_SRC, *seed_sources()]
    missing = [p for p in sources + [SMOKE_SRC] if not (ROOT / p).is_file()]
    if missing:
        print("build: 缺源文件: " + ", ".join(missing))
        return 2

    out = Path(args.out) if args.out else BUILD / f"meta_boot{EXE_SUFFIX}"
    out.parent.mkdir(parents=True, exist_ok=True)

    cmd = cc + CFLAGS + ["-o", str(out), *sources]
    print(f"compiler: {' '.join(cc)}")
    print(f"sources:  {len(sources)} 个（{DRIVER_SRC} + seed/src 下 {len(sources) - 1} 个）")
    log = BUILD / "build.log"
    with open(log, "wb") as fh:
        proc = subprocess.run([str(a) for a in cmd], cwd=str(ROOT), env=cc_env(),
                              stdout=fh, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print(f"build: 编译失败（rc={proc.returncode}），日志 {log.relative_to(ROOT)}：")
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines()[:12]:
            print("  " + line)
        return 1
    print(f"built:    {out.relative_to(ROOT)}")

    # 冒烟：真的跑一条程序。参数是 `<src> <入口> <期望值> [断言文本] [模式] [逻辑路径=文件]...`。
    smoke = [str(out), SMOKE_SRC, SMOKE_ENTRY, str(SMOKE_WANT), "", "-"]
    proc = subprocess.run([str(a) for a in smoke], cwd=str(ROOT), env=cc_env(),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    if proc.returncode != 0:
        print(f"smoke:    失败（rc={proc.returncode}），{SMOKE_SRC} 没跑出 {SMOKE_WANT}：")
        for line in text.splitlines()[-8:]:
            print("  " + line)
        return 1
    print(f"smoke:    {SMOKE_SRC} -> {SMOKE_ENTRY}() = {SMOKE_WANT}  OK")
    print()
    print(f"==== BUILD OK ({out.name}) ====")
    return 0


if __name__ == "__main__":
    sys.exit(main())
