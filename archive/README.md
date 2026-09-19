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

Build from the repository root with:

```text
python scripts/build_seed.py
```

The binaries land in `build/seed/bin/` (with the `.exe` suffix on Windows).
`seed/` itself keeps only sources: neither build output nor Zig cache
directories are created inside it.

The physical types are `BITS`, `FLOATS`, `SIMD`, `ADDR`, `UNIT`, and `NEVER`.

## `archive/std/` — the two Lain sources bound to the first-generation host ABI

Moved out of `std/` on 2026-09-20. The rule applied: **anything that does not match the second
generation goes to `archive/`** — there is no compatibility to preserve (no users, no release).
The mechanical test for "does not match" is whether a file has `@foreign` bound to a
first-generation host capability name (`lain_ast_v1_*`, `tool_*`, or the dash-named
`bootstrap.*`).

| File | Lines | Why it is here |
| --- | --- | --- |
| `std/meta.lain` | 4675 | **178** `@abi_export`, **61** `@foreign`, all pointing at the first-generation host (30 × `lain_ast_v1_*`, 25 × `tool_*`, `raw_parse`, 5 × dash-named `bootstrap.*`) |
| `std/bootstrap/abi_entry.lain` | 1070 | First-generation Meta ABI entry; imports `std::meta` and calls through the dash-named capabilities |

The second-generation host (`seed/src/meta/host.c`) exposes **11** capabilities. The intersection
with those 61 is **empty**, so these two files are not "not yet finished" — they are a different ABI.

The 23 files left in `std/` (2027 lines) contain **zero** `@foreign`: `core/{arena,memory,result,
slice,source,string,vec}.lain`, `allocation/backend/bounds/control/diagnostic/effect/intrusive/
mem/memory_model/type_policy.lain`, `platform/*`, `bootstrap/{ast_tree,ir_builder,type_shape}.lain`.
They are the **goal**, not first-generation interface code — the second generation cannot parse
them yet only because its input language is still a v0 subset.

Live consumers that now point at the moved files: `src/lainc/meta.lain:15`,
`src/lainc/types.lain:13` (both `import("std::meta")`).

