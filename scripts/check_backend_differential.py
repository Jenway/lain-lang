#!/usr/bin/env python3
"""Compare the two C backends on the same LAINIR.

`seed/lainir/compiler.l1` and `src/lainc/backend_c.lain` both lower LAINIR to C,
and they must agree on what a program does.  Nothing compared them, so every
divergence found so far was found by hand -- a 16-bit load that read eight
bytes, a 16-bit store that wrote eight, a 32-bit load that read eight, a
mixed-precedence expression that dropped a term, and load/store helpers that
dereferenced a cast pointer instead of going through memcpy, so they panicked
on the unaligned addresses `#alloca` hands out.

Text comparison is not usable: the two differ in their helper names and in
whether `#addr` is `uint8_t *` or `uintptr_t`.  So the comparison is semantic.
Each side's C is built into an executable whose entry prints the returned value,
and the two values must be equal.

The seed names its C entry `main`, which collides with the harness, so that
side is compiled with `-Dmain=<alias>` in one step and linked with the harness
in another.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
SEED_EMITTER = ROOT / "seed" / "lainir" / "compiler.l1"
LAIN_BACKEND = ROOT / "build" / "backend_c_entry.l1"
FIXTURES = ROOT / "scripts" / "fixtures"

SEED_ALIAS = "lainc_seed_entry"

# Fixtures are LAINIR and must define `#proc main() -> #bits<32>`; the value it
# returns is what the two backends must agree on.
CASES = (
    "differential_load_store.l1",
    "differential_arithmetic_control.l1",
    "differential_bitwise_and_conversions.l1",
    "differential_unsigned_compare.l1",
    "differential_conversions.l1",
)

SEED_HARNESS = """\
#include <stdio.h>
#include <stdint.h>
int32_t {alias}(void);
int main(void) {{ printf("%lld\\n", (long long){alias}()); return 0; }}
"""

LAIN_HARNESS = """\
#include <stdio.h>
int lainc_entry(void);
int main(void) {{ printf("%lld\\n", (long long)lainc_entry()); return 0; }}
"""


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(part) for part in command],
        cwd=cwd or ROOT,
        capture_output=True,
        text=True,
    )


def lower_through_seed(fixture: Path, output: Path) -> None:
    result = run([SEED, SEED_EMITTER, "lainir_compile_module", output, fixture])
    if result.returncode or not output.is_file():
        raise RuntimeError(
            f"seed lowering of {fixture.name} failed: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )


def lower_through_lain(fixture: Path, output: Path) -> None:
    result = run([SEED, "interpreter", LAIN_BACKEND, "main", output, fixture])
    if result.returncode or not output.is_file():
        raise RuntimeError(
            f"Lain lowering of {fixture.name} failed: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )


def build_and_run(
    generated: Path, harness: Path, work: Path, label: str, rename_entry: bool
) -> str:
    """Compile one side to an executable and return what it printed."""
    object_file = work / f"{label}.o"
    # Debug, not -O2: the runtime alignment check is what makes an unaligned
    # load fail loudly, and optimised code drops the check and reads
    # unaligned memory "successfully" on x86.
    compile_flags = ["-std=c11"]
    if rename_entry:
        compile_flags.append(f"-Dmain={SEED_ALIAS}")
    emitted = run(
        ["zig", "cc", *compile_flags, "-c", "-o", object_file, generated]
    )
    if emitted.returncode:
        raise RuntimeError(
            f"{label}: compiling the generated C failed:\n"
            f"{emitted.stderr.strip()[:800]}"
        )
    harness_object = work / f"{label}_harness.o"
    built_harness = run(
        ["zig", "cc", "-std=c11", "-c", "-o", harness_object, harness]
    )
    if built_harness.returncode:
        raise RuntimeError(
            f"{label}: compiling the harness failed:\n"
            f"{built_harness.stderr.strip()[:400]}"
        )
    executable = work / f"{label}.exe"
    linked = run(["zig", "cc", "-o", executable, object_file, harness_object])
    if linked.returncode:
        raise RuntimeError(
            f"{label}: linking failed:\n{linked.stderr.strip()[:800]}"
        )
    executed = run([executable])
    if executed.returncode:
        raise RuntimeError(
            f"{label}: the program exited {executed.returncode}:\n"
            f"{executed.stderr.strip()[:800]}"
        )
    return executed.stdout.strip()


def main() -> int:
    for required in (SEED, SEED_EMITTER, LAIN_BACKEND):
        if not required.is_file():
            print(
                f"backend differential: {required.relative_to(ROOT)} is missing; "
                "build the seed and the backend entry first",
                file=sys.stderr,
            )
            return 2
    missing = [name for name in CASES if not (FIXTURES / name).is_file()]
    if missing:
        print("backend differential: missing fixtures: " + ", ".join(missing), file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="lain-differential-") as raw:
        work = Path(raw)
        seed_harness = work / "seed_harness.c"
        lain_harness = work / "lain_harness.c"
        seed_harness.write_text(
            SEED_HARNESS.format(alias=SEED_ALIAS), encoding="utf-8", newline="\n"
        )
        lain_harness.write_text(LAIN_HARNESS, encoding="utf-8", newline="\n")

        for name in CASES:
            try:
                fixture = FIXTURES / name
                seed_c = work / f"{fixture.stem}_seed.c"
                lain_c = work / f"{fixture.stem}_lain.c"
                lower_through_seed(fixture, seed_c)
                lower_through_lain(fixture, lain_c)
                from_seed = build_and_run(
                    seed_c, seed_harness, work, f"{fixture.stem}_seed", True
                )
                from_lain = build_and_run(
                    lain_c, lain_harness, work, f"{fixture.stem}_lain", False
                )
            except RuntimeError as failure:
                # A side that cannot be built or that aborts at run time is a
                # divergence too, and the usual cause is the backend emitting
                # something that only looks like C.
                print(f"{name}: {failure}", file=sys.stderr)
                return 1
            if from_seed != from_lain:
                print(
                    f"{name}: the two backends disagree: "
                    f"seed returned {from_seed!r}, Lain backend returned {from_lain!r}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {name}: both backends returned {from_seed}")
    print("PASS backend differential: both C backends agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
