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
  entry procedure with only the allocator by default.  For compiler-API
  refeed tests, set `LAINIR_RUN_ARTIFACT` to a path; the run then exposes the
  three artifact-write capabilities and closes that file when the program
  finishes.

The generated Lain compiler bundle is not part of this directory; build it at
`build/bootstrap/lainc.l1` from the sources described in `bootstrap/README.md`.

Build from this directory with:

```text
zig build
```

The physical types are `BITS`, `FLOATS`, `SIMD`, `ADDR`, `UNIT`, and `NEVER`.
