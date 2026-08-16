# `lainir-c`

This directory contains the LAIN-IR implementation of the LAIN-IR to C
compiler.  The C bootstrap interpreter is stage 0: it executes this code, but
it does not provide parsing, verification, or C emission on the compiler's
behalf.

## Boundary

- Compiler logic is written in `.l1`.
- Generated C may use the C standard library and platform libraries.
- The native `lainir-c` must not link a private C implementation of LAIN-IR.
- Bootstrap capabilities provide source bytes, diagnostics, allocation, and an
  output byte stream only.
- Generated output is deterministic.

The compiler accepts canonical `#bits<N>`, `#float<32>`, `#float<64>`,
`#addr`, `#unit`, and `#never` physical signatures. During bootstrap
migration it also accepts the older `i32`/`addr` aliases used by
`compiler.l1`. A return expression may contain decimal
constants, parameter references, nested integer arithmetic and comparisons,
and direct calls:

```lain-ir
#proc add(#bits<32> %left, #bits<32> %right) -> #bits<32> {
  #return #add(%left, %right)
}

#proc main() -> #bits<32> {
  #let %answer: #bits<32> = #call add(40, 2)
  #return %answer
}
```

The compiler resolves calls across the complete module, accepts forward
references, rejects duplicate or missing procedures and parameters, checks
ordered local bindings and call arity, and emits C prototypes before
definitions. This remains an
intentionally small instruction subset, but lexer, parser, IR, name
verification, and emission are now separate LAIN-IR procedures.

Structured `#if` supports optional bare `else`, branch-local bindings, and a
following continuation. Conditions must be `i1`, and every path through a
non-unit function must return.

Structured `#loop` supports mutable `%name: type = value`, `#break`, and
`#continue`. Falling through the loop body exits it; only `#continue` starts
the next iteration. Named jumps may target an active outer loop. Unknown
targets and duplicate active labels are rejected, and loop-local bindings do
not escape into the continuation.

The first memory slice supports constant-size `#alloca`, canonical four-field
`#lea`, typed `#load`, and `#store` of a statically typed value. Generated C lowers loads and stores
through `memcpy`, so unaligned addresses and C's strict-aliasing rules do not
introduce undefined behavior:

```lain-ir
#let %memory: addr = #alloca(4)
#let %slot: addr = #lea(base=%memory, idx=0, scale=1, offset=0)
#let %answer: #bits<32> = 42
#store %answer, %slot
#let %loaded: #bits<32> = #load[#bits<32>](%slot)
```

There is deliberately no hidden `#global` or `#data` instruction in this
layer. Frozen compiler data is represented as ordinary immutable bytes and
addresses supplied by the surrounding module/host boundary; named data
segments would move source-language module semantics into the IR.

At this stage a bare integer literal has no width of its own, so it cannot be
stored directly; bind it to a typed local first.

Physical procedure references use the signature-bearing form from the
language design. The signature is checked at the call site, and when the
target is directly a `#proc_addr`, it is also checked against the referenced
procedure:

```lain-ir
#let %target: #addr = #proc_addr(add)
#let %sum: #bits<32> =
  #call_indirect[(#bits<32>, #bits<32>) -> #bits<32>](%target, 40, 2)
```

The generated C uses an explicit fixed-width function-pointer cast. No native
LAIN-IR runtime or private helper library performs the call.

External procedures are declarations, not hidden implementations. They may
name libc or platform ABI functions and can return either a physical value or
`#unit`. Calls returning `#unit` are emitted as effect-only statements:

```lain-ir
#extern #proc srand(#bits<32> %seed) -> #unit;

#proc main() -> #bits<32> {
  #call srand(1)
  #return 42
}
```

## Self-hosting

The compiler now reaches a native fixed point:

```text
C bootstrap interpreter
  -> lainir-c.stage1.c
  -> native lainir-c.stage1
  -> lainir-c.stage2.c
  -> native lainir-c.stage2
  -> lainir-c.stage3.c
```

Stage 2 and stage 3 output are required to be byte-for-byte identical. The
small native host in `bootstrap/src/host/native_compiler.c` supplies only
source bytes, diagnostics, allocation, artifact I/O, and the process entry
point. It contains no lexer, parser, verifier, IR, or emitter logic.

The reference interpreter exposes `lainir_eval_block(...)` and
`lainir_fold_module(...)` for compiler integrations. `lainir_fold_module`
recursively executes `#eval` blocks and materializes scalar bit results as
constants; nested evals are folded inside-out. The self-hosted compiler has a
matching LAIN-IR evaluator for its constant integer subset, and rejects
runtime-dependent evals instead of emitting them as runtime code. Constant
locals and constant-argument local calls inside an eval block are substituted
before evaluation, so nested blocks and constant `#if` branches can build
values through ordinary `#let` bindings. Ordinary
`lainir_run(...)` remains the runtime entry point. Interpreter limits can be
configured with `lainir_caps_set_limits`.

## Stage-0 invocation

From the repository root, after building `bootstrap`:

```text
bootstrap/zig-out/bin/lainir-interpreter \
  src/lainir/compiler.l1 lainir_compile output.c input.l1
```

Then compile `output.c` with an ordinary C compiler.  No LAIN-IR support
library is involved.

## Syntax authority

Canonical public syntax is defined by `docs/02-lain-ir.md`. Both the C
reference front end and the LAIN-IR-written compiler accept canonical
`#bits<N>`, `#float<N>`, `#addr`, typed `#store`, explicit signed/unsigned
integer operations, width conversions, and signature-bearing indirect calls.
The reference parser keeps old `i32`/`addr` spellings only for migration;
`lainir-print --strict` rejects those aliases.
Legacy scalar aliases and ambiguous integer spellings remain accepted only as
a migration surface for the existing bootstrap compiler source. The
LAIN-IR-written compiler now also performs a small compile-time evaluator for
constant integer eval blocks and zero-argument local calls; unsupported
runtime-dependent evals are rejected instead of being emitted as runtime code.
Verifier diagnostics preserve the source line of parsed instructions (direct
C-API nodes may have line `0`).
