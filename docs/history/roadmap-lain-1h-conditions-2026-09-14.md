# 编码 1h：普通条件使用共同表达式 lowering

日期：2026-09-14。仅归档表达式/条件迁移切片，完整 1h 未完成。

## 修改

bootstrap 库的条件 prefix/expression 改用普通值的 AST 表达式窗口，按最低优先级的
最右运算符分割，保留左结合。删除旧右递归条件 writer 与专用最后运算符扫描。
循环的逻辑运算符选择沿用相同优先级；while 验证完整条件及 body，移除只接受首个
运算符为比较的启动限制。一元 `!` 在共同 operand writer 输出取反。

逻辑值先归一化为 bits<1>，再显式拓宽后求和，避免 bits<1> 的 OR 溢出。
本切片没有新增 core/VM 语言规则，没有改变 effect/handler 执行协议。

## 验收

- `check_condition_expression_lowering.py` 11 个用例通过 verifier，实际运行到独立预期值 42。
  覆盖算术/比较/布尔混用、左结合、乘除优先级、OR/AND、取反、布尔值、嵌套和循环。
- 旧源码原型的首个反例被 verifier 2024 拒绝；测试能暴露原缺陷。
- `check_operator_precedence.py` 与 `check_lainir_boundaries.py` 退出 0。
- 首次正式库重建因遗漏 `!` 输出非法 `%!` 失败；修复后生产重建退出 0（session 44549），
  运行完整 ABI/metadata、共用宏及普通返回表达式回归。
- `check_lainc_lainir_api_snapshots.py` 退出 0，三份规范快照及两条失败不留产物检查通过。
- `check_stdlib_conformance.py` 退出 0，当前全部 IR、执行及导入诊断 fixture 通过。
- `check_function_shape_conformance.py` 退出 0：12 项一致，仍有 1 项类型环境 SKIP。
- `check_backend_differential.py` 从当前 compiler/backend 源码重建，11 个用例全部运行正确，退出 0（session 82773）。
- 生产 manifest 全部源码/产物 SHA-256 与实际文件一致。

新增入口登记到主门禁，目前 43 项；本切片没有重新执行完整 43 道入口。
局部/参数绑定、宏完整语义、类型环境、effect/handler 和完整编译期执行链仍待完成。
不据此宣称短路副作用语义、正式 lainc 运行组合或编译器固定点完成。
