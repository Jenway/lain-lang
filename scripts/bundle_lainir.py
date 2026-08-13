"""Concatenate LAIN-IR tool modules while de-duplicating externs.

LAIN-IR currently has no source-level import form.  This is only a build-time
module bundler; formatter, lexer, highlighting, and LSP logic remain LAIN-IR.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def bundle(inputs: list[Path]) -> str:
    defined: set[str] = set()
    for path in inputs:
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("#proc "):
                name = stripped[len("#proc "):].split("(", 1)[0]
                defined.add(name)
    # Names are the linkage identity.  Signatures may spell the same physical
    # type as `i64`/`#bits<64>` or `addr`/`#addr`, so comparing raw declaration
    # text would leave duplicate declarations in a bundle.
    seen_extern_names: set[str] = set()
    chunks: list[str] = []
    for path in inputs:
        lines = path.read_text(encoding="utf-8").splitlines()
        kept: list[str] = []
        index = 0
        while index < len(lines):
            line = lines[index]
            stripped = line.strip()
            if stripped.startswith("#extern #proc "):
                # Extern signatures are allowed to span lines.  Consume the
                # whole declaration before deciding whether to retain it;
                # dropping only the first line leaves an invalid bundle.
                name = stripped[len("#extern #proc "):].split("(", 1)[0]
                declaration: list[str] = [line]
                while ";" not in lines[index]:
                    index += 1
                    if index >= len(lines):
                        break
                    declaration.append(lines[index])
                signature = "\n".join(declaration)
                if name not in defined and name not in seen_extern_names:
                    seen_extern_names.add(name)
                    kept.extend(declaration)
                index += 1
                continue
            kept.append(line)
            index += 1
        chunks.append("\n".join(kept))
    return "\n\n".join(chunks) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(bundle(args.inputs), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
