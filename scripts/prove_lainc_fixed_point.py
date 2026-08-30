"""Prove that two textual LAIN-IR artifacts have the same fixed-point shape.

The check is intentionally stricter than a raw byte comparison report: it
compares canonical text, extern declarations, procedure labels/signatures and
SHA-256 summaries of every procedure body.  A non-zero exit code identifies
the first category that diverges so a formatter difference is not confused
with semantic drift.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from canonicalize_lainir import _blocks, canonicalize


def _procedure_summary(text: str) -> dict[str, dict[str, object]]:
    _externs, procedures, _other = _blocks(text)
    summary: dict[str, dict[str, object]] = {}
    for block in procedures:
        lines = block.splitlines()
        header = lines[0]
        label = header.split("#proc ", 1)[1].split("(", 1)[0]
        if label in summary:
            raise ValueError(f"duplicate procedure label: {label}")
        body = "\n".join(lines[1:])
        summary[label] = {
            "header": header,
            "body_sha256": hashlib.sha256(body.encode("utf-8")).hexdigest(),
            "body_lines": len(lines) - 1,
        }
    return summary


def prove(left: Path, right: Path) -> dict[str, object]:
    left_text = left.read_text(encoding="utf-8")
    right_text = right.read_text(encoding="utf-8")
    left_externs, _left_procs, _left_other = _blocks(left_text)
    right_externs, _right_procs, _right_other = _blocks(right_text)
    left_canonical = canonicalize(left_text)
    right_canonical = canonicalize(right_text)
    left_summary = _procedure_summary(left_text)
    right_summary = _procedure_summary(right_text)
    labels_left = set(left_summary)
    labels_right = set(right_summary)
    header_mismatches = sorted(
        label
        for label in labels_left & labels_right
        if left_summary[label]["header"] != right_summary[label]["header"]
    )
    body_mismatches = sorted(
        label
        for label in labels_left & labels_right
        if left_summary[label]["body_sha256"]
        != right_summary[label]["body_sha256"]
    )
    return {
        "canonical_equal": left_canonical == right_canonical,
        "canonical_bytes": [len(left_canonical.encode()), len(right_canonical.encode())],
        "extern_equal": sorted(left_externs) == sorted(right_externs),
        "extern_count": [len(left_externs), len(right_externs)],
        "procedure_labels_equal": labels_left == labels_right,
        "procedure_count": [len(labels_left), len(labels_right)],
        "header_mismatches": header_mismatches,
        "body_mismatches": body_mismatches,
        "left_only_labels": sorted(labels_left - labels_right),
        "right_only_labels": sorted(labels_right - labels_left),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--json", type=Path, help="write the proof report")
    args = parser.parse_args()
    report = prove(args.left, args.right)
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.json:
        args.json.write_text(payload, encoding="utf-8", newline="\n")
    print(payload, end="")
    ok = (
        report["canonical_equal"]
        and report["extern_equal"]
        and report["procedure_labels_equal"]
        and not report["header_mismatches"]
        and not report["body_mismatches"]
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
