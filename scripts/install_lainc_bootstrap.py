#!/usr/bin/env python3
"""Install a verified lainc bootstrap package into a versioned prefix."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "scripts" / "check_lainc_bootstrap_snapshot.py"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--package-dir", type=Path, default=ROOT / "build" / "release" / "bootstrap"
    )
    parser.add_argument("--prefix", type=Path, default=ROOT / "build" / "install")
    args = parser.parse_args()
    package_dir = args.package_dir if args.package_dir.is_absolute() else ROOT / args.package_dir
    prefix = args.prefix if args.prefix.is_absolute() else ROOT / args.prefix
    package_file = package_dir / "package.json"
    if not package_file.is_file():
        raise SystemExit(f"bootstrap package metadata is missing: {package_file}")
    package = json.loads(package_file.read_text(encoding="utf-8"))
    if package.get("schema") != "lain-bootstrap-package-v1":
        raise SystemExit("unsupported bootstrap package schema")
    artifact_name = package.get("artifact")
    snapshot_name = package.get("snapshot_manifest")
    if not isinstance(artifact_name, str) or not isinstance(snapshot_name, str):
        raise SystemExit("bootstrap package has no artifact or snapshot manifest")
    artifact = package_dir / artifact_name
    snapshot = package_dir / snapshot_name
    if not artifact.is_file() or not snapshot.is_file():
        raise SystemExit("bootstrap package is incomplete")
    checked = subprocess.run(
        [sys.executable, str(CHECK), str(artifact), str(snapshot)], cwd=ROOT, text=True
    )
    if checked.returncode:
        return checked.returncode
    version = package.get("version")
    if not isinstance(version, str) or not version:
        raise SystemExit("bootstrap package has no version")
    destination = prefix / "lainc" / version
    destination.mkdir(parents=True, exist_ok=True)
    for name in (artifact_name, snapshot_name, "package.json"):
        shutil.copyfile(package_dir / name, destination / name)
    print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
