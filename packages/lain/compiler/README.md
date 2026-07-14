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
- `mini_meta.lain`: the M4 Lain-owned AST-to-L1 driver and result façade
- `mini_syntax.lain`: zero-copy semantic/Middle-AST views over RawAst handles
- `mini_type.lain`: explicit error / `i32` / `bool` / `addr` type objects
- `mini_module.lain`: Meta-owned procedure/dependency summary and lookup policy
- `mini_elaborate.lain`: module function table, parameter/local scope,
  call/arity resolution, and expression type validation
- `mini_lower.lain`: multi-procedure lowering with parameters, calls,
  comparison, and structured `if/else`
- `mini_compile_result.lain`: single-parse CompileResult/L1Unit query protocol
- `mini_diagnostic.lain`: stable diagnostic codes and user-facing messages
- `l1_text_builder.lain`: the canonical structured-L1 text construction API
  used by Lain-owned frontends

M3 builds these modules once, in dependency order, into
`build/core-self-hosting/meta_compiler.l1`.  The reusable artifact declares
only its required host capabilities and can compile target source without
source-linking the compiler graph through Scheme on every invocation.
The artifact loader enforces a host-side allowlist in addition to checking the
artifact's exact extern set at build time; declaring an arbitrary Scheme
binding as an extern does not grant access to it.

M4 adds a Lain-owned top-level Middle Form layer (`mini_middle.lain`) and a
lexical scope query that follows nested block ownership rather than treating a
function body as one flat sibling interval.  The reusable artifact can now
validate the complete mini frontend source closure—syntax, Middle Forms,
types, module table, elaborator, L1 text builder, lowerer, diagnostics,
CompileResult, and Meta facade—and selectively lower a named procedure while
representing the other validated callables as extern contracts.  The M4 test
executes the resulting self-compiled `mini_meta_schema_version` procedure.

Whole-unit text emission is still a bootstrap path, not the long-term L1 ABI.
Its dedicated `core.string-append-linear!` capability consumes temporary text
fragments so compiler-sized emission remains bounded.  M6 replaces this text
path with the structured Lain-owned `L1Unit` builder.

The package path is imported as `packages::lain::compiler::*`.
