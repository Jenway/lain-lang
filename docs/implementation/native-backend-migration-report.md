# Native backend migration report

Baseline: 2026-09-07. This report records the current native backend boundary
for the `lainc` to LAINIR API migration.

## Current evidence

The input side is valid: `scripts/run_lain_backend.py` verifies a canonical
LAINIR input before attempting backend compilation. The backend source side is
not migrated yet:

| Check | Result | Evidence |
| --- | --- | --- |
| Input artifact verification | pass | seed verifier accepts the formal constant-return artifact |
| Backend runner ABI preflight | expected failure | `run_lain_backend.py` stops before compilation while legacy inventory is non-empty |
| Backend capability inventory | expected failure | `check_lain_backend_abi.py --report` finds eight legacy `@foreign` declarations |
| Active compiler backend compile | blocked | diagnostic `1001 unexpected character near '@'` |
| Frozen seed compiler backend compile | blocked | the same diagnostic, so this is not a compiler selection error |
| Formal compiler bundle backend compile | blocked | the same diagnostic at line 10, confirming the gap is shared by the current source frontend |
| Native C emission | not reached | backend L1 is not produced |
| In-process execution | not reached | no migrated backend artifact exists |

The native build entry has the same dependency explicitly: `build_lainc_native.py`
requires `build/backend_c_entry.l1`. That file is absent while backend source
compilation is blocked, so native clean build cannot be treated as an
independent failure.

The eight declarations are mapped in
[`lain-backend-capability-abi.md`](lain-backend-capability-abi.md). Their host
link names remain provider/driver details; the intended source-level names are
the `backend.*` logical capabilities.

## Required completion gate

The report is complete only when all four commands below succeed for a
multi-procedure fixture:

1. compile `backend_c.lain` with the active compiler;
2. verify the generated LAINIR and capability manifest;
3. emit C and compare it with the historical backend output;
4. build and run the native in-process smoke.

Until then, `backend_c.lain` remains outside `COMPILER_SOURCES.txt`, and the
source-boundary check must continue to enforce that separation.
