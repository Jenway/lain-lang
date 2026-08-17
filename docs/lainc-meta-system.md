# src/lainc meta system

The Lain-written compiler (`src/lainc/lainc.lain`) now carries a compile-time
meta layer on top of its text emitter.  Following the project principle
(*meta decides; LAIN-IR describes, executes, verifies*), the meta layer owns
binding, lookup, and compile-time evaluation; the emitter only writes the
LAIN-IR text the meta layer decides on.

## Meta tables

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
  interpreted: parameters are bound from the argument list (single-level
  `operand op operand` expressions, no recursion), the `return` expression
  is evaluated, and the result is bound into consts.
  - `square(5) -> 25`, `add2(40, 2) -> 42`, `five() -> 42`.
  - Qualified calls `math.square(6)` resolve through the module's row to the
    kind-6 member (see Modules).
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

- consteval interpretation is single-level (no nested `a + b * c`), no
  recursion, no calls inside consteval bodies.
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

`python tests/lainir_lain/run_lainc_m1.py` and `run_lainc_m2.py` stay green
(gen2 == gen3 fixed point).  Probe programs live under `build/eval-demo/`
(not tracked): `const_probe`, `constexpr_probe`, `ce_probe`, `ce2`, `ce4`,
`rec_probe`, `ns_probe`, `meta_integration`.
