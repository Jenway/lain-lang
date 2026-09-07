# Native backend migration report

Baseline: 2026-09-07. This report records the current native backend boundary
for the `lainc` to LAINIR API migration.

## Current evidence

The input side is valid: `scripts/run_lain_backend.py` verifies a canonical
LAINIR input before attempting backend compilation. The backend source and
seed provider now use the logical capability ABI:

| Check | Result | Evidence |
| --- | --- | --- |
| Input artifact verification | pass | seed verifier accepts the formal constant-return artifact |
| Backend runner ABI preflight | pass | `run_lain_backend.py` validates the logical declaration inventory before compilation |
| Backend capability inventory | pass | `check_lain_backend_abi.py --report` finds exactly eight `backend.*` declarations |
| Active compiler backend compile | pass | `run_lain_compiler.py --library` emits a verified backend artifact |
| Seed backend adapter | pass | seed binds the eight logical names to provider-side bootstrap functions |
| Native C emission | pass | multi-procedure fixture emits C and a logical capability manifest |
| In-process execution | pass | generated C runs the fixture and returns `42` |

The native build entry still requires `build/backend_c_entry.l1`; the migration
gate now generates and verifies that artifact. A final clean native compiler
build remains a separate gate.

The eight declarations are mapped in
[`lain-backend-capability-abi.md`](lain-backend-capability-abi.md). Their host
link names remain provider/driver details; the intended source-level names are
the `backend.*` logical capabilities.

## Required completion gate

The executable gate now covers the following checks for a multi-procedure
fixture:

1. compile `backend_c.lain` with the active compiler;
2. verify the generated LAINIR and capability manifest;
3. emit C and produce the logical capability manifest;
4. build and run the native in-process smoke.

The current fixture gate is automated by:

```text
python scripts/check_native_backend_migration.py
```

The historical canonical C diff and final clean native compiler build remain
separate completion checks.

Until then, `backend_c.lain` remains outside `COMPILER_SOURCES.txt`, and the
source-boundary check must continue to enforce that separation.
