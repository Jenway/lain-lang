# Lain-written backend and native driver

`src/lainc/backend_c.lain` is an executable L1-to-C backend written in Lain.
It reads canonical L1 through the source capability and emits C through the
artifact capability.  `seed/src/host/native_lainc.c` supplies the small native
driver (argv, file I/O, allocation and artifact output); lowering policy and
text emission remain in Lain.

Run it with:

```text
python scripts/run_lain_backend.py build/backend_fixture.l1 -o build/backend_fixture.c

# Build a native compiler executable from the canonical L1 compiler
python scripts/build_lainc_native.py \
  build/archive-usable/gen-current-src-lainc-api-real15.l1 \
  build/lainc.exe
```

The backend currently handles:

- multiple procedures and parameters, with forward declarations;
- `#return`, `#let`, SSA assignments, `#call`, `#if`, `#loop`, `#break`,
  `#continue`, and `else` branches;
- scalar arithmetic/comparison, zero/sign-preserving casts, division/remainder;
- typed loads, stores, address arithmetic, and string literals;
- module/type-factory namespace expansion and multi-source namespace
  qualification for imported procedures;
- native multi-source `lainc.exe -o output.l1 source.lain ...` invocation.
- archive `Builder` lowering is emitted as concrete Lain procedures: unit,
  expression, call, return, and branch construction all write the physical
  `L1.Unit` model and no host-side Builder fallback is required.

Unsupported L1 lines are preserved as `/* unsupported L1: ... */` comments.
The native driver uses `-O2`; the generated compiler's metadata scans are
dramatically slower at `-O0`.  Artifact generation does not inject API,
Compiler, or Builder procedure stubs: those procedures must be emitted by the
Lain backend itself.  The archive's `Compiler.compile` and `API.compile` are
materialized in Lain, and the complete archive passes the L1 verifier and the
bootstrap interpreter on the empty-source API path.
