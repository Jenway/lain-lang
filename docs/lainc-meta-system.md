# src/lainc meta system

The Lain-written compiler (`src/lainc/lainc.lain`) now carries a compile-time
meta layer on top of its text emitter.  Following the project principle
(*meta decides; LAIN-IR describes, executes, verifies*), the meta layer owns
binding, lookup, and compile-time evaluation; the emitter only writes the
LAIN-IR text the meta layer decides on.

## Meta tables

All meta objects live in text tables (rows), referenced by 1-based row
numbers (`usize`).  Row format: `kind|name_start|name_length|payload_start|
payload_length|` (5 fields, 5 `|`).

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
- **Records**: `std::struct` declarations write a layout table
  (`Name:field#offset#width#...=size|`); constructors expand to
  `#alloca(size)` + per-field `#store`; field access `p.x` resolves the
  variable's type through a per-function type table and the layout table
  into `#load[#bits<W>](#lea(base=%p, ..., offset=off))`.

## Known limitations

- consteval interpretation is single-level (no nested `a + b * c`), no
  recursion, no calls inside consteval bodies.
- Multi-argument consteval works; argument/parameter counts must agree.
- Module values are bound (kind 2) but member resolution is still textual
  (`mod.member` -> `f0_member`), not yet meta-driven.
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
