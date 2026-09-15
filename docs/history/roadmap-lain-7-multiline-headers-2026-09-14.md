# 编码 7：多行过程头与外部声明切片

日期：2026-09-14。仅归档过程头读取修复，完整后端/native 阶段未完成。

## 修改

`src/lainc/backend_c.lain` 将 LF 作为空白读取物理类型与参数名，定义/声明两遍扫描均
等到过程体 `{` 或外部声明 `;` 后才派发。参数表、箭头与返回类型可跨物理行。
过程头终止符扫描忽略行注释中的符号。没有增加 Parser 语言规则或修改 seed。

差分测试的临时产物移入 `build/`，符合仓库生成物约束。

## 验收

- 修复前新增多行参数/返回类型用例被 backend 拒绝（session 92529，退出 1）。
- 修复后通过 verifier 并 lower 到 C（session 73851，退出 0）。
- `check_backend_c_shape.py` 退出 0；新增定义和外部声明保留 addr、8/32/64 位
  参数与返回类型，错误的 `(void)` 原型不存在。
- `check_backend_differential.py` 从当前源码重建 backend，11 个用例全部通过并退出 0
  （session 85828）；新用例两个后端均返回独立预期值 42。
- `check_native_backend_canonical_diff.py` 退出 0（session 84801），保持历史 fixture 的过程签名及函数体一致。
- 当前 `build/bootstrap/compiler_core.l1` 整体 lower 退出 0，生成 129474 字节 C；
  `zig cc -c` 退出 0。234 个 unsupported 标记均承载原始源码注释，没有指令标记。

## 限定

整库 C 编译成对象不证明宿主 ABI 链接、native 编译器运行或 Lain 固定点。
含真实 `#eval` 的 bundle 仍必须先完成编译期执行，再进入 backend。
浮点、bitcast/proc_addr、间接调用、alloca 生命周期和错误拒绝规则仍需完成。

本次编译扫描器时发现普通 lowering 对布尔表达式中 `index + 1 < end` 的优先级处理
生成非法转换（verifier 2024）；扫描器以显式局部值表达该计算。反例已加入
`check_local_assignment_types.py`，由阶段一继续跟踪，未归档为完成。
