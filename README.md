# Lain

Lain is a native language experiment.

## Description

Lain consists of three layers:

1. **Lain IR**

    - A structured IR similar to C, but a bit lowerer;

2. **Compile-time execution**

    - Expressions can be evaluated during compilation.
    - Procedures can be executed during compilation.

3. **Meta functions**

    - Scheme functions can be injected through compiler hooks to inspect and transform AST nodes.

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
zig build                         # build and install lainc, l1c, and l1i
zig build test                    # build everything and run core tests
zig build lainc -Dscheme=gauche  # select the Scheme host explicitly
zig build lainc -Dscheme=chibi
```

Build products are installed under `zig-out/bin`. On Windows the default
Scheme backend is Gauche; other platforms default to the bundled Chibi setup.
The legacy Makefile remains available during the transition.
