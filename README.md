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
    - Scheme remains the stage-0/reference implementation during migration.

Lain aims to keep the compiler small and move most high-level language features into reusable meta libraries.

## Design Principles

- Keep the core IR small and stable.
- Represent control flow explicitly.
- Represent memory operations explicitly.
- Separate compile-time evaluation from compile-time execution.
- Move high-level language features into libraries whenever possible.
- Treat meta programming as a layer built on top of the IR rather than a special-purpose subsystem.

## Build and Run

Zig 0.16 or newer is the primary stage-0 build tool:

```text
zig build                         # install tools and the stage-2 compiler artifact
zig build test                    # build everything and run core tests
zig build self-host-compiler      # explicitly rebuild/install stage2_compiler.l1
zig build lainc -Dscheme=gauche  # select the Scheme host explicitly
zig build lainc -Dscheme=chibi
```

Build products are installed under `zig-out/bin`. On Windows the default
Scheme backend is Gauche; other platforms default to the bundled Chibi setup.
The legacy Makefile remains available during the transition.

Normal LAIN-IR emission uses the installed self-hosted artifact:

```text
lainc --emit-l1 input.lain output.l1
lainc --emit-workspace-l1 output.l1 math.lain app.lain
```

Multi-file compilation keeps one stable syntax unit per input, constructs the
module dependency graph in Lain, and lowers modules in topological order.  The
normal C launcher transports paths/bytes and calls the Lain-owned compiler ABI
once; it does not join source files or re-run compilation to obtain
diagnostics.  Module summaries can already outlive their syntax units, while
persistent per-module incremental reuse remains a later milestone.

`--emit-l1` currently accepts the self-hosting core subset: `i32`/`addr`
procedures, calls, arithmetic, explicit returns, foreign declarations, and
explicit `let name: Module = module { ... }` workspaces. The older, broader
The Scheme stage-0 frontend is maintained only on `bootstrap/stage0`.
The main worktree may invoke it through the sibling bootstrap worktree when
rebuilding the first compiler seed; normal compilation never falls back to it.
