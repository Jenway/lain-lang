# 第一代标准库

这里放用 LAIN-IR 编写的 bootstrap standard library。它是正式 `std/*.lain`
能够被编译以前的启动实现，不是另一套语言规范。

当前已经冻结 `lain_std_abi_version`、`lain_std_initialize`、
`lain_std_expand`、`lain_std_elaborate` 和 `lain_std_lower` 五个入口。前两个
提供版本和上下文初始化，expand/elaborate 目前是 identity pass，lower 暂时
调用 core hook；语言形式、类型、泛型、effect 和真正的 lowering 仍要逐步迁入，
并由 `#eval` 承担编译期执行。

`scripts/build_lain_compiler.py` 会分别生成：

```text
build/lainir/lain_compiler_core.l1
build/lainir/bootstrap_std.l1
build/lainir/lain_compiler.l1
```

最后一个是供 seed 执行的组合 artifact；前两个用于检查边界，确保标准库
不是悄悄重新拼回 compiler core。
