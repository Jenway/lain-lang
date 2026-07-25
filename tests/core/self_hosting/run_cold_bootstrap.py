#!/usr/bin/env python3
"""Rebuild stage1, stage2, and stage3 from the pinned stage0 commit.

This gate deliberately ignores artifact stamps and the bootstrap worktree.
The seed is exported from Git into a temporary read-only input tree, proving
that the recorded commit—not uncommitted local state—can recover lainc.
"""

from __future__ import annotations

import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE / "bootstrap_seed.json"
OUT = ROOT / "build/core-self-hosting"


def run(
    args: list[str], *, env: dict[str, str] | None = None, binary: bool = False
) -> subprocess.CompletedProcess:
    return subprocess.run(
        args,
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=not binary,
    )


def require(result: subprocess.CompletedProcess, label: str) -> None:
    if result.returncode == 0:
        return
    stdout = result.stdout if isinstance(result.stdout, str) else ""
    stderr = result.stderr if isinstance(result.stderr, str) else ""
    detail = (stderr or stdout).strip()
    raise RuntimeError(f"{label}: {detail}")


def tool(name: str) -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def main() -> int:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    bootstrap_repo = pathlib.Path(
        os.environ.get("LAIN_BOOTSTRAP_REPOSITORY", ROOT.parent / "lain-bootstrap")
    ).resolve()
    commit = manifest["commit"]
    safe = f"safe.directory={bootstrap_repo.as_posix()}"
    resolved = run(
        ["git", "-c", safe, "-C", str(bootstrap_repo), "rev-parse", commit]
    )
    require(resolved, "resolve pinned bootstrap seed")
    if resolved.stdout.strip() != commit:
        raise RuntimeError(
            f"bootstrap seed mismatch: expected {commit}, got {resolved.stdout.strip()}"
        )

    archive = run(
        [
            "git",
            "-c",
            safe,
            "-C",
            str(bootstrap_repo),
            "archive",
            "--format=tar",
            commit,
        ],
        binary=True,
    )
    require(archive, "archive pinned bootstrap seed")

    with tempfile.TemporaryDirectory(prefix="lain-cold-bootstrap-") as tmp:
        seed = pathlib.Path(tmp) / "stage0"
        seed.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive.stdout), mode="r:") as tar:
            tar.extractall(seed)

        env = os.environ.copy()
        env["LAIN_BOOTSTRAP_ROOT"] = str(seed)
        env["LAIN_SELF_HOST_FORCE_REBUILD"] = "1"
        for script, label in (
            ("build_meta_artifact.py", "stage0 -> stage1"),
            ("build_self_hosted_compiler.py", "stage1 -> stage2"),
            ("build_stage3_compiler.py", "stage2 -> stage3"),
        ):
            require(
                run([sys.executable, str(HERE / script)], env=env),
                label,
            )

    stage2 = OUT / "stage2_compiler.l1"
    stage3 = OUT / "stage3_compiler.l1"
    if stage2.read_bytes() != stage3.read_bytes():
        raise RuntimeError("cold bootstrap did not reach the stage2/stage3 byte fixed point")
    for artifact in (stage2, stage3):
        require(
            run([str(tool("l1check")), str(artifact), "compiler_compile"]),
            f"verify {artifact.name}",
        )
        schema = run(
            [str(tool("l1i")), str(artifact), "compiler_api_schema_version"]
        )
        require(schema, f"read {artifact.name} compiler API schema")
        if schema.stdout.strip() != str(manifest["compiler_api_schema"]):
            raise RuntimeError(
                f"{artifact.name} compiler API schema is {schema.stdout.strip()}"
            )

    print(
        "PASS cold bootstrap from pinned "
        f"{manifest['branch']}@{commit[:12]} reached stage2 == stage3"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
