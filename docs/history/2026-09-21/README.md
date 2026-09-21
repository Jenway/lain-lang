> 历史记录。原路径：`README.md`。归档日期：2026-09-21。
> 本文保留整理前的内容；其中的状态、命令、语法和结论不作为现行依据。
> 当前文档从 [文档索引](../../README.md) 阅读。

# Lain

Current compiler version: `0.1.0-alpha.1`.

Lain is a native language experiment.

## Description

Lain consists of three layers:

1. **Lain IR**

    - A structured IR similar to C, but a bit lowerer;

2. **Compile-time execution**

    - Expressions can be evaluated during compilation.
    - Procedures can be executed during compilation.

3. **Meta functions**

    - The self-hosted frontend is written in Lain and transforms RawAst into
      structured LAIN-IR.
    - Language features are implemented by Lain code rather than a C or
      Scheme frontend.

Lain aims to keep the compiler small and move most high-level language features into reusable meta libraries.

## Design Principles

- Keep the core IR small and stable.
- Represent control flow explicitly.
- Represent memory operations explicitly.
- Separate compile-time evaluation from compile-time execution.
- Move high-level language features into libraries whenever possible.
- Treat meta programming as a layer built on top of the IR rather than a special-purpose subsystem.

## Project layout

```text
seed/        minimal C execution base and the LAINIR-written LAINIR compiler
bootstrap/   hand-maintained LAINIR sources for the startup Lain compiler
src/         formal Lain-written implementation
std/         Lain-written standard library and standard Meta definitions
scripts/     build, verification and fixed-point drivers
docs/        language design, current-implementation and roadmap documents
build/       generated bundles, snapshots, native programs, and test output
```

Generated compiler artifacts are never checked into `src/` or `bootstrap/`.
`python scripts/build_lain_compiler.py` writes the startup compiler bundle to
`build/bootstrap/lainc.l1`.

## Bootstrap status

The old `EvalResult` path has been removed. Meta compile-time evaluation now
lowers to temporary LAINIR `#eval` and executes through LAINVM. Rebuilding is
available again: the hand-written bootstrap source can regenerate
`build/bootstrap/lainc.l1` and compile the ordinary physical subset, the formal
standard-library source closure passes its ABI and conformance checks, and the
`src/lainc` source closure builds as a verified 362-procedure LAINIR artifact
that is deterministic across independent builds.

The gen2/gen3 fixed-point gate is still open. `srclainc.l1` is currently a
library artifact with no `compiler_compile` entry point the seed can call, so
two identical builds do not yet prove a compiler fixed point, and the native
compiler matrix follows that result. See
`docs/implementation/lain-written-backend.md` and
`docs/roadmaps/lain-roadmap.md` section 3.2 for the current evidence and the
remaining gate.

`--emit-l1` currently accepts the self-hosting core subset: `#bits<32>`/`#addr`
callables, calls, arithmetic, explicit returns, foreign bindings, and canonical
declarations of the form:

```lain
let NAME [: EXPECTED] = INITIALIZER
```

The declaration constructors are `std::func`, `std::struct`, `std::module`,
and `import("path")`. `@` is reserved for attributes such as `@export` and
`@foreign`; it does not introduce a second declaration grammar. Historical
standalone `fn`, `struct`, `module`, and `import` declarations are rejected.

Build the generated bootstrap bundle and its manifest with:

```text
python scripts/freeze_lainc_bootstrap.py
```

The command writes both files under `build/bootstrap/`. Verify them with:

```text
python scripts/check_lainc_bootstrap_snapshot.py build/bootstrap/lainc.l1
```
