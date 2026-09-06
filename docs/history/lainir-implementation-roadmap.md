# LAIN-IR compiler roadmap

> **Historical implementation record.** This file preserves completed frontend and compiler-boundary milestones. Current work is tracked in [`../roadmaps/compiler-bootstrap.md`](../roadmaps/compiler-bootstrap.md) and [`../roadmaps/lainir-maintenance.md`](../roadmaps/lainir-maintenance.md).
> The former `src/compiler-archive` tree was later merged into `src/lainc`.

The implementation grows as executable vertical slices.  A phase is complete
only when seed can execute it and a native C compiler can compile its
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

The initial compiler parser still consumes one compiler artifact, so these
layers are procedures in `compiler.l1`.  Physical source-file splitting
belongs to M2,
after compiler-source bundling exists.

## M2 — parser complete

Status: complete.

- [x] Own a physical cursor/token representation in LAIN-IR memory.
- [x] Tokenize identifiers, numbers, strings, hash names, punctuation,
  comments, and EOF accepted by `seed/src/text/parser.c`.
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
  by `docs/01-lain-ir.md`; their physical representation is ordinary memory
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

- [x] seed interprets the compiler and emits `lainir-c-gen1.c`.
- [x] The system C compiler creates a standalone `lainir-c-gen1`.
- [x] gen1 compiles all of the current `src/lainir` compiler source.
- [x] The native executable links no private LAIN-IR implementation library;
  its C host provides process I/O and allocation capabilities only.

## M6 — convergence

Status: complete for the current LAIN-IR compiler closure.  The bootstrap
interpreter executes the compiler source to produce gen1 C; gen1 and
gen2 then produce gen2 and gen3 C respectively.

- [x] gen1 produces gen2.
- [x] gen2 produces a verifier-valid gen3.
- [x] gen2 and gen3 C output is byte-for-byte identical.

## M7 — first Lain frontend slices

The Lain frontend is separate from the LAIN-IR parser. Its RawAst is a
generic topology tree; semantic forms are interpreted by LAIN-IR meta code.

- [x] Parse Lain atoms and delimiter groups into a LAIN-IR-owned RawAst.
- [x] Interpret `let NAME: type = std::struct { ... }` in meta code.
- [x] Build a meta-owned record descriptor with field spans, offsets, size,
  alignment, and diagnostic codes.
- [x] Reject duplicate field names in the meta-owned record descriptor.
- [x] Interpret `std::module { ... }` into a meta-owned module descriptor with
  member spans and duplicate-member diagnostics.
- [x] Execute a first compile-time expression slice over generic syntax.
- [x] Evaluate left-associative `+`, `-`, `*`, and `/` expressions with a
  division-by-zero diagnostic.
- [x] Lower a numeric record to executable LAIN-IR and run it.
- [x] Build a deterministic multi-source workspace boundary with import
  resolution and cycle/unresolved-module diagnostics.
- [x] Cache parsed units and graph edges so repeated meta phases do not
  re-lex/re-parse each source; unresolved imports are stored as a source-count
  sentinel and cycle checks walk the cached edge vectors.
- [x] Add a unified LAIN-IR driver that schedules workspace, meta, and
  compile-time evaluation phases into one deterministic artifact.
- [x] Propagate the first workspace/meta/eval diagnostic to the driver exit
  status instead of silently accepting an error artifact.
- [x] Make the self-hosting source closure resolve the physical compiler from
  `src/compiler-archive` while preserving package-qualified logical imports.
- [x] Provide a reproducible bundle/run entry for the LAIN-IR frontend.
- [x] Replace the inspection artifact with the `compiler_compile`
  request/result boundary and its first executable source-to-LAIN-IR slice;
  the broader language closure remains tracked under M8/M9.
- [x] Generalize the meta type registry and record layout to `bool`, signed
  `i8`/`i16`/`i32`/`i64`, and unsigned `u8`/`u16`/`u32`/`u64`, including
  alignment padding.
- [x] Generalize physical field lowering from two fields to an `i32` field
  sequence (including generated accumulator chains).
- [x] Lower heterogeneous physical fields (`bool`/`i8`/`i16`/`i32`/`i64`)
  through sign-extension into a `bits<64>` accumulator and typed stores.
- [x] Preserve source-level signedness for the built-in `i*`/`u*` field slice;
  narrow unsigned loads are zero-extended and signed loads sign-extended.
- [ ] Define and enforce source-level overflow policy instead of relying on
  the current positive test values.

## M8 — replace the phase artifact with the compiler boundary

This is the next implementation block.  The existing `src/compiler-archive/*.lain`
remains the source of truth; the small frontend above is not a substitute for
it.

- [x] Add a stable `compiler_compile` entry and lower the first source form,
  `let main = std::func() -> i32 { return N; };`, to executable LAIN-IR.
- [x] Split executable-entry policy from library compilation: `compiler_compile`
  requires `main`, while `compiler_compile_library` keeps the same request /
  result ABI for source closures without a native entry.
- [x] Define a LAIN-IR-owned `CompilerRequest`/`CompilerResult` record with
  capability-backed source count, lowering options, diagnostic status, module
  count, and artifact presence.  The request now carries an explicit source
  slice and root-source selection.
- [x] Make the bootstrap host provide only source/path/artifact capabilities;
  the compiler bundle calls one `compiler_compile` entry and projects the
  returned status.  The host remains unchanged and has no Lain semantic code.
- [x] Add a syntax-unit index over cached RawAst nodes, preserving source
  spans and module boundaries without teaching RawAst Lain keywords.
- [x] Feed the cached syntax-unit index into compiler lowering instead of
  re-lexing and re-parsing each source during the function pass.
- [x] Lower multiple `std::func` declarations, `i32` parameters, literal
  arithmetic, and direct calls with arity/name diagnostics (`5108`/`5109`).
- [x] Preserve physical widths for `i8`/`i16`/`i32`/`i64` and unsigned
  variants in function signatures; require the native `main` shape.
- [x] Route function type-width lookup through the Meta built-in type registry;
  the program lowerer no longer owns the integer-name table.
- [x] Lower a two-branch `if` with boolean literals or `==`/`!=` comparison
  against a parameter/literal, preserving return coverage.
- [x] Lower ordered local `let` bindings in function bodies and resolve local
  names in return expressions.
- [x] Lower a mutable `while` loop with comparison exit, assignment,
  `break`, and `continue` in the first function-body control-flow slice.
- [x] Run record/module Meta validation before function collection and allow
  validated type/module declarations to coexist with executable functions.
- [ ] Implement Meta expansion for declarations, attributes, imports, and
  compile-time calls in LAIN-IR, with explicit limits and diagnostics.
- [x] Collect executable functions nested in `std::module` bindings while
  keeping `@export` as ordinary RawAst metadata rather than parser magic.
- [x] Validate the first Meta attribute contract (`@export` must precede a
  canonical `let` binding) and reject unknown/dangling attributes.
- [x] Run Meta-owned record validation for record bindings nested in modules;
  duplicate fields and unknown physical field types stop compilation.
- [x] Validate runtime record literals against their Meta descriptor: unknown,
  duplicate, and missing fields are rejected before LAIN-IR emission.
- [x] Validate nested module binding shape and duplicate member names before
  collecting executable members.
- [x] Validate top-level `std::consteval(...)` bindings through the existing
  LAIN-IR evaluator, including division-by-zero diagnostics.
- [x] Materialize successful top-level integer `std::consteval(...)` bindings
  as constants in function operands; unresolved and duplicate names remain
  diagnostics.
- [x] Execute the first compile-time direct function-call subset in LAIN-IR,
  including parameter substitution, arithmetic returns, arity, and name
  diagnostics.
- [x] Lower `std::consteval(...)` operands inside runtime function expressions
  by executing the same LAIN-IR compile-time call path and emitting the value.
- [x] Keep a Meta-owned physical type environment for aliases, records, and
  generic constructor values; named record types lower to `addr` and primitive
  aliases retain their registered bit width.
- [x] Resolve qualified function and constant operands through their final
  Meta member while retaining the original qualified atom span for diagnostics.
- [x] Consume source effect clauses (`! { ... }`) at the compiler boundary;
  effects do not become LAIN-IR syntax or host-side semantics.
- [x] Lower boolean literals, ordered comparisons, and arithmetic expressions
  nested in direct-call arguments, inserting explicit physical truncation at
  the callee parameter width.
- [x] Register simple inferred integer `let NAME = literal` bindings in the
  same Meta-owned constant environment, including module members.
- [x] Scope duplicate final-member function names by source ordinal and emit
  deterministic collision labels for later source units; nested module path
  identity remains part of M9.
- [x] Use cached import edges as a compiler boundary: unresolved imports and
  dependency cycles stop lowering with the workspace diagnostics (`4101`/
  `4103`).
- [x] Treat `std::...` imports as external capability modules and resolve
  package-qualified imports against physical source basenames; unresolved
  internal imports and cycles still produce diagnostics.
- [ ] Elaborate bindings and types, then lower the first executable Lain subset
  (function declarations, integer expressions, calls, and returns) to the
  existing LAIN-IR builder.
- [ ] Verify and print the generated LAIN-IR through the existing verifier and
  printer; no C-side semantic fallback is allowed.
- [x] Add one end-to-end fixture that compiles with `compiler_compile`, runs
  the generated LAIN-IR, and proves seed/gen1 byte convergence.

## M9 — self-host the full compiler source

The remaining work is deliberately ordered by semantic dependency.  We must
not hide unresolved module members behind generated externs: `Compiler.compile`
is a compile-time module value that has to become a real executable Lain
function before the canonical closure can be checked.

The first bootstrap increment now exists: `src/lainir/lainc.l1` is a
reproducible executable compiler artifact for the current Lain frontend
subset.  It can compile the small function/module fixtures and lower the
canonical `src/compiler-archive` source closure to verifier-valid LAIN-IR.  That
artifact is still below the full compiler boundary described by this section.

Status (2026-08-16): full compiler-API Meta instantiation is now accepted
end-to-end.  Running the re-frozen `src/lainir/lainc.l1` over
`compiler_api_schema.lain` plus all `src/compiler-archive` and std sources succeeds at
`compiler_compile`, passes `lainir-print` (including the previously blocked
`Meta.expand`/`Modules.declare` nominal parameter physical-type check), and
`lainir-seed run` returns `1` (`tests/lainir_lain/run_compiler_api_bootstrap.py`).
Remaining blocker: the empty-input fixture
`compiler_api_compile_empty.lain` produces a verifier-valid artifact whose
execution dereferences a null address in `lainir-seed run` (access violation
`0xC0000005`).  After that comes real Lain source input, wiring
`run_compiler_source_closure.py`/`run_compiler_api_bootstrap.py` into
`run_all.py`, and the gen2/gen3 byte comparison listed below.

- [ ] **Meta values and environments.** Represent module values, function
  values, and captured bindings in the LAIN-IR meta heap; make a compile-time
  function call return a module/member descriptor instead of only an integer.
- [ ] **`#eval` bridge.** Compile a compile-time block to LAIN-IR, invoke the
  existing interpreter during compilation, and materialize its result or AST
  data. Add recursion, step, allocation, and diagnostic limits; runtime
  interpretation treats the same form as an ordinary call boundary.
- [ ] **Qualified member lowering.** Resolve `Module.member` through the
  meta-owned descriptor/environment chain, including functions created inside
  a module factory and their captured bindings. Keep unresolved members as
  diagnostics, never as implicit host calls.
- [x] Materialize Meta-resolved call targets in the physical function list so
  generated direct calls cannot reference an omitted procedure.
- [x] Keep module-valued forwarding factories in the compile-time phase while
  preserving physical module-typed identity functions.
- [ ] **Canonical compiler closure.** Lower `src/compiler-archive` module by module,
  starting with `compiler_core`, then `compiler_driver`, `compiler_api`, and
  `lainc`; after each module, run `lainir-print` and execute a small request.
- [ ] **Thin CLI and fixed point.** Restore `lainc` as a capability-only host,
  rebuild gen2 and gen3 from the same canonical source, and require
  byte-identical artifacts.
- [ ] **Regression gate.** Re-enable the core suites that currently require
  `zig-out/bin/lainc`, then run the Lain frontend/tooling suites and the full
  self-hosting gate together.
