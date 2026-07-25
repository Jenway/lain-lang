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

## Bootstrap status

`main` no longer contains the C compiler host, C RawAst implementation,
C LAIN-IR storage/interpreter, Scheme bridge, or their Zig/Make build entry.
This is an intentional cut: the remaining compiler implementation must be
completed in Lain instead of silently delegating its data structures back to C.

The last buildable C/Scheme bootstrap is preserved in Git history and on the
`bootstrap/stage0` branch.  The next bootstrap target is:

```text
bootstrap branch: LAIN-IR interpreter + frozen compiler.l1
main branch:      Lain compiler and libraries written in .lain
```

The current `main` cutover commit is not buildable yet.  Its immediate work is
to replace the remaining `ast.*`, `compiler.storage.*`, `l1.*`, and
`meta.syntax.*` external contracts with Lain-owned implementations.

`--emit-l1` currently accepts the self-hosting core subset: `i32`/`addr`
callables, calls, arithmetic, explicit returns, foreign bindings, and canonical
declarations of the form:

```lain
let NAME [: EXPECTED] = INITIALIZER
```

The declaration constructors are `std::func`, `std::struct`, `std::module`,
and `import("path")`. `@` is reserved for attributes such as `@export` and
`@foreign`; it does not introduce a second declaration grammar. Historical
standalone `fn`, `struct`, `module`, and `import` declarations are rejected.

The final gate remains `stage2.l1 == stage3.l1`, but it must be restored using
the bootstrap branch interpreter without reintroducing C facilities into
`main`.
