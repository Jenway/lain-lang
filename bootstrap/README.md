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

- `l1check`: parse, verify, and print canonical LAIN-IR.
- `l1i`: execute a LAIN-IR entry procedure.
- `l1bootstrap`: run an explicitly selected frozen compiler artifact.

Build from this directory with:

```text
zig build
```

The physical types are `BITS`, `FLOATS`, `SIMD`, `ADDR`, `UNIT`, and `NEVER`.
