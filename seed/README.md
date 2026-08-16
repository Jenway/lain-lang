# Lain-IR bootstrap

This directory contains the small C runtime used to start the Lain-written
compiler artifact. It is split into layers so the IR model does not depend on
files, terminals, or command-line behavior.

```text
core <- text
  ^      ^
  |      |
interpreter
  ^
  |
host <- cli
```

- `include/lainir/core.h` and `src/core/` define and verify in-memory LAIN-IR.
- `src/text/` parses LAIN-IR text and emits it through `LainirWriter`.
- `src/interpreter/` executes an already parsed and verified module.
- `src/host/` adapts files and bootstrap capabilities to the library APIs.
- `src/cli/` contains the three command-line entry points.

The build produces:

- `lainir-print`: parse, verify, and print canonical LAIN-IR.
- `lainir-seed`: the C-written LAIN-IR interpreter.  Its `interpreter`
  subcommand runs an explicitly selected frozen compiler artifact with the
  bootstrap capabilities; its `run` subcommand executes a plain LAIN-IR
  entry procedure without capabilities.

The repository also carries the first frozen minimal Lain compiler at
`seed/frozen/lainc.l1`.  It is generated reproducibly from the current
LAIN-IR-written frontend bundle:

```text
python scripts/freeze_lainc_bootstrap.py
```

The frozen compiler currently covers the executable Lain subset exercised by
`tests/lainir_lain`, including functions, calls, modules, and the first
compile-time closure/module cases.  It is the bootstrap seed for the later
full `src/compiler` self-hosting closure; it is not yet the final `lainc`.

Build from this directory with:

```text
zig build
```

The physical types are `BITS`, `FLOATS`, `SIMD`, `ADDR`, `UNIT`, and `NEVER`.
