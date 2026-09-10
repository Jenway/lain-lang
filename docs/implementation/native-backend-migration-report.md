# Native backend migration status

Baseline: 2026-09-10.

This document records the evidence currently available for the native C
backend. It does not mark the migration complete while Meta execution is
disconnected from LAINVM.

## Verified now

- `scripts/check_backend_manifest.py` passes.
- `scripts/check_backend_abi_contract.py` passes.
- The seed project builds.
- The active bootstrap compiler rebuilds under `build/bootstrap/` and compiles
  the ordinary physical subset. Constant, arithmetic, call, local-binding and
  record-field fixtures execute with their expected results.
- The LAINIR/LAINVM boundary, temporary-TCB `#eval` contract and physical
  memory-safety checks pass.
- Native test scripts are valid Python and keep their outputs under `build/`
  or a temporary directory.

## Implemented and awaiting end-to-end execution

- atomic native artifact publication through a temporary file;
- native allocation accounting and allocation limits;
- additional C lowering for signed division, byte stores and `else` blocks;
- source offsets propagated from elaboration into LAINIR Builder calls;
- stable diagnostics for malformed calls and duplicate procedures;
- a matrix of successful programs and expected diagnostic failures;
- deterministic-output and historical-procedure comparison checks.

## Current blocker

The program-unit state and physical lowering implementation have been restored
without the removed evaluator protocol. Arithmetic `consteval`, pure scalar
Meta procedures, Effect factories with module arguments, and
`std::effect_operation(...)` now lower to temporary LAINIR containing `#eval`
and execute through LAINVM. The bootstrap compiler now builds and verifies the
complete formal standard-library source closure and the `src/lainc` source
closure. Two independent `srclainc.l1` builds are canonically identical across
357 procedures. The next gate is using that generated compiler to produce gen2
and gen3 artifacts, then proving the fixed point required by the native matrix.

The next required work is C4 in
[`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md): finish the LAINVM
handler boundary and replace the remaining Meta-call rejection with physical
`Value`-or-`Trap` execution. After that, the native matrix and fixed-point
checks must run before this migration can be marked complete.
