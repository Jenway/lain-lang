#!/usr/bin/env python3
"""Exercise the first public user-Meta attribute transformation slice."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


def compiler() -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out" / "bin" / f"lainc{suffix}"


def compile_case(
    source_name: str,
    expected_error: str | None = None,
) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="lain-meta-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        result = subprocess.run(
            [
                str(compiler()),
                "--emit-l1",
                str(FIXTURES / source_name),
                str(output),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        diagnostic = (result.stderr or result.stdout).strip()
        if expected_error is not None:
            ok = (
                result.returncode != 0
                and not output.exists()
                and expected_error in diagnostic
            )
            return ok, diagnostic or f"missing {expected_error}"
        if result.returncode != 0 or not output.exists():
            return False, diagnostic or "no output"
        return True, output.read_text(encoding="utf-8")


def main() -> int:
    failed = 0
    cases = (
        ("one replacement form", "meta_identity.lain", None, "#proc main("),
        (
            "constructed replacement form",
            "meta_construct.lain",
            None,
            "#return 1",
        ),
        (
            "hygiene prevents call-site capture",
            "meta_hygiene_no_capture.lain",
            "error 2003",
            None,
        ),
        (
            "fresh identifiers share expansion context",
            "meta_hygiene_fresh_binding.lain",
            None,
            "#return %arg0",
        ),
        ("zero replacement forms", "meta_empty.lain", None, "#proc main("),
        ("two replacement forms", "meta_two.lain", "error 2111", None),
        (
            "invalid signature",
            "meta_invalid_signature.lain",
            "error 2901",
            None,
        ),
        (
            "invalid result",
            "meta_invalid_result.lain",
            "error 2903",
            None,
        ),
        (
            "unknown attribute",
            "meta_unknown_attribute.lain",
            "error 2902",
            None,
        ),
    )
    for label, source, error, expected_text in cases:
        ok, detail = compile_case(source, error)
        if ok and expected_text is not None:
            ok = expected_text in detail
        print(f"{'PASS' if ok else 'FAIL'} {label}: {detail}")
        failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
