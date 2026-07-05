# Bootstrap Core Tests

这些测试定义 Lain `0.1.0` 的旧世界完成线。

这里不追求覆盖所有语言特性。每个测试都必须证明旧世界能支撑一块 Lain-meta compiler code。

## Layout

```text
tests/bootstrap-core/
  data_structures/
  control/
  meta/
  comptime/
  ir_builder/
```

## Required Suites

### data_structures

Purpose:

```text
compiler data can be modeled in Bootstrap Core
```

Initial cases:

```text
option_result.lain
list_slice.lain
enum_match.lain
ast_tree_structs.lain
```

### control

Purpose:

```text
parser/lowerer control flow can be written in Lain
```

Initial cases:

```text
match_nested.lain
early_return_result.lain
list_traversal.lain
```

### meta

Purpose:

```text
small compiler passes can be written as Lain code
```

Initial cases:

```text
split_comma_group.lain
parse_type_application.lain
parse_fn_shape.lain
diagnostic_builder.lain
```

### comptime

Purpose:

```text
Lain-meta helpers can execute at compile time through LAIN-IR
```

Initial cases:

```text
comptime_build_ast.lain
comptime_parse_type.lain
comptime_read_fixture.lain
```

### ir_builder

Purpose:

```text
Lain code can drive a safe IR builder wrapper
```

Initial cases:

```text
build_const_return_fn.lain
build_call_fn.lain
emit_l1_smoke.lain
```

## Pass Rule

`tests/bootstrap-core` should become part of the `0.1.0` release gate.

Cases are listed in `cases.txt`.

```text
active   runnable acceptance slice
pending  named contract that still needs a runnable .lain case
```

The default runner reports pending cases but does not count them as pass.

```text
python tests/bootstrap-core/run_bootstrap_core.py
```

Use this before `0.1.0` freeze:

```text
python tests/bootstrap-core/run_bootstrap_core.py --strict-pending
```

## Non Goals

Do not add tests here for:

```text
general feature demos
old regression fixtures
effect experiments
interface experiments
backend optimizations
syntax bikeshedding
```

Those belong in core contract tests or legacy fixtures.
