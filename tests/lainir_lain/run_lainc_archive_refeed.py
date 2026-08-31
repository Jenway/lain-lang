#!/usr/bin/env python3
"""Run the archive API smoke and re-feed its emitted L1 artifact.

The first seed process executes the generated API client.  The client writes
the `CompileResult.artifact` bytes through the opt-in run sink; a second seed
process then verifies and executes that file.
"""

from __future__ import annotations

import os
from pathlib import Path

from run_lainc_full_std_archive import main as run_full_archive


ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    os.environ.setdefault(
        "LAIN_FULL_FIXTURE",
        "tests/lainir_lain/fixtures/compiler_api_compile_emit_artifact.lain",
    )
    os.environ.setdefault("LAIN_FULL_STD_OUT", "build/debug-refeed-product.l1")
    os.environ.setdefault("LAIN_FULL_REFEED_ARTIFACT", "build/debug-refed-api.l1")
    os.environ.setdefault("LAIN_FULL_REFEED_EXPECTED", "42")
    return run_full_archive()


if __name__ == "__main__":
    raise SystemExit(main())
