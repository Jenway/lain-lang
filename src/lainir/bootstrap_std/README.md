# 第一代标准库

这里放用 LAIN-IR 编写的 bootstrap standard library。它是正式 `std/*.lain`
能够被编译以前的启动实现，不是另一套语言规范。

当前已经冻结 `lain_std_abi_version`、`lain_std_initialize`、
`lain_std_expand`、`lain_std_elaborate` 和 `lain_std_lower` 五个入口。版本和
上下文初始化由标准库提供；expand 会遍历 source unit，执行 module、record、
struct 和 import 校验，并返回带 owner 的 pass-result；elaborate 会检查展开
结果；lower 会调用标准库内的 lowering 实现，当前覆盖常量、算术、函数调用、
局部绑定、模块成员、struct 字段和简单 `if`。

旧的 LAIN-IR Meta 求值器、lowering 和求值结果包装已经删除。正式标准库重新接入
LAIN-VM 的 `#eval` 路径以前，这个目录不提供可用的编译期求值实现，也不能单独
重建旧的 frontend artifact。

完整类型/泛型语义、完整控制流和完整 L1 Unit 生成仍未完成；这些是剩余迁移项，
不应再写回 compiler core。

`scripts/build_lain_compiler.py` 会分别生成：

```text
build/lainir/lain_compiler_core.l1
build/lainir/bootstrap_std.l1
build/lainir/lain_compiler.l1
```

最后一个是供 seed 执行的组合 artifact；前两个用于检查边界，确保标准库
不是悄悄重新拼回 compiler core。
