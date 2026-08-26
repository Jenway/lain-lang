#!/usr/bin/env python3
"""Profile the Lain-written compiler bootstrap at subprocess-stage granularity.

The profiler deliberately stays outside the compiler.  This keeps generated
LAIN-IR deterministic while making the expensive bootstrap steps observable.
It writes per-step stdout/stderr logs, a machine-readable JSON report, and a
short Markdown summary under build/profiles/.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
PRINT = BIN / f"lainir-print{SUFFIX}"
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
ARCHIVE = ROOT / "src" / "compiler-archive"


@dataclass
class Step:
    name: str
    seconds: float
    returncode: int
    command: list[str]
    stdout_log: str
    stderr_log: str
    output_bytes: int | None = None


def run_step(
    name: str,
    command: list[Path | str],
    report_dir: Path,
    output: Path | None = None,
    extra_env: dict[str, str] | None = None,
) -> Step:
    args = [str(value) for value in command]
    stdout_path = report_dir / f"{name}.stdout.log"
    stderr_path = report_dir / f"{name}.stderr.log"
    print(f"[{name}] START", flush=True)
    started = time.perf_counter()
    with stdout_path.open("w", encoding="utf-8") as stdout_file, stderr_path.open(
        "w", encoding="utf-8"
    ) as stderr_file:
        process_env = os.environ.copy()
        if extra_env:
            process_env.update(extra_env)
        process = subprocess.Popen(
            args,
            cwd=ROOT,
            stdout=stdout_file,
            stderr=stderr_file,
            text=True,
            env=process_env,
        )
        while True:
            try:
                returncode = process.wait(timeout=5)
                break
            except subprocess.TimeoutExpired:
                elapsed = time.perf_counter() - started
                print(f"[{name}] running {elapsed:8.1f}s", flush=True)
    seconds = time.perf_counter() - started
    output_bytes = output.stat().st_size if output and output.exists() else None
    print(
        f"[{name}] END rc={returncode} time={seconds:.3f}s"
        + (f" output={output_bytes}B" if output_bytes is not None else ""),
        flush=True,
    )
    return Step(
        name=name,
        seconds=seconds,
        returncode=returncode,
        command=args,
        stdout_log=stdout_path.name,
        stderr_log=stderr_path.name,
        output_bytes=output_bytes,
    )


def write_reports(report_dir: Path, steps: list[Step], fixed_point: bool | None) -> None:
    total = sum(step.seconds for step in steps)
    payload = {
        "created_at": datetime.now().astimezone().isoformat(),
        "root": str(ROOT),
        "total_seconds": total,
        "fixed_point": fixed_point,
        "steps": [asdict(step) for step in steps],
    }
    (report_dir / "profile.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )
    ranked = sorted(steps, key=lambda step: step.seconds, reverse=True)
    compile_steps = [step for step in steps if step.name.startswith("compile_")]
    verify_steps = [step for step in steps if step.name.startswith("verify_")]
    compile_seconds = sum(step.seconds for step in compile_steps)
    verify_seconds = sum(step.seconds for step in verify_steps)
    lines = [
        "# Lain bootstrap profile",
        "",
        f"Generated: {payload['created_at']}",
        "",
        f"Total measured subprocess time: {total:.3f} s",
        "",
        f"Fixed point (gen2 == gen3): {fixed_point}",
        "",
        "## Time distribution",
        "",
        "| Rank | Step | Seconds | Share | Result | Output |",
        "| ---: | --- | ---: | ---: | ---: | ---: |",
    ]
    for rank, step in enumerate(ranked, 1):
        share = (step.seconds / total * 100.0) if total else 0.0
        size = f"{step.output_bytes} B" if step.output_bytes is not None else "-"
        lines.append(
            f"| {rank} | `{step.name}` | {step.seconds:.3f} | "
            f"{share:.1f}% | {step.returncode} | {size} |"
        )
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            f"- Compile processes consumed {compile_seconds:.3f} s "
            f"({compile_seconds / total * 100.0:.1f}% of measured time).",
            f"- Verifier processes consumed {verify_seconds:.3f} s "
            f"({verify_seconds / total * 100.0:.3f}% of measured time).",
            "- Similar gen2 and gen3 times indicate a stable hot path rather than "
            "one anomalous bootstrap generation.",
            "- The next useful trace is procedure and opcode counts in the seed "
            "interpreter, grouped by LAIN-IR procedure. That can locate repeated "
            "scanner, lookup, and lowering work without changing compiler output "
            "or breaking the byte-for-byte fixed-point check.",
            "- Per-step stdout and stderr logs are stored beside this report.",
            "",
        ]
    )
    (report_dir / "analysis.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--include-archive",
        action="store_true",
        help="also compile and verify the tokenizer+syntax archive chain",
    )
    parser.add_argument(
        "--trace-stage",
        choices=("gen1", "gen2", "gen3"),
        help="write seed interpreter procedure/opcode counters for one stage",
    )
    parser.add_argument(
        "--trace-gen1",
        action="store_true",
        help="deprecated alias for --trace-stage gen1",
    )
    parser.add_argument(
        "--stop-after-gen1",
        action="store_true",
        help="only compile and verify gen1 (useful with --trace-gen1)",
    )
    parser.add_argument(
        "--stop-after-gen2",
        action="store_true",
        help="compile and verify through gen2, then stop",
    )
    parser.add_argument(
        "--compiler-artifact",
        type=Path,
        default=FROZEN,
        help="compiler artifact used for gen1 (defaults to frozen lainc.l1)",
    )
    args = parser.parse_args()
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    report_dir = ROOT / "build" / "profiles" / f"lainc-{stamp}"
    report_dir.mkdir(parents=True)
    steps: list[Step] = []
    fixed_point: bool | None = None

    with tempfile.TemporaryDirectory(prefix="lainc-profile-") as directory:
        tmp = Path(directory)
        gen1 = tmp / "gen1.l1"
        gen2 = tmp / "gen2.l1"
        gen3 = tmp / "gen3.l1"

        commands = [
            (
                "compile_gen1",
                [SEED, args.compiler_artifact, "compiler_compile", gen1, LAINC],
                gen1,
            ),
            ("verify_gen1", [PRINT, gen1, "compiler_compile"], None),
            (
                "compile_gen2",
                [SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC],
                gen2,
            ),
            ("verify_gen2", [PRINT, gen2, "compiler_compile"], None),
            (
                "compile_gen3",
                [SEED, "interpreter", gen2, "compiler_compile", gen3, LAINC],
                gen3,
            ),
            ("verify_gen3", [PRINT, gen3, "compiler_compile"], None),
        ]
        if args.stop_after_gen1:
            commands = commands[:2]
        elif args.stop_after_gen2:
            commands = commands[:4]
        trace_stage = "gen1" if args.trace_gen1 else args.trace_stage
        for name, command, output in commands:
            extra_env = None
            if name == f"compile_{trace_stage}":
                extra_env = {
                    "LAINIR_TRACE_PROFILE": str(
                        report_dir / f"{trace_stage}-internal.tsv"
                    )
                }
            step = run_step(name, command, report_dir, output, extra_env)
            steps.append(step)
            if step.returncode:
                write_reports(report_dir, steps, fixed_point)
                print(f"profile: {report_dir}", flush=True)
                return step.returncode

        if not args.stop_after_gen1 and not args.stop_after_gen2:
            fixed_point = gen2.read_bytes() == gen3.read_bytes()
            print(f"[fixed_point] gen2 == gen3: {fixed_point}", flush=True)

        if args.include_archive and not args.stop_after_gen1 and not args.stop_after_gen2:
            entry = tmp / "entry.lain"
            entry.write_text(
                'let syntax: Module = import("packages::lain::compiler::syntax");\n'
                "let sy: Module = syntax.Syntax(0);\n"
                "let main = std::func() -> i32 {\n    return 0;\n};\n",
                encoding="utf-8",
                newline="\n",
            )
            chain = tmp / "chain.l1"
            archive_steps = [
                (
                    "compile_archive_tokenizer_syntax",
                    [
                        SEED,
                        "interpreter",
                        gen2,
                        "compiler_compile_library",
                        chain,
                        ARCHIVE / "tokenizer.lain",
                        ARCHIVE / "syntax.lain",
                        entry,
                    ],
                    chain,
                ),
                ("verify_archive_tokenizer_syntax", [PRINT, chain, "main"], None),
            ]
            for name, command, output in archive_steps:
                step = run_step(name, command, report_dir, output)
                steps.append(step)
                if step.returncode:
                    break

    write_reports(report_dir, steps, fixed_point)
    print(f"profile: {report_dir}", flush=True)
    if fixed_point is False:
        return 1
    return next((step.returncode for step in steps if step.returncode), 0)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
