# Native backend migration status

Baseline: 2026-09-10.

This document records the evidence currently available for the native C
backend. It does not mark the migration complete while the active bootstrap
compiler cannot be rebuilt.

## Verified now

- `scripts/check_backend_manifest.py` passes.
- `scripts/check_backend_abi_contract.py` passes.
- The seed project builds.
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

`scripts/build_lain_compiler.py` cannot verify the newly generated bootstrap
bundle. The first reported missing procedure is
`program_unit_meta_step_inc`; a complete inventory shows that the deleted old
lowering module also supplied the rest of the program-unit state and lowering
implementation. Restoring that file unchanged would restore the prohibited
`EvalResult` evaluator.

The next required work is C4 in
[`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md): finish the LAINVM
handler boundary and rebuild the bootstrap lowering around physical
`Value`-or-`Trap` execution. After that, the native matrix and fixed-point
checks must run before this migration can be marked complete.
