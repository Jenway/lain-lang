#!/usr/bin/env python3
"""Compare native backend procedure output with the historical C baseline.

The historical driver and the current logical-capability driver intentionally
have different host prologues.  This gate therefore compares the procedure
signatures and bodies after removing host declarations and current trace
instrumentation.  It still fails when the backend changes a procedure's shape
or generated expression body.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "scripts" / "fixtures"
RUN_BACKEND = ROOT / "scripts" / "run_lain_backend.py"
INPUT = FIXTURES / "backend_constant_return.l1"
HISTORICAL = FIXTURES / "backend_constant_return.historical.c"


def function_body(text: str, name: str) -> str:
    match = re.search(
        rf"\b(?:uint32_t|int)\s+{re.escape(name)}\s*\([^)]*\)\s*\{{",
        text,
    )
    if match is None:
        raise RuntimeError(f"missing generated procedure: {name}")
    depth = 0
    for index in range(match.end() - 1, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[match.start() : index + 1]
    raise RuntimeError(f"unterminated generated procedure: {name}")


def canonical_procedure(text: str, name: str) -> str:
    body = function_body(text, name)
    # The current native driver can emit optional procedure tracing.  It is a
    # host diagnostic hook and has no bearing on the generated LAIN-IR body.
    body = re.sub(
        r"\s*if \(getenv\(\"LAIN_NATIVE_TRACE_PROCS\"\)\) \{.*?\}\s*",
        " ",
        body,
        flags=re.DOTALL,
    )
    body = body.replace("(void)", "()")
    body = re.sub(r"\s+", " ", body).strip()
    return body


def main() -> int:
    if not INPUT.is_file() or not HISTORICAL.is_file():
        print("missing backend canonical-diff fixture", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-backend-diff-", dir=ROOT / "build") as directory:
        current = Path(directory) / "current.c"
        result = subprocess.run(
            [sys.executable, str(RUN_BACKEND), str(INPUT), "-o", str(current)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            print(result.stderr or result.stdout, file=sys.stderr)
            return result.returncode
        historical = HISTORICAL.read_text(encoding="utf-8")
        generated = current.read_text(encoding="utf-8")
        names = ("helper", "lainc_entry")
        for name in names:
            expected = canonical_procedure(historical, name)
            actual = canonical_procedure(generated, name)
            if expected != actual:
                print(f"FAIL historical C procedure diff: {name}", file=sys.stderr)
                print(f"historical: {expected}", file=sys.stderr)
                print(f"current:    {actual}", file=sys.stderr)
                return 1
    print("PASS native backend historical C procedure diff")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
