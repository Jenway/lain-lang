# Lain-written backend and native driver

`src/lainc/backend_c.lain` is the intended Lain-written L1-to-C backend. Its
lowering policy and text emission remain in Lain, while
`seed/src/host/native_lainc.c` supplies the native driver. The backend source
still uses legacy `@foreign` declarations, so it is currently a migration
target rather than a runnable part of the active compiler closure.

Run it with:

```text
python scripts/run_lain_backend.py build/backend_fixture.l1 -o build/backend_fixture.c
# The command also writes build/backend_fixture.c.manifest.json by default;
# pass --manifest <path> to choose an explicit location.

# Build a native compiler executable from the canonical L1 compiler
python scripts/build_lainc_native.py \
  build/archive-usable/gen-current-src-lainc-api-real15.l1 \
  build/lainc.exe

# Optional native smoke (requires the built executable):
python tests/core/native_binary/run_native_smoke.py build/lainc.exe

# Execute the emitted L1 in the same native process (no external seed run):
python tests/core/native_binary/run_native_inprocess_smoke.py build/lainc.exe
```

The historical backend implementation handles:

- multiple procedures and parameters, with forward declarations;
- `#return`, `#let`, SSA assignments, `#call`, `#if`, `#loop`, `#break`,
  `#continue`, and `else` branches;
- scalar arithmetic/comparison, zero/sign-preserving casts, division/remainder;
- typed loads, stores, address arithmetic, and string literals;
- module/type-factory namespace expansion and multi-source namespace
  qualification for imported procedures;
- native multi-source `lainc.exe -o output.l1 source.lain ...` invocation.
- native `lainc.exe --run -o output.l1 source.lain ...` in-process L1 execution;
  the executable links the seed parser, verifier, and interpreter for this
  final validation path.
- archive `Builder` lowering is emitted as concrete Lain procedures: unit,
  expression, call, return, and branch construction all write the physical
  `L1.Unit` model and no host-side Builder fallback is required.

These capabilities are not covered by the current migration baseline because
compiling the backend source stops at `@foreign` with diagnostic 1001.
Unsupported L1 lines are preserved as `/* unsupported L1: ... */` comments.
The native driver uses `-O2`; the generated compiler's metadata scans are
dramatically slower at `-O0`.  Artifact generation does not inject API,
Compiler, or Builder procedure stubs: those procedures must be emitted by the
Lain backend itself.  The archive's `Compiler.compile` and `API.compile` are
materialized in Lain, and the complete archive passes the L1 verifier and the
bootstrap interpreter on the empty-source API path.
