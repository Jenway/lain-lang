#!/usr/bin/env python3
"""src/lainc stage-E acceptance: archive factory chain compiles clean.

Compiles the real archive files tokenizer.lain + syntax.lain (the
cross-file module factory chain: syntax's `tokenizer.Tokenizer(Memory)`
evaluates at compile time) plus a factory-calling entry, and requires the
product to pass lainir-print (verifier-valid, no garbage).
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
PRINT = BIN / f"lainir-print{SUFFIX}"
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
ARCHIVE = ROOT / "src" / "compiler-archive"
STD = (
    ROOT / "std" / "memory_model.lain",
    ROOT / "std" / "allocation.lain",
    ROOT / "std" / "bounds.lain",
    ROOT / "std" / "effect.lain",
    ROOT / "std" / "core" / "vec.lain",
    ROOT / "std" / "core" / "string.lain",
    ROOT / "std" / "core" / "memory.lain",
)


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in arguments], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-e-") as directory:
        tmp = Path(directory)
        gen1 = tmp / "gen1.l1"
        gen2 = tmp / "gen2.l1"
        empty = tmp / "empty.lain"
        empty.write_text("", encoding="utf-8")

        compiled = run(SEED, FROZEN, "compiler_compile", gen1, LAINC)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)
        gen2_run = run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC)
        if gen2_run.returncode:
            raise RuntimeError(gen2_run.stderr or gen2_run.stdout)

        entry = tmp / "entry.lain"
        with open(entry, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                'let syntax: Module = import("packages::lain::compiler::syntax");\n'
                "let sy: Module = syntax.Syntax(0);\n"
                "let main = std::func() -> i32 {\n    return 0;\n};\n"
            )

        out = tmp / "chain.l1"
        res = run(
            SEED, "interpreter", gen2, "compiler_compile_library", out,
            *STD,
            ARCHIVE / "tokenizer.lain", ARCHIVE / "syntax.lain", entry,
        )
        if res.returncode:
            raise RuntimeError(f"chain: {res.stderr or res.stdout}")
        checked = run(PRINT, out, "main")
        if checked.returncode:
            raise RuntimeError(f"chain l1check: {checked.stderr}")
        text = out.read_text(encoding="utf-8")
        assert "#proc f0_Tokenizer_tokenize(" in text, "tokenizer members missing"
        assert "#proc f0_Syntax_parse(" in text, "syntax members missing"
        print("PASS archive factory chain (tokenizer+syntax) l1check clean")
    print("PASS src/lainc stage E: archive factory chain")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
