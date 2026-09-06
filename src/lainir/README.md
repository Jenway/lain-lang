# `lainir-compiler`

This directory contains the LAIN-IR implementation of the LAIN-IR to C
compiler.  The C bootstrap interpreter is the seed: it executes this code and
supplies parsing, verification, and compile-time `#eval`; C emission remains
in the LAIN-IR compiler source.

## Frozen Lain compiler

`src/lainir/lainc.l1` is the frozen minimal Lain compiler: it is the packaged
LAIN-IR-written Lain frontend (`src/lainir/lain/*.l1` plus
`src/lainir/tools/source.l1`), generated reproducibly by
`scripts/freeze_lainc_bootstrap.py` and executed by the seed interpreter.  It
currently covers the executable Lain subset exercised by `tests/lainir_lain`,
including functions, calls, modules, and the first compile-time
closure/module cases.  It is the bootstrap seed for a Lain-written
compiler. The Lain-written compiler source now lives together under
`src/lainc`; there is no separate archive compiler source tree.

## Boundary

- Compiler logic is written in `.l1`.
- Generated C may use the C standard library and platform libraries.
- The native `lainir-compiler` links only the seed runtime needed for parsing,
  verification, and `#eval`; it does not contain a second L1 evaluator.
- Bootstrap capabilities provide source bytes, diagnostics, allocation,
  `#eval` results, and an output byte stream.

The seed-side `lainir_eval_source_values` API parses, verifies, folds, and
returns scalar `#eval` results in source order. The compiler requests this
operation through `bootstrap.eval_source`; it does not evaluate arithmetic
itself.
- Generated output is deterministic.

The compiler accepts canonical `#bits<N>`, `#float<32>`, `#float<64>`,
`#addr`, `#unit`, and `#never` physical signatures. A return expression may contain decimal
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
intentionally small instruction subset; parsing, verification, IR ownership,
and `#eval` execution remain in seed.

Structured `#if` supports optional bare `else`, branch-local bindings, and a
following continuation. Conditions must be `i1`, and every path through a
non-unit function must return.

Structured `#loop` supports mutable `%name: type = value`, `#break`, and
`#continue`. Falling through the loop body exits it; only `#continue` starts
the next iteration. Named jumps may target an active outer loop. Unknown
targets and duplicate active labels are rejected, and loop-local bindings do
not escape into the continuation.

The first memory slice supports activation-scoped `#alloca`, canonical four-field
`#lea`, typed `#load`, and `#store` of a statically typed value. Generated C lowers loads and stores
through `memcpy`, so unaligned addresses and C's strict-aliasing rules do not
introduce undefined behavior:

```lain-ir
#let %memory: #addr = #alloca(4)
#let %slot: #addr = #lea(base=%memory, idx=0, scale=1, offset=0)
#let %answer: #bits<32> = 42
#store[#bits<32>] %answer, %slot
#let %loaded: #bits<32> = #load[#bits<32>](%slot)
```

`#data` declares named, read-only static bytes. `#data_addr(name)` obtains its
physical address; typed loads and `#lea` provide access. Stores through such an
address trap in the reference interpreter. Relocations and mutable global data
are not part of the current slice.

Each `#alloca` belongs to the current procedure activation. Its storage is
released when that activation returns, and its address must not escape it.

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
zig-out/bin/lainir-seed.exe
  -> zig-out/bin/lainir-compiler.exe
  -> generated C
```

The generated compiler can compile its own LAIN-IR source and repeated output
is required to be byte-for-byte identical. The
small native host in `seed/src/host/native_compiler.c` supplies only
source bytes, diagnostics, allocation, artifact I/O, and the process entry
point. It contains no lexer, parser, verifier, IR, or emitter logic.

The reference interpreter exposes `lainir_eval_block(...)`,
`lainir_fold_module(...)`, and `lainir_eval_source(...)` for compiler
integrations. The self-hosted compiler delegates `#eval` execution to the
seed interpreter through `bootstrap.eval_source`; it does not contain a
second expression evaluator. Ordinary
`lainir_run(...)` remains the runtime entry point. Interpreter limits can be
configured with `lainir_caps_set_limits`.

## Seed invocation

From the repository root, after building `seed`:

```text
zig-out/bin/lainir-seed.exe \
  seed/lainir/compiler.l1 lainir_compile_module output.c input.l1
```

Then compile `output.c` with an ordinary C compiler.  No LAIN-IR support
library is involved.

## Syntax authority

Canonical public syntax is defined by `docs/01-lain-ir.md`. Both the C
reference front end and the LAIN-IR-written compiler accept canonical
`#bits<N>`, `#float<N>`, `#addr`, typed `#store`, explicit signed/unsigned
integer operations, width conversions, and signature-bearing indirect calls.
LAIN-IR-written compiler delegates compile-time `#eval` blocks to the seed
interpreter; unsupported runtime-dependent evals are rejected instead of being
emitted as runtime code.
Verifier diagnostics preserve the source line of parsed instructions (direct
C-API nodes may have line `0`).
