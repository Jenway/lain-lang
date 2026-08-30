"""Verify canonical LAIN-IR comparison rules used by fixed-point gates."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from canonicalize_lainir import canonicalize  # noqa: E402


def main() -> int:
    first = """#extern #proc zeta() -> #unit;\r\n#extern #proc alpha() -> #unit;\r\n\r\n#proc main() -> #bits<32> {\r\n  #return 42   \r\n}\r\n"""
    second = """#extern #proc alpha() -> #unit;\n\n#proc main() -> #bits<32> {\n#return 42\n}\n#extern #proc zeta() -> #unit;\n"""
    if canonicalize(first) != canonicalize(second):
        raise RuntimeError("equivalent artifacts have different canonical forms")
    changed = second.replace("42", "43")
    if canonicalize(first) == canonicalize(changed):
        raise RuntimeError("canonicalizer erased a procedure body change")
    print("PASS artifact canonicalizer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
