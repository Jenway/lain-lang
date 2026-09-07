#!/usr/bin/env python3
"""Check logical capability classification in backend manifests."""

from __future__ import annotations

from backend_manifest import build_manifest


def main() -> int:
    manifest = build_manifest(
        "\n".join(
            (
                "#extern #proc backend.source_count() -> #bits<64>;",
                "#extern #proc bootstrap.artifact-begin() -> #unit;",
                "#extern #proc vendor_symbol() -> #unit;",
            )
        )
    )
    by_symbol = {item["symbol"]: item["capability"] for item in manifest["externs"]}
    expected = {
        "backend.source_count": "backend.source_count",
        "bootstrap.artifact-begin": "bootstrap.artifact-begin",
        "vendor_symbol": "foreign",
    }
    if by_symbol != expected:
        print(f"FAIL backend manifest classification: {by_symbol}")
        return 1
    if manifest["capabilities"] != sorted(expected.values()):
        print(f"FAIL backend manifest capability set: {manifest['capabilities']}")
        return 1
    print("PASS backend manifest capability classification")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
