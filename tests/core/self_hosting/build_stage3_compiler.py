#!/usr/bin/env python3
"""Build the stage-3 compiler with the stage-2 Lain compiler artifact."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from artifact_cache import (
    cache_matches,
    input_fingerprint as cache_fingerprint,
    write_stamp,
)
from build_meta_artifact import compiler_artifact_valid
from build_self_hosted_compiler import build as build_stage2
from compiler_source import build_compiler_source


ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/core-self-hosting/stage3_compiler.l1"
STAMP = ROOT / "build/core-self-hosting/stage3_compiler.stamp.json"
CACHE_SCHEMA = "lain-stage3-compiler-cache-v1"
FORCE_REBUILD = os.environ.get("LAIN_SELF_HOST_FORCE_REBUILD") == "1"


def tool(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def run(args: list[str]) -> None:
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def input_fingerprint(source: Path, stage2: Path) -> str:
    return cache_fingerprint(
        CACHE_SCHEMA,
        [
            ("builder", Path(__file__).read_bytes()),
            (
                "cache-contract",
                (Path(__file__).parent / "artifact_cache.py").read_bytes(),
            ),
            ("canonical-compiler-source", source.read_bytes()),
            ("stage2-artifact", stage2.read_bytes()),
            ("host-lainc", tool("lainc").read_bytes()),
            ("validator-l1check", tool("l1check").read_bytes()),
            ("schema-reader-l1i", tool("l1i").read_bytes()),
        ],
    )


def build() -> Path:
    stage2 = build_stage2()
    source = build_compiler_source()
    fingerprint = input_fingerprint(source, stage2)
    if (
        not FORCE_REBUILD
        and cache_matches(OUT, STAMP, CACHE_SCHEMA, fingerprint)
        and compiler_artifact_valid(OUT)
    ):
        return OUT

    OUT.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            str(tool("lainc")),
            "--artifact",
            str(stage2),
            "--emit-l1",
            str(source),
            str(OUT),
        ]
    )
    if not compiler_artifact_valid(OUT):
        raise RuntimeError(
            "stage-3 artifact lacks compiler_compile or compiler API schema 1"
        )
    write_stamp(OUT, STAMP, CACHE_SCHEMA, fingerprint)
    return OUT


if __name__ == "__main__":
    print(build().relative_to(ROOT))
