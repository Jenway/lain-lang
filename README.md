# Lain

Lain is a native language experiment with a small compiler core, a library-owned
meta layer, and a staged compile-time execution model.

## Current Direction

Lain is organized around three distinct phases:

1. `Scheme meta phase`
   - Rewrites syntax.
   - Defines language-level objects such as `type`, `module`, `effect`,
     `signature`, and `interface` in libraries.
   - Performs static normalization and lowering preparation.
   - Should stay mostly pure.

2. `compile-time lain phase`
   - Runs `lain` code during compilation.
   - Handles effectful compile-time work such as code generation, bridge
     generation, schema loading, and other artifact-producing tasks.
   - Uses explicit effects and injected capabilities rather than compiler magic.

3. `runtime phase`
   - The final program.

## Compiler Boundary

The compiler should not provide semantic objects like `module` or `effect`.
Those belong to meta libraries. The compiler only provides minimal substrate:

- syntax construction and source spans
- diagnostics and symbol generation
- phase orchestration
- IR construction and code emission
- interface artifact I/O such as `.lci`
- capability injection for compile-time `lain`

## Module System Direction

The current module design follows these rules:

- `module` is a meta-layer object, not a default runtime value
- `signature` is the interface type for modules
- `interface` remains the dynamic-dispatch protocol
- `let` is the unified top-level binding syntax
- `import(path)` returns a module object
- `export` forms the module interface and is not a runtime effect

See [docs/module-rfc-01.md](docs/module-rfc-01.md) for the concrete module
direction.
