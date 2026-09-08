#!/usr/bin/env python3
"""Profile the Lain-written compiler bootstrap at subprocess-stage granularity.

The profiler deliberately stays outside the compiler.  This keeps generated
LAIN-IR deterministic while making the expensive bootstrap steps observable.
It writes per-step stdout/stderr logs, a machine-readable JSON report, and a
short Markdown summary under build/profiles/.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

try:
    import psutil
except ImportError:  # pragma: no cover - optional profiling enhancement
    psutil = None


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
PRINT = BIN / f"lainir-print{SUFFIX}"
FROZEN = ROOT / "bootstrap" / "lainc.l1"
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
    peak_rss_bytes: int | None = None
    peak_vms_bytes: int | None = None
    diagnostic_count: int | None = None


def read_internal_trace(path: Path) -> dict[str, object] | None:
    """Summarize the optional seed procedure trace without changing artifacts."""
    if not path.exists():
        return None
    rows: list[dict[str, object]] = []
    total_calls = 0
    total_steps = 0
    meta_calls = 0
    specialization_calls = 0
    cache_hits = 0
    cache_misses = 0
    cache_seen = False
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        fields = line.split("\t")
        if len(fields) != 4:
            continue
        kind, name, calls_text, steps_text = fields
        if kind == "cache":
            cache_seen = True
            try:
                value = int(calls_text)
            except ValueError:
                continue
            if name == "meta_lookup_hits":
                cache_hits = value
            elif name == "meta_lookup_misses":
                cache_misses = value
            continue
        if kind != "procedure":
            continue
        try:
            calls = int(calls_text)
            steps = int(steps_text)
        except ValueError:
            continue
        total_calls += calls
        total_steps += steps
        if name.startswith("meta_") or name.startswith("Meta_"):
            meta_calls += calls
        if "special" in name or "factory" in name:
            specialization_calls += calls
        rows.append({"name": name, "calls": calls, "steps": steps})
    rows.sort(key=lambda row: int(row["steps"]), reverse=True)
    return {
        "file": path.name,
        "procedure_count": len(rows),
        "total_calls": total_calls,
        "total_steps": total_steps,
        "meta_call_count": meta_calls,
        "specialization_call_count": specialization_calls,
        "cache_hits": cache_hits,
        "cache_misses": cache_misses,
        "cache_instrumented": cache_seen,
        "top_procedures": rows[:25],
    }


def source_manifest(paths: list[Path]) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for path in paths:
        try:
            data = path.read_bytes()
        except OSError:
            continue
        result.append(
            {
                "path": str(path),
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    return result


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
    peak_rss = 0
    peak_vms = 0
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
        process_info = psutil.Process(process.pid) if psutil else None
        try:
            while True:
                try:
                    if process_info:
                        try:
                            memory = process_info.memory_info()
                            peak_rss = max(peak_rss, memory.rss)
                            peak_vms = max(peak_vms, memory.vms)
                        except psutil.Error:
                            pass
                    returncode = process.wait(timeout=5)
                    break
                except subprocess.TimeoutExpired:
                    if process_info:
                        try:
                            memory = process_info.memory_info()
                            peak_rss = max(peak_rss, memory.rss)
                            peak_vms = max(peak_vms, memory.vms)
                        except psutil.Error:
                            pass
                    elapsed = time.perf_counter() - started
                    print(f"[{name}] running {elapsed:8.1f}s", flush=True)
        except KeyboardInterrupt:
            # Ctrl-C should not leave a multi-minute seed compiler orphaned.
            # This is especially important on Windows, where the child does
            # not necessarily receive the console interrupt with its parent.
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
        if process_info:
            try:
                memory = process_info.memory_info()
                peak_rss = max(peak_rss, memory.rss)
                peak_vms = max(peak_vms, memory.vms)
            except psutil.Error:
                pass
    seconds = time.perf_counter() - started
    output_bytes = output.stat().st_size if output and output.exists() else None
    diagnostic_count = None
    try:
        stderr_text = stderr_path.read_text(encoding="utf-8")
        match = re.search(r"bootstrap summary .* diagnostics=(\d+)", stderr_text)
        if match:
            diagnostic_count = int(match.group(1))
    except OSError:
        pass
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
        peak_rss_bytes=peak_rss or None,
        peak_vms_bytes=peak_vms or None,
        diagnostic_count=diagnostic_count,
    )


def write_reports(
    report_dir: Path,
    steps: list[Step],
    fixed_point: bool | None,
    sources: list[dict[str, object]],
    trace: dict[str, object] | None,
) -> None:
    total = sum(step.seconds for step in steps)
    budget_threshold = 0.50
    budget_alerts = [
        step.name for step in steps
        if total > 0 and step.seconds / total > budget_threshold
    ]
    dominant_step = max(steps, key=lambda step: step.seconds).name if steps else None
    payload = {
        "created_at": datetime.now().astimezone().isoformat(),
        "root": str(ROOT),
        "total_seconds": total,
        "fixed_point": fixed_point,
        "sources": sources,
        "trace": trace,
        "cache": {
            "hits": trace.get("cache_hits") if trace else None,
            "misses": trace.get("cache_misses") if trace else None,
            "instrumented": bool(trace and trace.get("cache_instrumented")),
        },
        "budget": {
            "threshold_share": budget_threshold,
            "dominant_step": dominant_step,
            "alerts": budget_alerts,
        },
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
        "| Rank | Step | Seconds | Share | Result | Output | Peak RSS | Peak VMS | Diagnostics |",
        "| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for rank, step in enumerate(ranked, 1):
        share = (step.seconds / total * 100.0) if total else 0.0
        size = f"{step.output_bytes} B" if step.output_bytes is not None else "-"
        rss = f"{step.peak_rss_bytes / 1024 / 1024:.1f} MiB" if step.peak_rss_bytes else "-"
        vms = f"{step.peak_vms_bytes / 1024 / 1024:.1f} MiB" if step.peak_vms_bytes else "-"
        diagnostics = str(step.diagnostic_count) if step.diagnostic_count is not None else "-"
        lines.append(
            f"| {rank} | `{step.name}` | {step.seconds:.3f} | "
            f"{share:.1f}% | {step.returncode} | {size} | {rss} | {vms} | {diagnostics} |"
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
            (
                f"- Budget alert: `{dominant_step}` exceeds 50% of measured time; "
                "the next performance task should target this phase."
                if budget_alerts
                else "- No single phase exceeds the 50% performance budget."
            ),
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
    if trace:
        lines.extend(
            [
                "## Internal procedure trace",
                "",
                f"- Procedures: {trace['procedure_count']}",
                f"- Calls: {trace['total_calls']}",
                f"- Steps: {trace['total_steps']}",
                f"- Meta calls: {trace['meta_call_count']}",
                f"- Specialization/factory calls: {trace['specialization_call_count']}",
                f"- Meta lookup cache hits/misses: {trace['cache_hits']}/{trace['cache_misses']}",
                "",
                "| Procedure | Calls | Steps |",
                "| --- | ---: | ---: |",
            ]
        )
        for row in trace["top_procedures"][:10]:
            lines.append(f"| `{row['name']}` | {row['calls']} | {row['steps']} |")
        lines.append("")
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
    trace_summary: dict[str, object] | None = None
    sources = source_manifest([args.compiler_artifact, LAINC])

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
                trace_file = report_dir / f"{trace_stage}-internal.tsv" if trace_stage else None
                trace_summary = read_internal_trace(trace_file) if trace_file else None
                write_reports(report_dir, steps, fixed_point, sources, trace_summary)
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

    trace_file = report_dir / f"{trace_stage}-internal.tsv" if trace_stage else None
    trace_summary = read_internal_trace(trace_file) if trace_file else None
    write_reports(report_dir, steps, fixed_point, sources, trace_summary)
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
