#!/usr/bin/env python3
"""Archive API refeed gate for a simple arithmetic user return expression."""

from __future__ import annotations

import os

from run_lainc_full_std_archive import main as run_full_archive


def main() -> int:
    os.environ.setdefault(
        "LAIN_FULL_FIXTURE",
        "tests/lainir_lain/fixtures/compiler_api_compile_emit_arithmetic.lain",
    )
    os.environ.setdefault("LAIN_FULL_STD_OUT", "build/debug-refeed-arithmetic.l1")
    os.environ.setdefault(
        "LAIN_FULL_REFEED_ARTIFACT", "build/debug-refed-arithmetic.l1"
    )
    os.environ.setdefault("LAIN_FULL_REFEED_EXPECTED", "3")
    return run_full_archive()


if __name__ == "__main__":
    raise SystemExit(main())
