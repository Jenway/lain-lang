#!/usr/bin/env python3
"""Build and freeze the current minimal Lain compiler artifact.

The frozen artifact is intentionally generated from the LAIN-IR-written
frontend bundle.  It is the first executable bootstrap lainc for the small
Lain subset currently implemented by that frontend.
"""

from __future__ import annotations

import subprocess
import sys
import hashlib
import json
import argparse
from datetime import datetime, timezone
from pathlib import Path

from build_lain_compiler import MODULES, OUTPUT as BUILD


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "scripts" / "build_lain_compiler.py"
ABI_VERSION = 1


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_closure_hash() -> tuple[str, list[dict[str, object]]]:
    entries: list[dict[str, object]] = []
    digest = hashlib.sha256()
    for path in MODULES:
        relative = path.relative_to(ROOT).as_posix()
        data = path.read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(data)
        entries.append(
            {
                "path": relative,
                "bytes": len(data),
                "sha256": sha256_bytes(data),
            }
        )
    return digest.hexdigest(), entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=BUILD)
    args = parser.parse_args()
    output = args.output
    if not output.is_absolute():
        output = ROOT / output
    result = subprocess.run(
        [sys.executable, str(BUILD_SCRIPT)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        sys.stderr.write(result.stderr or result.stdout)
        return result.returncode
    if output.resolve() != BUILD.resolve():
        raise SystemExit("bootstrap bundle output is fixed at build/bootstrap/lainc.l1")
    if not output.is_file():
        print(f"snapshot artifact is missing: {output}", file=sys.stderr)
        return 1
    source_hash, sources = source_closure_hash()
    artifact = output.read_bytes()
    manifest = output.with_suffix(output.suffix + ".snapshot.json")
    manifest.write_text(
        json.dumps(
            {
                "schema": "lain-bootstrap-snapshot-v1",
                "abi_version": ABI_VERSION,
                "created_at": datetime.now(timezone.utc).isoformat(),
                "artifact": output.relative_to(ROOT).as_posix()
                if output.is_relative_to(ROOT)
                else str(output),
                "artifact_bytes": len(artifact),
                "artifact_sha256": sha256_bytes(artifact),
                "source_closure_sha256": source_hash,
                "source_files": sources,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    try:
        shown = output.relative_to(ROOT)
    except ValueError:
        shown = output
    print(shown)
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
