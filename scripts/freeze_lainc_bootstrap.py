#!/usr/bin/env python3
"""Build and freeze the current minimal Lain compiler artifact.

The frozen artifact is intentionally generated from the LAIN-IR-written
frontend bundle.  It is the first executable bootstrap lainc for the small
Lain subset currently implemented by that frontend.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "lainir" / "lain_compiler.l1"
FROZEN = ROOT / "bootstrap" / "frozen" / "lainc.l1"
BUILD_SCRIPT = ROOT / "scripts" / "build_lain_compiler.py"


def main() -> int:
    result = subprocess.run(
        [sys.executable, str(BUILD_SCRIPT)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        sys.stderr.write(result.stderr or result.stdout)
        return result.returncode
    FROZEN.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(BUILD, FROZEN)
    print(FROZEN.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
