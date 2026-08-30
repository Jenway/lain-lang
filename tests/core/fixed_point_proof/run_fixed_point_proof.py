#!/usr/bin/env python3
"""Regression tests for the M7.1 fixed-point proof report."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from prove_lainc_fixed_point import prove  # noqa: E402


LEFT = """\
#proc main() -> #bits<32> {
#return 0
}
#extern #proc host() -> #unit;
"""

RIGHT = """\
#extern #proc host() -> #unit;

#proc main() -> #bits<32> {
  #return 0
}
"""

NEGATIVE = RIGHT.replace("#return 0", "#return 1")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainc-proof-") as directory:
        root = Path(directory)
        left = root / "left.l1"
        right = root / "right.l1"
        negative = root / "negative.l1"
        left.write_text(LEFT, encoding="utf-8")
        right.write_text(RIGHT, encoding="utf-8")
        negative.write_text(NEGATIVE, encoding="utf-8")

        equal = prove(left, right)
        assert equal["canonical_equal"]
        assert equal["extern_equal"]
        assert equal["procedure_labels_equal"]
        assert not equal["header_mismatches"]
        assert not equal["body_mismatches"]

        changed = prove(left, negative)
        assert not changed["canonical_equal"]
        assert changed["body_mismatches"] == ["main"]
    print("fixed-point proof positive/negative: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
