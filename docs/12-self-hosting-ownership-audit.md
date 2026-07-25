# Historical M20.5 Self-hosting Ownership Audit

Snapshot: 2026-07-25.

> Historical note: the C host described below was removed from `main` after
> this audit.  The current branch is intentionally at a pure-Lain cutover
> point and is not buildable until the listed storage/parser/IR responsibilities
> are reimplemented in Lain.  The old implementation remains in Git history
> and `bootstrap/stage0`; it must not be copied back into `main`.

## Direct answer

Lain owns most compiler *policy*, but C still owns most compiler *physical
machinery*. Therefore most semantic compiler code is already self-hosted;
most C host code cannot yet be deleted.

The current system is:

```text
stage0 C + Scheme seed
  -> stage1 L1 compiler artifact
  -> stage2 Lain-compiled compiler artifact
  -> stage3 Lain-compiled compiler artifact
  -> stage2 == stage3
```

Scheme is not the evaluator or normal stage2/stage3 compiler policy. It is
used to recover stage1 from the cold seed.

## Ownership matrix

| Responsibility | Current owner | Can remove C/Scheme now? |
| --- | --- | --- |
| token/group reader and RawAst arena | C | No |
| surface-form interpretation | Lain | Yes, old Scheme copy is seed-only |
| types, nominal identity, specialization | Lain | Yes, policy is self-hosted |
| module summaries, dependency and resolution policy | Lain | Yes, policy is self-hosted |
| diagnostics and compilation result policy | Lain | Yes |
| Meta expansion and hygiene policy | Lain | Yes |
| expression/body/function lowering | Lain | Yes |
| comptime and structured L1 execution policy | Lain | Yes |
| generated syntax storage | C host | No |
| structured L1 storage and builder | C host | No |
| L1 verifier/parser/printer | C | No |
| strings and growable compiler storage | C host | No |
| CLI, artifact loading and capability registration | C | No |
| stage1 recovery | C + Scheme on `bootstrap/stage0` | No |

## What can be removed from the normal compiler path

The following are candidates for removal from a future prebuilt `lainc`
distribution, while remaining in the bootstrap branch/tool:

- Scheme VM initialization and the Scheme Meta libraries;
- stage0 `--bootstrap-emit-l1`;
- legacy `.lci` source-module metadata and source import scanning;
- the C reference execution path once differential tests can link it as a
  separate test tool.

This does not mean deleting their source today. The pinned seed must retain
enough C/Scheme to recreate stage1.

## What blocks deleting most C

The generated compiler artifact has 139 extern capabilities. They provide the
memory model on which the Lain compiler currently runs:

```text
44 SyntaxStore operations
62 L1Unit/evaluation-store operations
16 Meta syntax facade operations
10 core string/unit helpers
7 CompilerStorage operations
```

Deleting these would leave the self-hosted compiler without syntax storage,
IR construction, strings, vectors, verification or an artifact loader. A
native backend alone does not solve this; Lain first needs a runtime ABI and
owned allocation/container implementation that can host the compiler.

## Removal sequence

### Host reduction A: remove obsolete orchestration

- eliminate C source import scanning;
- replace `.lci` module metadata with the Lain-owned module artifact;
- make normal `lainc` load exactly one compiler artifact and one versioned
  capability table.

This can be done before a native backend.

### Host reduction B: compress the physical API

- expose opaque `SyntaxStore`, `L1Unit` and `CompilerStorage` handles;
- batch construction/query operations where ownership is clear;
- keep validation in C temporarily;
- require schema negotiation when loading the compiler artifact.

This reduces coupling without prematurely rewriting safe, mechanical code.

### Host reduction C: move runtime storage into Lain

Prerequisites:

- stable allocation/deallocation ABI;
- slices/vectors/maps and owned strings in the standard library;
- error-safe cleanup;
- ability to compile the compiler to a native executable.

Then move CompilerStorage, generated syntax storage and structured L1 storage
one subsystem at a time. Differential tests against the C implementations
remain until byte output and diagnostics match.

### Host reduction D: native compiler executable

After a backend can emit and link a standalone executable, build a native
stage2 compiler and use it to build native stage3. Only then can the C
artifact executor and most builder FFI be retired from the normal path.

## Bootstrap branch verdict

The branch layout is correct in principle: `bootstrap/stage0` contains the
C/Scheme cold seed and `main` contains the self-hosted compiler.

The repository now proves that a committed bootstrap seed is sufficient.
`bootstrap_seed.json` pins
`a11f26affc9ea6c53d4a5551129ccf5164f19b79`; the cold gate exports that commit
instead of reading the worktree and forces all three generations to rebuild.
The result passes `l1check`, exposes compiler API schema 1 and reaches a
byte-identical stage2/stage3 fixed point.

Therefore `bootstrap/stage0` is now a valid recovery seed for this main-line
compiler snapshot. Future changes to the compiler surface contract must update
the bootstrap commit and pass `zig build test-cold-bootstrap` before the seed
manifest moves.
