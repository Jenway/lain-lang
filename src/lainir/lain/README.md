# Bootstrap Lain compiler modules written in LAINIR

This directory contains the remaining hand-written LAINIR modules used to
bootstrap the Lain compiler. It is a temporary implementation layer, not the
long-term compiler architecture.

The active boundaries are:

```text
Lain source -> RawAst -> Meta rules -> LAINIR
LAINIR source -> parser -> verifier -> LAIN-VM execution
```

`raw_ast.l1` and `ast_runtime.l1` preserve source topology without assigning
Lain language meaning. The `meta_*.l1`, workspace, syntax-unit and lowering
modules implement the bootstrap subset of language policy. `compiler_api.l1`
and `compiler.l1` expose the physical `compiler_compile` boundary.

The former standalone frontend driver, Meta evaluator, evaluation cache and
program-lowering path were coupled to an invalid evaluation result protocol and
have been removed. This source set is intentionally incomplete until Meta
compile-time execution is reconnected through LAIN-VM. The frozen compiler
artifact remains the temporary bootstrap tool during that migration.

The implementation plan and its acceptance gates are recorded in
`docs/roadmaps/lain-roadmap.md`.
