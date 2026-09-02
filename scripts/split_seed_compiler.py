"""Split seed/lainir/compiler.l1 into ordered, lossless source sections."""

from __future__ import annotations

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "seed" / "lainir" / "compiler.l1"
OUT = ROOT / "seed" / "lainir" / "compiler_parts"
SECTIONS = [
    ("parser_interface", "#proc byte_at("),
    ("ir_access", "#proc seed_procedure_is_main("),
    ("c_emitter", "#proc seed_emit_c_type("),
    ("integer_literal", "#proc emit_c_i64("),
    ("command_entry", "#proc lainir_compile("),
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    data = SOURCE.read_bytes()
    lines = data.splitlines(keepends=True)
    starts = []
    for index, (name, marker) in enumerate(SECTIONS):
        if index == 0:
            starts.append(0)
            continue
        matches = [i for i, line in enumerate(lines) if line.decode("utf-8").startswith(marker)]
        if len(matches) != 1:
            raise SystemExit(f"could not find unique marker for {name}: {marker}")
        starts.append(matches[0])
    OUT.mkdir(exist_ok=True)
    for index, (name, _) in enumerate(SECTIONS):
        first = starts[index]
        last = starts[index + 1] if index + 1 < len(starts) else len(lines)
        (OUT / f"{name}.l1").write_bytes(b"".join(lines[first:last]))
    (OUT / "SOURCE_ORDER").write_text(
        "\n".join(name for name, _ in SECTIONS) + "\n",
        encoding="utf-8",
        newline="",
    )
    rebuilt = b"".join((OUT / f"{name}.l1").read_bytes() for name, _ in SECTIONS)
    if args.verify and rebuilt != data:
        raise SystemExit("compiler parts do not reconstruct compiler.l1")
    print(f"wrote {len(SECTIONS)} parts, {len(data)} bytes")
    if args.verify:
        print("verified byte-for-byte reconstruction")


if __name__ == "__main__":
    main()
