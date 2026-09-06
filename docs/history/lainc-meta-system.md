# src/lainc meta system

当前执行路线见 [`docs/roadmaps/compiler-bootstrap.md`](../roadmaps/compiler-bootstrap.md)。

> **历史实现说明。** 本文记录 `src/lainc` 过渡编译器的现状，其中把 Meta
> 描述为 callable registry/evaluator 的段落不再代表目标架构。当前定义是：Meta
> 负责 AST 操作和变换，编译期执行由 Meta 生成的 LAIN-IR `#eval` 完成。
> 文中的 `src/compiler-archive` 后来已经合并进 `src/lainc`。

The reusable Meta callable registry is now defined in `std/meta.lain` and
instantiated by `src/compiler-archive/meta.lain`.  The archive layer owns
syntax expansion and interpreter orchestration; the standard library owns the
phase data model (`Callable`, `Registry`, registration, and lookup).

`std/meta.lain` also owns `CallablePhase`, phase-aware registration, nominal
type interning, module-value interning, and the shared `ComptimeValue`
constructors/comparator.  `compiler-archive/types.lain` is a compatibility
facade over these standard definitions, so compiler code can depend on
`std::meta` without importing archive AST types.

The archive Meta factory now exposes scalar-handle descriptors for
`MetaProgram`, `MetaProcedure`, `MetaEnvironment`, `MetaBinding`, `MetaValue`,
and `MetaDiagnostic`.  A program descriptor records its stable semantic id,
unit/source/table handles, and procedure metadata; an environment records its
parent and binding-table handle.  These handles keep physical L1 ids separate
from Meta identity while avoiding premature container specialization during
self-hosting.

The archive Meta factory exposes `evaluate` and `invoke_program` as the
structured execution bridge.  They run a selected procedure through the
Lain-owned `l1_interpreter` with a capability-free external-call table;
external procedures are rejected with interpreter status 7102 during Meta
evaluation.

The Lain-written compiler (`src/lainc/lainc.lain`) now carries a compile-time
meta layer on top of its text emitter.  Following the project principle
(*meta decides; LAIN-IR describes, executes, verifies*), the meta layer owns
binding, lookup, and compile-time evaluation; the emitter only writes the
LAIN-IR text the meta layer decides on.

## Meta tables

The standard `TypeRegistry` assigns stable built-in type ids 1–7, interns
specialized types by kind/owner/argument tuple, and stores module values by
their stable module id plus namespace.  Repeating an interning request returns
the existing identity instead of allocating a second Meta value.

All meta objects live in text tables (rows), referenced by 1-based row
numbers (`usize`).  Row format: `kind|name_start|name_length|payload_start|
payload_length|mod_row|` (6 fields, 6 `|`); `mod_row` is the 1-based row of
the owning module (0 = top-level) and makes member resolution
namespace-aware.

| kind | meaning                                   |
|------|-------------------------------------------|
| 1    | scalar constant (consts table)            |
| 2    | module                                    |
| 3    | function                                  |
| 4    | record type                               |
| 6    | consteval (compile-time) function         |

Tables and operations:

- `meta_pipe_count` / `meta_rows` / `meta_field` / `meta_row_field` —
  row/field parsing (subtractive division; no `/`).
- `meta_append` — append a row, returns its number.
- `meta_lookup` — find a row by name span (names live in the source buffer;
  the table stores source offsets).
- `meta_lookup_qualified` — find a row by name span among rows owned by a
  given module row (field 5); used for module member resolution.
- `meta_parse_decimal` / `meta_append_decimal` (digits lookup table +
  subtractive division) / `meta_is_number` / `meta_eval_const` /
  `meta_fold_binary` — numeric parsing and constant folding.

## Compile-time evaluation

- **Literal folding**: `emit_add_expr` folds `literal op literal` for
  `+ - *` (subtraction only when left >= right, usize).
- **Top-level constexprs**: `let N: i32 = 40 + 2;` binds `N -> 42` into the
  consts table; `emit_operand2` splices the stored value at use sites
  (`N + 1` becomes `#add(5, 1)`).
- **consteval calls**: `let square = std::consteval(n: i32) -> i32 { return
  n * n; };` binds a kind-6 row (payload = params pos, body pos) and emits
  **no product**.  A top-level initializer that is a call to a kind-6 row is
  interpreted: parameters are bound from the argument list, the `return`
  expression is evaluated, and the result is bound into consts.
  - `square(5) -> 25`, `add2(40, 2) -> 42`, `five() -> 42`.
  - Qualified calls `math.square(6)` resolve through the module's row to the
    kind-6 member (see Modules).
  - The evaluator (`meta_eval_expr` + `meta_eval_nested` +
    `meta_eval_nested_call` + `meta_eval_bool`) handles:
    - nested arithmetic (`x * y + 1`, `a + b * c`) with an iterative
      shunting-yard (two explicit stacks, no recursion);
    - nested argument expressions (`calc(2 + 3, 4 * 2)`);
    - local `let` bindings inside the body (`let base = x * 10; return
      base + 1;`);
    - references to already bound top-level constants (the consts table is
      copied into the parameter environment);
    - `if / else if / else` chains with `== != < <= > >=` comparisons and
      boolean `&&` / `||` conditions (iterative, precedence-aware);
    - calls to other consteval functions — bare (`square(a)`) and
      module-qualified (`math.square(a)`) — where the callee's body is
      evaluated purely (calls=0) and the caller's parameter environment is
      threaded into the callee, so argument expressions can reference caller
      parameters; calls therefore cannot nest inside a callee;
    - calls inside `if`/`else` conditions: comparison operands may be
      consteval calls (`if identity(n) == 0 || n == 10 { ... }`) via
      `meta_eval_cond_single_call`, whose callees are also evaluated purely;
    - top-level initializers that are nested constant expressions over
      bound constants (`let answer = a * 1000 + b * 100;`).
- **Records**: `std::struct` declarations write a layout table
  (`Name:field#offset#width#...=size|`); constructors expand to
  `#alloca(size)` + per-field `#store`; field access `p.x` resolves the
  variable's type through a per-function type table and the layout table
  into `#load[#bits<W>](#lea(base=%p, ..., offset=off))`.

## Modules

A module is a meta **value**: `let NAME = std::module { ... };` binds a
kind-2 row (payload = body position) and emits no runtime object.

Members are namespace-isolated:

- **Functions** are emitted as `f0_<mod>_<member>` (`emit_label` adds the
  module prefix; `main`/`compiler_compile` special-casing is suppressed for
  members).  A call `mod.member(args)` resolves through the meta table:
  the first path segment is looked up (must be kind 2), then the call is
  emitted as `#call f0_<mod>_<member>` (`emit_operand2` / `emit_call_named`).
  Two modules may use the same member name without colliding.
- **Constants** (`let N = <const-expr>;` inside a module) are bound into the
  consts table with the module's row recorded; a qualified reference
  `mod.N` resolves via `meta_lookup_qualified` and splices the stored value.
  A bare `N` reference inside the module also resolves (unqualified lookup
  scans all rows).
- **consteval members** (`let F = std::consteval ...;` inside a module) are
  bound kind 6 with the module's row; a top-level `let X = mod.F(args);`
  folds the call to a constant.

Calls to module members from **inside** the module must be fully qualified
(`mod.other(...)`), since the emitter does not thread the enclosing module
into expression lowering.  Nested modules are not yet supported (a nested
`std::module` inside a module is skipped).  Cross-module name collisions
for bare (unqualified) constant references pick the first binding.

## Known limitations

- The archive `Meta.invoke` AST-expansion boundary is phase-checked and has a
  working procedure-0 identity primitive.  Scalar user-defined procedures can
  use `invoke_program`, where the caller supplies the structured L1 unit and
  argument frame.  The AST path still needs a program/environment handle so a
  numeric procedure id can be connected to a syntax-producing callable.
  Unknown or non-Meta phases return diagnostic 2906/2907 rather than silently
  running.

- consteval interpretation has no recursion and calls cannot nest inside a
  callee: a compile-time function may call another consteval function, but
  the callee's own body is evaluated purely (its return expression and
  arguments cannot contain further calls).  `&&` / `||` conditions,
  `else if` chains, and calls inside conditions are supported.
- A callee's parameters shadow same-named bindings in the caller's
  environment (`meta_consteval_copy_outer_skip`), so `outer(n)` calling
  `inner(n)` binds each function's own `n` correctly.
- Multi-argument consteval works; argument/parameter counts must agree.
- consteval members are only callable from top-level constant initializers
  (function-body calls to a consteval member are not folded).
- The frozen front-end (used to bootstrap gen1) has quirks that the meta
  code works around: compound expressions keep only the first operation
  (`a - b - c` must be split), `0 - 1` in comparisons must go through a
  variable, `#add/#sub` promote to 64-bit (arithmetic lives in `usize`),
  statements after an inner while inside an outer loop are dropped (factored
  into helper functions), and value-returning recursion lowers incorrectly
  (consteval evaluation is deliberately non-recursive).

## Verification

The source-closure, compiler-API, nested-namespace, standalone Meta, and
archive-usable tests are green; the module-consteval member fixture remains a
known hanging gate after the first three module fixtures pass.  The generated gen2 compiler parses and
compiles the complete std-plus-archive input, producing a real archive API
artifact.  `lainir-print` verifies that artifact, and its `main` calls
`api.compile` on the empty fixture and returns `0` under `lainir-seed run`.
The seed runner supplies the minimal allocator pair
`bootstrap.allocate-pages`/`bootstrap.release-pages` needed by generated
self-hosting artifacts; source/artifact capabilities remain exclusive to the
bootstrap compilation command.  On Windows, small external
pages now use the process heap and large pages use demand-zero `VirtualAlloc`;
the latest gen1 profile measured 112.2 MiB peak RSS and 113.9 MiB peak VMS,
with a host allocation budget available as a safety limit.

The remaining limitation is semantic coverage: the archive Meta AST-expansion
bridge still needs a program/environment handle for arbitrary syntax-producing
procedures.  The fixed-point compiler and the empty `api.compile` path are
working, while full language-feature coverage still depends on completing that
bridge and replacing the archive's remaining data-model stubs.

Host allocation ownership is explicit: `lainir-seed run` registers only the
allocator pair, while the bootstrap command owns source/artifact capabilities.
Both paths track external pages, honor explicit release, and reclaim any
remaining pages at run end.  `LAINIR_BOOTSTRAP_MAX_ALLOC_BYTES` and
`--max-alloc-bytes` provide diagnostic ceilings when investigating runaway
lowering.
Probe programs live under `build/eval-demo/` (not tracked): `const_probe`,
`constexpr_probe`, `ce_probe`, `ce2`, `ce4`, `rec_probe`, `ns_probe`,
`meta_integration`.
