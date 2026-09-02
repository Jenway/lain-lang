# Lain frontend slices written in LAIN-IR

This directory is the beginning of the Lain compiler frontend.  It is
separate from the LAIN-IR parser in `src/lainir/tools/`:

```text
Lain source -> RawAst -> meta/domain interpretation -> typed model -> LAIN-IR
LAIN-IR source -> LAIN-IR parser/verifier/interpreter
```

`raw_ast.l1` only builds atoms and delimiter groups.  It does not know what
`let`, `type`, or `std::struct` mean.  `meta.l1` is the first domain slice: it
interprets the generic tree as a record binding, resolves the built-in signed
and unsigned integer names, and computes field offsets and record size.  The
registry covers `bool`, `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, and `u64`,
including alignment padding.  The textual dump in the tests
is only the inspection boundary; the descriptor itself is an allocated meta
object with field spans, sizes, offsets, and a diagnostic slot.  Duplicate
field names are rejected in this meta phase.

`module_meta.l1` uses the same pattern for `let NAME = std::module { ... }`:
it builds a member-span linked list and rejects duplicate member bindings.

`workspace.l1` applies the same boundary to multiple source units.  The host
only supplies bytes and paths; LAIN-IR code treats `std::...` imports as
external capabilities, resolves package-qualified imports against physical
source basenames, orders modules by path, and reports unresolved internal
imports (`4101`) or dependency cycles (`4103`) in the deterministic artifact
summary.  `workspace_cache.l1` then
retains each parsed RawAst and its resolved edge vector, so semantic passes and
cycle checks do not re-lex or re-parse the same source.

`driver.l1` is the current phase boundary: it runs the sorted workspace pass,
then invokes record meta and `std::consteval` evaluation for each unit.  Its
artifact is an inspection result today, but it already returns the first
workspace/meta/eval diagnostic as the process status.  The separate
`compiler_compile` entry now owns the executable lowering path.

`compiler_api.l1` is that compiler boundary.  It owns a LAIN-IR
`CompilerRequest`/`CompilerResult` layout; `compiler.l1` exposes the single
`compiler_compile` entry and the matching `compiler_compile_library` entry for
library/source-closure builds.  The request records capability-backed source count,
options, an explicit source slice, and a root-source selection; the result
contains status, diagnostic, syntax-unit count, and artifact presence.
`syntax_units.l1` indexes the cached RawAst workspace without teaching RawAst
any Lain keywords.  The compiler slice now collects multiple `std::func`
declarations across that source slice, preserves their physical widths, and
lowers direct calls and one binary return expression.  Top-level
`std::consteval(...)` bindings are validated by the same LAIN-IR evaluator;
their integer values are materialized for function operands.  Cached import
edges are checked before lowering, so unresolved imports and cycles fail with
the workspace diagnostics instead of being silently ignored.
The executable slice now keeps a Meta-owned type environment: primitive aliases
retain their registered width, record and generic-constructor aliases lower to
`addr`, and `&`/`&mut` references use the same physical address form.  Qualified
calls/constants (`math.add(...)`, `math.answer`) resolve their final member in
this slice, while source effect clauses (`! { ... }`) are consumed before
LAIN-IR emission.  Same-source duplicate functions are rejected; identical
final members in later source units receive deterministic physical labels.
Nested module-path identity and export-aware lookup still belong to the
canonical compiler closure.

The accepted lowering remains an executable function subset with a native
zero-parameter `main` entry; the host no longer needs a second semantic entry
point.

The compiler API now dispatches `lain_std_expand`, `lain_std_elaborate`, and
`lain_std_lower` through the bootstrap standard-library artifact.  Module and
struct, record, and type status recognition is supplied by bootstrap-stdlib
procedures; import/eval/call lowering is still a core implementation scheduled
for migration.

`eval_result.l1` is the shared result ABI for compile-time evaluation.  It keeps
the scalar/object kind and resource owner in one layout; the evaluator and Meta
passes use the owner transfer helpers instead of inventing a second result record.

The evaluator currently handles left-associative integer `+`, `-`, `*`, and
`/`, returning a phase diagnostic for unsupported syntax or division by zero.

Build and run the current frontend boundary with:

```text
python scripts/build_lain_frontend.py
python scripts/run_lain_frontend.py -o build/lainir/result.txt path/to/source.lain
```

Physical lowering now handles an arbitrary short sequence of registered
numeric fields.  Narrow signed fields are sign-extended and unsigned fields
zero-extended into a `bits<64>` accumulator; stores use their physical width.
Overflow policy still belongs to the eventual source-level type model, and the
probe uses positive values.
