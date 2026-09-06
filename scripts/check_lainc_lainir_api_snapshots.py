#!/usr/bin/env python3
"""Check fixed compiler artifacts and failed-compilation atomicity.

The broader stdlib conformance suite compares bootstrap and formal compilers.
This gate instead keeps a small, checked-in canonical artifact baseline for the
formal compiler, so a coordinated regression in both implementations remains
observable during the LAINC -> LAINIR API migration.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from bundle_lainir import bundle
from check_stdlib_conformance import (
    CORE,
    FORMAL_STDLIB,
    IMPORT_DIAGNOSTIC_CASES,
    ROOT,
    compile_sources,
    compile_status,
)


SNAPSHOT_CASES = (
    "formal_constant_return",
    "formal_arithmetic_return",
    "formal_struct_field_return",
)
SNAPSHOTS = ROOT / "scripts" / "fixtures" / "lainc_api_migration"
SOURCES = ROOT / "scripts" / "fixtures"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def main() -> int:
    required = (CORE, FORMAL_STDLIB, *(SNAPSHOTS / f"{name}.l1" for name in SNAPSHOT_CASES))
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError("missing snapshot inputs: " + ", ".join(missing))

    with tempfile.TemporaryDirectory(prefix="lainc-api-snapshots-") as directory:
        work = Path(directory)
        compiler = work / "formal_compiler.l1"
        compiler.write_text(bundle([CORE, FORMAL_STDLIB]), encoding="utf-8", newline="\n")

        for name in SNAPSHOT_CASES:
            output = work / f"{name}.l1"
            result = compile_sources(compiler, (SOURCES / f"{name}.lain",), output)
            if result.returncode:
                detail = result.stderr.strip() or result.stdout.strip()
                raise RuntimeError(f"{name}: compiler failed: {detail}")
            if read(output) != read(SNAPSHOTS / f"{name}.l1"):
                raise RuntimeError(f"{name}: canonical artifact differs from snapshot")
            print(f"PASS {name}: canonical artifact snapshot")

        for name, sources, status in IMPORT_DIAGNOSTIC_CASES:
            output = work / f"{name}.l1"
            observed = compile_status(compiler, sources, output)
            if observed != status:
                raise RuntimeError(f"{name}: expected diagnostic {status}, got {observed}")
            if output.exists():
                raise RuntimeError(f"{name}: failed compilation left an artifact")
            print(f"PASS {name}: diagnostic {status} and no artifact")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL LAINC API snapshots: {error}", file=sys.stderr)
        raise SystemExit(1)
