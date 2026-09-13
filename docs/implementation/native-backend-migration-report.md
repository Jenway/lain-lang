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
362 procedures. The next gate is using that generated compiler to produce gen2
and gen3 artifacts, then proving the fixed point required by the native matrix.

The next required work is C4 in
[`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md): finish the LAINVM
handler boundary and replace the remaining Meta-call rejection with physical
`Value`-or-`Trap` execution. After that, the native matrix and fixed-point
checks must run before this migration can be marked complete.

## Updates since the 2026-09-10 baseline

The procedure count in the blocker above was 357 at that baseline and is 362
now: the generic facilities were removed (353) and the function declaration
shape rules moved into the standard library (362). Only the count changed; the
blocker is the same.

Four defects in the Lain-written C backend were found and fixed after this
baseline. All four produced wrong output rather than a failure, so none of the
checks listed above could have caught them:

- a line beginning `} else {` was matched as a prefix and the rest of the line
  discarded, so the else body and its closing brace were lost and the next
  function nested inside the previous one;
- `#data` and `#data_addr` were emitted as literal text rather than as a data
  object and its address;
- `emit_c_type` recognised five types and answered uint64_t for everything
  else, so `#bits<8>` and `#float<32>` were both 64-bit; it now fails on a type
  it cannot name instead of substituting one;
- an unrecognised expression was copied into the C verbatim, and three
  constructs leaked that way -- `#sdiv` emitted `L1_sdiv#a, b)`, so division in
  the backend was broken outright.

`scripts/check_backend_c_shape.py` now guards the first and third of these by
checking the emitted C directly, which is what none of the earlier checks did.

float operations remain unimplemented on purpose: this backend passes every
value as uintptr_t, so a float has no representation across a call boundary.
The construct is marked rather than lowered.

The stage named C4 above is 编码 5 in the current roadmap; the naming moved when
the roadmap was consolidated.
