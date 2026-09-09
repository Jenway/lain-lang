#!/usr/bin/env python3
"""Capture a reproducible, machine-readable bootstrap baseline.

The baseline deliberately records both successful and failed gates.  A
verifier-valid artifact is not promoted to a "usable compiler" result here;
the existing test's return code remains the authority.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import psutil
except ImportError:  # pragma: no cover - optional memory measurement
    psutil = None


ROOT = Path(__file__).resolve().parents[1]
PYTHON = Path(sys.executable)

SMOKE_TESTS = (
    "scripts/check_lainc_lainir_api.py",
    "scripts/check_lainir_boundaries.py",
    "scripts/check_backend_manifest.py",
    "scripts/check_backend_abi_contract.py",
)

FULL_TESTS = SMOKE_TESTS + (
    "scripts/check_lainc_lainir_api_baseline.py",
)

SOURCE_ROOTS = (
    ROOT / "seed" / "src",
    ROOT / "src" / "lainir",
    ROOT / "src" / "lainc",
    ROOT / "std",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_manifest() -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    for root in SOURCE_ROOTS:
        if not root.exists():
            continue
        paths = [root] if root.is_file() else sorted(root.rglob("*"))
        for path in paths:
            if not path.is_file():
                continue
            try:
                entries.append(
                    {
                        "path": str(path.relative_to(ROOT)).replace("\\", "/"),
                        "bytes": path.stat().st_size,
                        "sha256": sha256(path),
                    }
                )
            except OSError:
                continue
    return entries


def process_memory(process: object) -> tuple[int, int]:
    if psutil is None:
        return 0, 0
    try:
        info = psutil.Process(process.pid)  # type: ignore[attr-defined]
    except (psutil.Error, AttributeError):
        return 0, 0
    rss = 0
    vms = 0
    processes = [info]
    try:
        processes.extend(info.children(recursive=True))
    except psutil.Error:
        pass
    for child in processes:
        try:
            memory = child.memory_info()
            rss += memory.rss
            vms += memory.vms
        except psutil.Error:
            continue
    return rss, vms


def terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if psutil is not None:
        try:
            children = psutil.Process(process.pid).children(recursive=True)
        except (psutil.Error, AttributeError):
            children = []
        for child in children:
            try:
                child.kill()
            except psutil.Error:
                pass
    process.kill()


def run_test(
    name: str, report_dir: Path, timeout_seconds: float | None
) -> dict[str, object]:
    stdout_path = report_dir / f"{name.replace('/', '_')}.stdout.log"
    stderr_path = report_dir / f"{name.replace('/', '_')}.stderr.log"
    command = [str(PYTHON), str(ROOT / name)]
    started = time.perf_counter()
    peak_rss = 0
    peak_vms = 0
    timed_out = False
    started_at = time.perf_counter()
    with stdout_path.open("w", encoding="utf-8", newline="\n") as stdout, stderr_path.open(
        "w", encoding="utf-8", newline="\n"
    ) as stderr:
        process = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr)
        while process.poll() is None:
            rss, vms = process_memory(process)
            peak_rss = max(peak_rss, rss)
            peak_vms = max(peak_vms, vms)
            if timeout_seconds is not None and time.perf_counter() - started_at >= timeout_seconds:
                timed_out = True
                terminate_process_tree(process)
                break
            time.sleep(0.25)
        rss, vms = process_memory(process)
        peak_rss = max(peak_rss, rss)
        peak_vms = max(peak_vms, vms)
    result = {
        "name": name,
        "command": command,
        "returncode": process.returncode,
        "seconds": round(time.perf_counter() - started, 3),
        "stdout_log": stdout_path.name,
        "stderr_log": stderr_path.name,
        "peak_rss_bytes": peak_rss or None,
        "peak_vms_bytes": peak_vms or None,
        "timed_out": timed_out,
    }
    print(
        f"[{name}] rc={result['returncode']} time={result['seconds']:.3f}s",
        flush=True,
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--full", action="store_true", help="run the complete compiler gates too")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="directory for logs and baseline.json (default: build/baselines/<timestamp>)",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        help="stop an individual test after this many seconds and record a timeout",
    )
    args = parser.parse_args()

    stamp = datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    report_dir = args.output_dir or ROOT / "build" / "baselines" / stamp
    report_dir.mkdir(parents=True, exist_ok=True)
    tests = FULL_TESTS if args.full else SMOKE_TESTS
    results: list[dict[str, object]] = []
    for test in tests:
        result = run_test(test, report_dir, args.timeout_seconds)
        results.append(result)
        if result["returncode"] != 0:
            # Preserve the failure and stop before a later, dependent test
            # turns the root cause into a second failure.
            break

    payload = {
        "schema": "lain-bootstrap-baseline-v1",
        "created_at": datetime.now().astimezone().isoformat(),
        "root": str(ROOT),
        "python": str(PYTHON),
        "mode": "full" if args.full else "smoke",
        "tests_requested": list(tests),
        "tests_run": results,
        "all_passed": bool(results) and all(item["returncode"] == 0 for item in results),
        "source_manifest": source_manifest(),
    }
    output = report_dir / "baseline.json"
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(output)
    return 0 if payload["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
