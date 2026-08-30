#!/usr/bin/env python3
"""Regression tests for the M4.2 link manifest contract."""

from __future__ import annotations

import sys
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from backend_manifest import build_manifest, write_manifest  # noqa: E402


SOURCE = """\
#extern #proc bootstrap.allocate-pages(#bits<64> %size) -> #addr;
#extern #proc user_hook(#bits<32> %value) -> #unit;
#proc main() -> #bits<32> {
  #return 42
}
"""


def main() -> int:
    manifest = build_manifest(SOURCE, source="fixture.l1")
    assert manifest["schema"] == "lain.backend.link-manifest.v1"
    assert manifest["source"] == "fixture.l1"
    assert len(manifest["externs"]) == 2
    assert manifest["capabilities"] == ["bootstrap.allocate-pages", "foreign"]
    externs = {item["symbol"]: item for item in manifest["externs"]}
    assert externs["bootstrap.allocate-pages"]["capability"] == "bootstrap.allocate-pages"
    assert externs["user_hook"]["capability"] == "foreign"
    assert build_manifest(SOURCE, source="fixture.l1") == manifest
    temporary = ROOT / "build" / "backend_manifest_fixture.l1"
    output = ROOT / "build" / "backend_manifest_fixture.json"
    temporary.write_text(SOURCE, encoding="utf-8", newline="\n")
    try:
        written = write_manifest(temporary, output)
        assert "\\" not in written["source"]
        assert output.exists()
    finally:
        temporary.unlink(missing_ok=True)
        output.unlink(missing_ok=True)
    # Exercise the real verifier -> Lain backend -> C artifact path as a
    # compile-only smoke test.  Linking is intentionally left to M4.3.
    fixture = ROOT / "build" / "backend_fixture.l1"
    with tempfile.TemporaryDirectory(prefix="lain-backend-smoke-") as directory:
        generated = Path(directory) / "backend.c"
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "run_lain_backend.py"),
             str(fixture), "-o", str(generated)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        env = os.environ.copy()
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "local"))
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "global"))
        compiled = subprocess.run(
            ["zig", "cc", "-std=c11", "-O2", "-c", str(generated),
             "-o", str(Path(directory) / "backend.o")],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)

        # Exercise the non-trivial M4.2 subset: an extern prototype, a helper
        # procedure, a local binding, a conditional branch and an external call.
        full_fixture = ROOT / "tests" / "core" / "backend_manifest" / "backend_full.l1"
        full_generated = Path(directory) / "backend_full.c"
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "run_lain_backend.py"),
             str(full_fixture), "-o", str(full_generated)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        full_text = full_generated.read_text(encoding="utf-8")
        if "extern uint64_t user_hook(uintptr_t value);" not in full_text:
            raise RuntimeError("backend did not emit user extern prototype")
        if "uint32_t small=" not in full_text:
            raise RuntimeError("backend did not preserve local bits<32> width")
        if "/* unsupported L1:" in full_text:
            raise RuntimeError("backend left a supported fixture line unsupported")
        compiled = subprocess.run(
            ["zig", "cc", "-std=c11", "-O2", "-c", str(full_generated),
             "-o", str(Path(directory) / "backend_full.o")],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)
    print("backend link manifest positive/deterministic: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
