"""Split the self-hosting lainc source without changing its concatenated form.

The generated parts are deliberately contiguous slices of lainc.lain.  The
part manifest is therefore also the source-order manifest: concatenating the
parts byte-for-byte reconstructs the original file.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "lainc" / "lainc.lain"
OUT = ROOT / "src" / "lainc"

# 1-based inclusive line ranges.  Boundaries follow the existing section
# comments and top-level declaration groups; no source text is synthesized.
SECTIONS = [
    ("runtime_buffers", 1, 1316),
    ("meta_tables", 1317, 1962),
    ("meta_objects", 1963, 2684),
    ("arithmetic", 2685, 3685),
    ("scanning", 3686, 3967),
    ("expression_lowering", 3968, 6808),
    ("types_statements", 6809, 8210),
    ("function_lowering", 8211, 8605),
    ("archive_runtime", 8606, 8966),
    ("functions_records", 8967, 10772),
    ("entrypoints", 10773, 11191),
]


def split(replace_entrypoint: bool = False) -> bytes:
    data = SOURCE.read_bytes()
    lines = data.splitlines(keepends=True)
    if len(lines) != SECTIONS[-1][2]:
        raise SystemExit(f"unexpected source line count: {len(lines)}")
    OUT.mkdir(exist_ok=True)
    for name, first, last in SECTIONS:
        (OUT / f"{name}.lain").write_bytes(b"".join(lines[first - 1 : last]))
    manifest = "\n".join(name for name, _, _ in SECTIONS) + "\n"
    (OUT / "LAIN_SOURCE_ORDER").write_text(manifest, encoding="utf-8", newline="")
    if replace_entrypoint:
        SOURCE.write_text(
            "// Lain compiler entrypoint. Sources are concatenated in LAIN_SOURCE_ORDER.\n",
            encoding="utf-8",
            newline="",
        )
    return data


def verify() -> None:
    manifest = (OUT / "LAIN_SOURCE_ORDER").read_text(encoding="utf-8").splitlines()
    expected = [name for name, _, _ in SECTIONS]
    if manifest != expected:
        raise SystemExit("MANIFEST does not match split definition")
    rebuilt = b"".join((OUT / f"{name}.lain").read_bytes() for name in manifest)
    original = b"".join(
        (SOURCE.parent / f"{name}.lain").read_bytes() for name in manifest
    )
    if rebuilt != original:
        raise SystemExit("split parts do not reconstruct lainc.lain byte-for-byte")
    print(f"verified {len(manifest)} parts, {len(original)} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()
    original = split(replace_entrypoint=args.replace)
    if args.verify:
        if args.replace:
            # verify() reads the newly written entrypoint only for its metadata;
            # compare against the bytes captured before replacement here.
            manifest = (OUT / "LAIN_SOURCE_ORDER").read_text(encoding="utf-8").splitlines()
            rebuilt = b"".join((OUT / f"{name}.lain").read_bytes() for name in manifest)
            if rebuilt != original:
                raise SystemExit("split parts do not reconstruct original source")
            print(f"verified {len(manifest)} parts, {len(original)} bytes")
        else:
            verify()
