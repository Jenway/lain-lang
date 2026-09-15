# 类型表达式末尾节点修复

限定类型及引用类型在参数列表末尾没有后继节点时，`meta_type_end`
返回最初的类型节点，导致参数解析停在 `std::type` 中间的冒号。
两条 nil 分支现返回扫描后的 `%current`。

默认 bootstrap 编译器已从源码重建；`check_type_expression_end.py`
七项通过，`check_bits_type.py` 位宽正反例通过，`git diff --check` 通过。
该检查加入主基线，门禁数量由 44 增至 45。
隔离相同修复此前完成 46 文件闭包编译及 verifier 验证。

该切片仅修复类型表达式边界。普通类型工厂的调用、绑定与返回语义仍待完成。
