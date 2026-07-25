# lain.compiler

This package is the first Lain-written compiler core.

It is intentionally separate from the old Scheme-hosted pipeline.  That
pipeline now lives on `bootstrap/stage0`; `main` contains no `std/meta`
implementation.  Bootstrap may build this package, but compiler concepts added
here must not become C or Scheme special cases.

Current scope:

- the 19-module self-hosted compiler closure defined by `compiler_source.py`
- the structured L1 model used by active Bootstrap Core import tests
- `compiler.lain`: the M5 Lain-owned AST-to-L1 driver and result façade
- `syntax.lain`: zero-copy semantic/Middle-AST views over RawAst handles
- `types.lain`: explicit error / `i32` / `bool` / `addr` type objects
- `modules.lain`: Meta-owned ModuleValue/ModuleSummary views, export and
  dependency policy
- `elaborator.lain`: module function table, parameter/local scope,
  call/arity resolution, and expression type validation
- `workspace.lain`: multi-module validation, diagnostics, summary queries,
  and physical link orchestration
- `diagnostics.lain`: stable diagnostic codes and user-facing messages
- `l1_unit_builder.lain`: opaque structured L1Unit construction API
- `l1_interpreter.lain`: M7 Lain-owned interpreter for the structured L1
  bootstrap subset
- `lower.lain`: one `LowerContext`-driven lowering engine for both plain
  source roots and indexed workspace modules; only name/type resolution is
  mode-specific before physical L1 construction
- `compiler_state.lain`: retained M9 compatibility probes; it is no longer in
  the active self-hosted compiler closure

M3 builds these modules once, in dependency order, into
`build/core-self-hosting/meta_compiler.l1`.  The reusable artifact declares
only its required host capabilities and can compile target source without
source-linking the compiler graph through Scheme on every invocation.
The artifact loader enforces a host-side allowlist in addition to checking the
artifact's exact extern set at build time; declaring an arbitrary Scheme
binding as an extern does not grant access to it.

M4 adds a Lain-owned top-level Middle Form layer (`surface_forms.lain`) and a
lexical scope query that follows nested block ownership rather than treating a
function body as one flat sibling interval.  The reusable artifact can now
compile the complete mini frontend source closure—syntax, Middle Forms,
types, module table, elaborator, L1 text builder, lowerer, diagnostics,
CompileResult, and Meta facade—and selectively validate/lower a named
procedure while representing the other callable signatures as extern
contracts. The full compile entry still validates every body. The M4 test
executes the resulting self-compiled `compiler_frontend_schema_version` procedure.

M5 makes the declaration model explicit and uniform:

```lain
let name: ExpectedShape = initializer;
```

Functions, modules, and dependencies are not parser declaration kinds.
`fn`, `module`, and `require` are Meta constructors; a binding's expected
shape and initializer determine its MetaValue. A workspace can therefore bind
two modules, resolve an exported member call, and lower it to physical L1 names
such as `math__add` and `app__main`. The M5 tests execute the latter to 42 and
verify that dependency, visibility, duplicate-name, and cycle failures emit no
partial L1.

M6 removes whole-unit text generation from the default self-hosted artifact
path. `compiler_frontend_compile`, `compiler_frontend_compile_named`, and workspace compilation
return opaque structured `L1Unit` handles. Lain owns naming, type, call, and
procedure-order policy; C owns only physical L1 node storage and handle
lifecycle. Text emission is a debug view of an already-built unit, and the
default artifact no longer requires `core.string-append-linear!`.

M7 adds a read-only physical L1 ABI and implements evaluation policy in
`l1_interpreter.lain`. The interpreter owns procedure lookup, argument
binding, local bindings, arithmetic/comparison, calls, structured branches,
returns, and extern rejection. C supplies opaque node and frame/result storage
only. The C interpreter remains the reference oracle; both interpreters execute
the same linked workspace unit to 42.

M8 uses that same Lain interpreter for structured comptime evaluation.
`compiler_frontend_comptime` compiles a temporary unit, evaluates it without host
capability dispatch, and returns a structured status/value result.
`compiler_frontend_comptime_materialize_main` demonstrates the next Meta step by
feeding the evaluated value back into a new structured unit. The recursive
acceptance case deterministically computes 55, preserves source diagnostic
codes, and rejects an extern call with status 7002. The artifact schema is 8.

M9 replaces the compiler's internal `0 == failure` convention with real Lain
data. `M9CompilerState` owns an opaque syntax reference, phase, module count,
optional unit and an inline bootstrap diagnostic slice. Finishing the state
produces `M9CompileResult`; a failed result never exposes a partial unit and
retains diagnostic code, message and source extent. The public integer-return
entry remains only as a CLI/bootstrap compatibility projection.

Aggregate results are passed by address in the current physical ABI. Their
allocations therefore live for the complete interpreter run, and typed field
metadata survives L1 text round-trips. M9 uses a tagged-result field while
match-as-value lowering is incomplete; this is the bootstrap tagged-struct
representation permitted by the core contract, not a return to integer error
codes. The reusable artifact executes the state transition probes, compiles
valid source, rejects invalid source atomically, and includes
`compiler_state.lain` in its incremental self-compile closure. The artifact
schema is 9.

The package path is imported as `packages::lain::compiler::*`.

## M10-M13 fixed-point self hosting

M10 replaces consumer-side aggregate mirrors with Meta-owned nominal type
identities carried by `lci-v2` semantic interfaces.  ABI shapes remain a
physical contract, but consumers import `M9CompilerState`,
`M9CompileResult`, and related types by identity rather than redeclaring their
fields.

M11 moves the growable compiler context into `compiler_context.lain`.  Lain
owns vector growth, symbol tables, lexical scope stacks, diagnostics, and the
`CompileResult` policy.  The host exposes only opaque raw storage slots.
The normal compiler API now uses stable `CompilerContext` and `CompileResult`
names; the old M9 state/result implementation is retained only in its own
compatibility module for bootstrap ABI probes.

M12 makes the frontend phases explicit in `frontend_pipeline.lain`:

```text
RawAst root -> ParsedProgram -> MiddleProgram
            -> ElaboratedProgram -> LoweredProgram
```

Each failed phase produces a diagnostic and never exposes a partial
`L1Unit`.  The normal `compiler_frontend_compile` entry runs this Lain-owned pipeline.

M13 compiles the authoritative compiler source closure with that
pipeline.  Foreign declarations are interpreted from ordinary RawAst
attribute topology, relocated to their `link_name`, and deduplicated in the
Lain lowerer.  The resulting structured unit is serialized through the
physical L1 printer; typed loads and canonical LEA/call statement spelling
make the text a lossless reloadable artifact.

The fixed-point gate is:

```text
stage0 C/Scheme bootstrap -> stage1 meta_compiler.l1
stage1 + compiler_source.lain -> stage2_compiler.l1
stage2 + compiler_source.lain -> stage3_compiler.l1

stage2 bytes == stage3 bytes
```

Both generated artifacts pass `l1check`, report compiler API schema 1, and
produce identical LAIN-IR and diagnostics for ordinary compilation and
Meta-owned module workspaces. Run the complete gate with:

```text
zig build test-self-host
```

## M14-M18 command-line compiler and multi-file ownership

The milestones now have concrete boundaries:

- **M14** installs the stage-2 artifact and makes it the normal `lainc` path.
- **M15** gives every parsed source a stable opaque syntax handle owned by a
  `SyntaxStore`/`SyntaxUnit`; destroying another unit cannot invalidate it.
- **M16** builds a real multi-root `ModuleWorkspace`.  Source files are never
  concatenated, and dependency order is independent of command-line order.
- **M17** materializes a Lain-owned `ModuleArtifact` summary whose copied
  names, dependencies and export signatures remain valid after syntax units
  are destroyed.  Stage artifacts use schema-versioned, content-hashed cache
  stamps and are revalidated on every cache hit.
- **M18** defines one `CompileRequest -> compiler_compile -> CompileResult`
  ABI for both single-file and workspace compilation.  The launcher compiles
  once and projects output or diagnostics from that same owned result.

M17 does **not** yet claim persistent per-module incremental code reuse.  The
detached summary and trustworthy cache are the foundation for a future
serialized module artifact; currently the cached compiled unit is still the
whole compiler artifact.

`zig build` now generates `build/core-self-hosting/stage2_compiler.l1`, checks
it with `l1check`, and installs it beside `lainc` as
`zig-out/bin/stage2_compiler.l1`.  Generation is keyed by a SHA-256 input and
output stamp; an unchanged build verifies the artifact without recompiling the
current compiler closure.

The normal CLI loads that artifact and sends both single-file and workspace
builds through one owned request/result ABI:

```text
lainc --emit-l1 input.lain output.l1
  -> CompileRequest(mode=single, paths, sources, count=1)
  -> compiler_compile(request) -> result handle

lainc --emit-workspace-l1 output.l1 math.lain app.lain
  -> CompileRequest(mode=workspace, paths, sources, count=2)
  -> compiler_compile(request) -> result handle

result handle
  -> outcome + L1 text
  -> or diagnostic code/message/path/span
  -> module count
  -> compiler_result_destroy(result)
```

`SourceUnit` and `WorkspaceInput` are Lain-owned compiler structures.  The C
launcher transports bytes and paths only; it does not concatenate source
files or decide module boundaries.  A syntax store keeps every source unit's
tree alive simultaneously.  Lain constructs `ModuleWorkspace`, validates and
orders its dependency graph, and lowers the indexed module roots directly.

`compiler_compile` performs compilation once.  The launcher then projects L1
or diagnostics from the same owned result handle, instead of re-running the
compiler once per diagnostic field.  The result owns copied strings until its
explicit destroy call; the physical storage implementation does not know the
request or result schema.

The artifact host registers only the physical capability allowlist.  It does
not load `polyfills.scm` or `std/meta/driver.scm`.  A failed compilation
projects the Lain diagnostic from the existing result, exits non-zero, and
does not create the requested output file.

Physical function lookup is strict. `core.function-by-name` returns only a
procedure that Meta has already declared in the current `L1Unit`; it never
synthesizes an extern or guesses a fallback signature. An undeclared call
therefore crosses the VM FFI boundary as an exception and cannot leave a
partial output unit. Foreign procedures must enter through an explicit
`@foreign` declaration whose Lain-owned lowering supplies the complete
physical signature and link name.

The remaining stage-0 type bridge is strict as well. It accepts only the
bootstrap compiler's known scalar atoms; standard-library aliases such as
`CStr` and `opaque` are not physical L1 type names, and an unknown name cannot
default to `i32`. Until serialized module artifacts carry transitive nominal
signature dependencies, compiler modules state those dependencies explicitly
and in topological import order (`types` before `compiler_context` before
`modules`). M21 is expected to derive and persist this edge set instead of
requiring the temporary source-level ordering discipline.

The current stage-2 language boundary is deliberate: it covers the compiler's
self-hosting subset, not every form handled by the stage-0 Scheme reference.
Legacy compatibility tests therefore invoke `--bootstrap-emit-l1` explicitly;
the normal command never silently falls back from Lain to Scheme.

## User Meta fixed point

Top-level `std::meta(fn)` bindings are compiled to temporary structured
LAIN-IR and executed by the same Lain-owned interpreter used for consteval.
The only callable externs are the explicit `meta.syntax-*` topology
capabilities. Generated syntax is sealed into immutable units and re-enters
Meta expansion until no user attribute remains.

One initial form may execute at most 64 user Meta transformations. Generated
identifiers use the expansion hygiene context; `syntax_clone` is the explicit
call-site capture operation. All generated units for one compilation share a
single owned syntax store, so origins stay valid across rounds and the
frontend releases the complete expansion lifetime through one exit.

Meta failures distinguish malformed results (`2903`), expansion exhaustion
(`2904`), invalid syntax handles (`2905`), and unavailable capabilities
(`2906`). Diagnostics produced after re-entry include the expansion depth and
resolve their node origin back to the initial call site.

## Enum TypeValues and physical layout

`std::enum` is a Lain-owned Meta constructor, not RawAst or LAIN-IR syntax:

```lain
let Option = std::enum(T: type) {
    None,
    Some(T),
};
```

`enums.lain` validates zero/one-payload variants, interns the declaration as a
nominal `TypeValue`, and interns concrete applications such as `Option(i32)`
as specialized `TypeValue`s. Type discovery walks only actual type positions
(binding expectations, function signatures, local declarations and enum
payloads); it deliberately does not deep-scan every RawAst expression.

The deterministic physical layout is a minimal tag followed by aligned maximum
payload storage. For `Option(i32)` the contract is one tag bit, payload offset
4, total size 8 and alignment 4. Variant constructors allocate that layout,
store the tag and optional payload, and materialize the nominal value as its
physical address. Exhaustive `match` reads the tag, emits structured
conditionals, and loads a bound single payload from the same layout. LAIN-IR
itself gains no enum, variant, pattern or CFG-label concept.

The match elaborator supports zero- or one-payload variants, an arm-local
payload identifier, `_` as the final arm, exhaustive coverage, and a common
arm `TypeId`. Payload bindings participate in normal expression validation and
lowering, so an arm may use forms such as `value + fallback` without exposing
`value` to another arm. Pattern guards and nested destructuring remain later
elaborator work.

`match` may be returned directly or used as a local initializer. Value matches
allocate a result slot and lower to nested structured `if`/`else` regions whose
selected arm stores the result before the common load. The physical builder
only exposes block construction; enum and pattern policy remains entirely in
the Lain-owned elaborator and lowerer.

Diagnostics `3001`-`3003` cover malformed declarations, duplicate variants and
invalid payload shapes. Diagnostics `3010`-`3014` cover a non-enum target,
unknown or duplicate variants, non-exhaustive matches, unreachable arms and arm
type disagreement. The core enum suite compiles and executes
`Option(i32).Some(42)` through an exhaustive match, and executes the layout
contract inside the real compiler artifact with the normal capability
allowlist. The launcher offers
`--artifact <path> --artifact-run <zero-argument-entry>` for these
artifact-level executable contracts.

## Structured conditional values

An `if` is an ordinary typed expression and can initialize a local or appear
inside another conditional:

```lain
let selected: i32 = if ready {
    if cached { 40 } else { compute() }
} else {
    0
};
```

The elaborator requires a `bool` condition, exactly one expression per branch,
and the same `TypeId` from both branches. Lowering allocates one result slot,
emits structured then/else regions that store only the selected value, and
loads the result after the region. Single-file and workspace compilation use
the same policy. LAIN-IR gains no conditional-value node or CFG label.

## Local mutation and structured loops

The self-hosted compiler recognizes assignment and loop control as Meta-level
body forms without adding parser keywords:

```lain
let value: i32 = 0;
loop {
    value = value + 1;
    if value == 2 { continue; }
    if value == 4 { break; }
}
```

An assignment target must resolve to a preceding local `let`; parameters and
unresolved names are not mutable targets. The assigned value must have the
same `TypeId` as that local. `break` and `continue` are accepted only under a
lexically enclosing loop, including inside nested structured `if` blocks.

Lowering maps these forms to the existing physical `SET`, `LOOP`, `BREAK`,
and `CONTINUE` instructions. Falling off a `#loop` body begins the next
iteration, consistently in the interpreter and C backend. No CFG labels or
source-level loop nodes are added to LAIN-IR.
