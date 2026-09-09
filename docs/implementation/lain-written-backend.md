# Lain-written C backend

`src/lainc/backend_c.lain` contains the Lain-written LAINIR-to-C lowering.
`seed/src/host/native_lainc.c` supplies the native process entry and the host
operations used to publish compiler artifacts. The backend remains outside
`src/lainc/COMPILER_SOURCES.txt`; it is a separately compiled backend module.

The backend source currently covers procedure declarations, calls, structured
branches and loops, integer arithmetic and comparison, casts, typed memory
operations, address arithmetic, and string data. The pending changes also add
signed division, byte stores, complete `else` emission, and removal of an
accidental NUL byte in unsupported-instruction comments.

The native host writes each artifact to `<output>.tmp` and renames it only
after a successful compile. Failed requests remove the temporary file. It also
tracks compiler allocations and applies the configured allocation limit.

The intended executable checks are:

```text
python scripts/check_native_backend_migration.py
python scripts/check_native_backend_canonical_diff.py
python scripts/check_native_lainc_matrix.py build/lainc.exe
python scripts/check_native_formal_stdlib.py build/lainc.exe
python scripts/check_native_lainc_determinism.py build/lainc.exe
```

These checks are not currently green. The active hand-written bootstrap source
cannot rebuild `build/bootstrap/lainc.l1` because removal of the old
`EvalResult` evaluator also removed the only complete bootstrap lowering
module. C4 must reconnect Meta execution through LAINVM before the native
compiler and the matrix can be rebuilt and executed.

Generated compiler bundles, C files, manifests and executables belong under
`build/`. No generated backend artifact is stored in `src/` or `bootstrap/`.
