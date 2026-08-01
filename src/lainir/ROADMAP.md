# LAIN-IR compiler roadmap

The implementation grows as executable vertical slices.  A phase is complete
only when stage 0 can execute it and a native C compiler can compile its
output.

## M1 — executable code-generation path

Status: complete.

- [x] LAIN-IR entry point reads source bytes through capabilities.
- [x] LAIN-IR code recognizes a decimal `#return`.
- [x] LAIN-IR code emits deterministic standalone C.
- [x] Generated C compiles and preserves the program exit status.
- [x] Invalid input produces a diagnostic and non-zero compiler status.
- [x] Recognize and validate the complete one-procedure grammar.
- [x] Separate source access, lexical trivia, matching, and C writer logic.

The stage-0 parser still consumes one compiler artifact, so these layers are
procedures in `compiler.l1`.  Physical source-file splitting belongs to M2,
after compiler-source bundling exists.

## M2 — parser complete

Status: complete.

- [x] Own a physical cursor/token representation in LAIN-IR memory.
- [x] Tokenize identifiers, numbers, strings, hash names, punctuation,
  comments, and EOF accepted by `bootstrap/src/text/parser.c`.
- [x] Report malformed strings, hash names, arrows, and unknown characters.
- [x] Assign keyword-specific token kinds rather than classifying hash names
  in the parser.
- [x] Build the first LAIN-IR-owned `Module -> Function -> ReturnConst`
  representation and make the C emitter consume it.
- [x] Represent wrapping arithmetic, explicit signed/unsigned division and
  comparisons, and `zext/sext/trunc`.
- [x] Represent `i32` parameter lists, `%parameter` references, call argument
  lists, parameter scope, and call arity.
- [x] Carry `i8/i16/i32/i64` physical bit widths through signatures and C
  emission; require the native `main() -> i32` entry shape.
- [x] Parse ordered `#let` bindings, enforce declaration order and unique local
  names, and emit typed C locals before the final return.
- [x] Represent structured `#if` with then/else/continuation bodies, isolate
  branch scopes, require `i1` conditions, and verify return coverage.
- [x] Represent `#loop`, mutable assignment, `#break`, and `#continue`;
  preserve the LAIN-IR rule that loop-body fallthrough exits the loop.
- [x] Represent `addr`, constant-size `#alloca`, canonical `#lea`, typed
  `#load`, and typed `#store`.
- [x] Parse external physical procedure declarations, `#unit` returns, and
  effect-only direct calls.
- [x] Represent every procedure and instruction form in the executable text
  language, including canonical physical types, typed stores, procedure
  addresses, and signature-bearing indirect calls.
- [x] Resolve direct-call names and forward procedure references for the
  implemented expression subset; reject missing and duplicate procedures.
- [x] Parse and verify the complete current `src/lainir/compiler.l1`.
- [x] Differentially compare shared-subset acceptance with the C parser.

## M3 — verifier complete

Status: complete.

- [x] Initial structural validation for modules, functions, signatures, and
  the required native entry.
- [x] Physical bit-width validation for parameters, returns, arithmetic,
  comparisons, and direct-call arguments.
- [x] Parameter binding and direct-call validation for the implemented subset.
- [x] Extend structural and type validation to every instruction form,
  including memory operands, conversions, and indirect signatures.
- [x] Validate active named and unnamed loop targets, including outer-loop
  jumps and duplicate active labels.
- [x] Differentially compare parse-versus-verify failure phases with the C
  verifier.

## M4 — C backend complete

Status: complete.

- [x] Procedures, locals, direct calls, basic memory, and structured control
  flow for the implemented parser subset.
- [x] Complete the executable text language's address and memory forms:
  allocation, `lea`, typed load/store, and typed field access.
- [x] Keep source-language aggregates and globals outside LAIN-IR, as required
  by `docs/02-lain-ir.md`; their physical representation is ordinary memory
  and address arithmetic rather than hidden backend forms.
- [x] Explicit wrapping fixed-width integer arithmetic and checked division
  without C signed-overflow undefined behavior.
- [x] External calls go through declared libc/system ABI symbols only.
- [x] `#proc_addr` and signature-bearing `#call_indirect`, including
  direct-target signature verification.
- [x] External calls through declared libc/system ABI for the implemented
  physical signature subset.
- [x] Differentially execute arithmetic, conversions, memory-independent
  calls, and indirect calls against the C interpreter.

## M5 — self-hosting

Status: complete.

- [x] Stage 0 interprets the compiler and emits `lainir-c.stage1.c`.
- [x] The system C compiler creates a standalone `lainir-c.stage1`.
- [x] Stage 1 compiles all of the current `src/lainir` compiler source.
- [x] The native executable links no private LAIN-IR implementation library;
  its C host provides process I/O and allocation capabilities only.

## M6 — convergence

Status: complete.

- [x] Stage 1 produces stage 2.
- [x] Stage 2 produces stage 3.
- [x] Stage 2 and stage 3 C output is byte-for-byte identical.
