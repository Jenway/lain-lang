# Bootstrap Core Tests

这些测试定义自举编译器仍然依赖的最小 Lain 语言能力。

这里不追求覆盖所有语言特性，也不再保护已经被 M4 Meta artifact 取代的
旧 compiler-core 实现。每个测试都必须证明当前自举路径仍然需要的一项能力。

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
ast_tree_structs.lain
```

### control

Purpose:

```text
parser/lowerer control flow can be written in Lain
```

Initial cases:

```text
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
parse_binding_shape.lain
diagnostic_builder.lain
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
