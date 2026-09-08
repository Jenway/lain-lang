#!/usr/bin/env python3
"""Verify packaging and installation of the lainc bootstrap release."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "scripts" / "package_lainc_bootstrap.py"
INSTALL = ROOT / "scripts" / "install_lainc_bootstrap.py"
CHECK = ROOT / "scripts" / "check_lainc_bootstrap_snapshot.py"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str]) -> None:
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainc-release-") as directory:
        root = Path(directory)
        package_dir = root / "package"
        prefix = root / "prefix"
        run([sys.executable, str(PACKAGE), "--output-dir", str(package_dir)])
        package = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
        run(
            [
                sys.executable,
                str(INSTALL),
                "--package-dir",
                str(package_dir),
                "--prefix",
                str(prefix),
            ]
        )
        installed = prefix / "lainc" / package["version"]
        run(
            [
                sys.executable,
                str(CHECK),
                str(installed / package["artifact"]),
                str(installed / package["snapshot_manifest"]),
            ]
        )
        for name in (package["artifact"], package["snapshot_manifest"], "package.json"):
            if sha256(package_dir / name) != sha256(installed / name):
                raise RuntimeError(f"installed bootstrap file differs: {name}")
    print("PASS lainc bootstrap release package and install")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, KeyError, json.JSONDecodeError) as error:
        print(f"FAIL lainc bootstrap release: {error}", file=sys.stderr)
        raise SystemExit(1)
