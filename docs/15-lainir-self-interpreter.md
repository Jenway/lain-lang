# LAIN-IR self-interpreter

## Goal and boundary

The bootstrap branch implements the LAIN-IR parser, verifier, and interpreter
in LAIN-IR itself.  The core receives source bytes and scratch memory
explicitly and declares no external procedures.

```text
libc-only native driver
  -> source bytes and scratch memory
  -> LAIN-IR tokenizer/parser
  -> ProgramImage
  -> verifier
  -> interpreter
```

Only the native driver may use `#extern`, and every external name must be a real
libc symbol.  Custom C callbacks, capability bridges, and generic host
dispatchers are forbidden.

## Completed milestones

```text
S0  typed store; zext, sext, trunc
S1  stable ID-based ProgramImage v1
S2  explicit Arena, Value, Frame, and local storage
S3  expression evaluation and direct calls
S4  structured if/loop/break/continue/return
S5  checked guest allocation, address calculation, load, and store
S6  byte tokenizer, two-pass parser, symbol resolution, and verifier
S7  native file driver using only real libc procedures
S8  removal and audit of custom bootstrap callbacks
S9  nested self-interpretation and third-program execution
```

S9 is an execution proof, not only a source-closure check:

```text
stage-0 C interpreter
  executes the L1-written interpreter
    parses the interpreter core into ProgramImage
    executes that parsed interpreter
      parses and verifies a third LAIN-IR module
      executes its nested procedure
      checks that it returns 7
```

During S9 two latent defects were fixed:

- nested call parsing now buffers argument expression IDs locally before
  committing a contiguous range to `ProgramImage`;
- re-entering a structured region reinitializes an existing static LocalId
  instead of treating a loop's second `#let` execution as a duplicate local.

The verifier now checks direct-call argument counts and TypeIds.

## Validation

Run in the bootstrap worktree:

```powershell
zig build test-self-interpreter
```

This executes every S0-S9 test, the libc-only native driver, the external
boundary audit, the self-source closure test, and the nested execution proof.

## What this does and does not complete

Completed: a usable LAIN-IR execution substrate whose parser, verifier, and
interpreter logic are expressed in LAIN-IR.

Still separate: lowering the Lain compiler on `main` to a frozen LAIN-IR
compiler artifact.  Once that artifact exists, this bootstrap substrate can
execute it without importing a Racket translator or custom C compiler
services.
