# lain.compiler

This package is the first Lain-written compiler core.

It is intentionally separate from `std/meta`, which remains the old Scheme
hosted compiler pipeline. The old compiler may build this package and import
its interfaces, but compiler concepts added here should not become C or Scheme
special cases.

Current scope:

- AST topology helpers
- AST S-expression tree consumers
- type shape helpers
- diagnostic data helpers
- meta-owned module summary, query, dependency, and diagnostic helpers
- LAIN-AST topology readers for module export extraction
- LAIN-AST to ModuleSummary-like export count and score helpers
- minimal IR builder model helpers
- a first `lainc.lain` entry that composes module summary, IR builder, and diagnostics

The package path is imported as `packages::lain::compiler::*`.
