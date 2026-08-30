#!/usr/bin/env python3
"""Check the machine-readable seed trace/cache counters."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from profile_lainc_bootstrap import Step, read_internal_trace, write_reports  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainir-trace-contract-") as directory:
        path = Path(directory) / "trace.tsv"
        path.write_text(
            "section\tname\tcalls\tsteps\n"
            "procedure\tmeta_lookup\t7\t31\n"
            "cache\tmeta_lookup_hits\t5\t0\n"
            "cache\tmeta_lookup_misses\t2\t0\n"
            "summary\ttotal\t0\t31\n",
            encoding="utf-8",
        )
        trace = read_internal_trace(path)
        if not trace:
            raise RuntimeError("trace was not parsed")
        if trace["cache_hits"] != 5 or trace["cache_misses"] != 2:
            raise RuntimeError(f"cache counters mismatch: {trace}")
        if trace["cache_instrumented"] is not True:
            raise RuntimeError("cache trace was not marked instrumented")

        report_dir = Path(directory) / "profile"
        report_dir.mkdir()
        write_reports(
            report_dir,
            [
                Step("compile_gen1", 8.0, 0, [], "", ""),
                Step("verify_gen1", 1.0, 0, [], "", ""),
            ],
            None,
            [],
            trace,
        )
        profile = (report_dir / "profile.json").read_text(encoding="utf-8")
        if '"dominant_step": "compile_gen1"' not in profile:
            raise RuntimeError("dominant phase was not recorded")
        if '"compile_gen1"' not in profile:
            raise RuntimeError("50% budget alert was not recorded")
        analysis = (report_dir / "analysis.md").read_text(encoding="utf-8")
        if "Budget alert" not in analysis:
            raise RuntimeError("budget guidance was not written")
    print("trace cache counter contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
