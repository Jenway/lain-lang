#!/usr/bin/env python3
"""Package the checked-in lainc snapshot for a bootstrap release."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "bootstrap" / "lainc.l1"
SNAPSHOT = ROOT / "bootstrap" / "lainc.l1.snapshot.json"
CHECK = ROOT / "scripts" / "check_lainc_bootstrap_snapshot.py"
VERSION = ROOT / "VERSION"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir", type=Path, default=ROOT / "build" / "release" / "bootstrap"
    )
    args = parser.parse_args()
    output = args.output_dir if args.output_dir.is_absolute() else ROOT / args.output_dir
    checked = subprocess.run(
        [sys.executable, str(CHECK), str(ARTIFACT)], cwd=ROOT, text=True
    )
    if checked.returncode:
        return checked.returncode
    output.mkdir(parents=True, exist_ok=True)
    packaged_artifact = output / ARTIFACT.name
    packaged_snapshot = output / SNAPSHOT.name
    shutil.copyfile(ARTIFACT, packaged_artifact)
    snapshot = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    snapshot["artifact"] = packaged_artifact.name
    packaged_snapshot.write_text(
        json.dumps(snapshot, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    package = {
        "schema": "lain-bootstrap-package-v1",
        "version": VERSION.read_text(encoding="utf-8").strip(),
        "artifact": packaged_artifact.name,
        "snapshot_manifest": packaged_snapshot.name,
        "artifact_sha256": snapshot["artifact_sha256"],
        "source_closure_sha256": snapshot["source_closure_sha256"],
        "abi_version": snapshot["abi_version"],
    }
    (output / "package.json").write_text(
        json.dumps(package, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
