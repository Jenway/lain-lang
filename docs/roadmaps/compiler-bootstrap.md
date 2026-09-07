# Lain 编译器自举计划

基线日期：2026-09-06。

## 当前基线

项目已有可执行的 C seed、LAINIR 编译器、bootstrap 标准库、正式标准库源码，以及统一在 `src/lainc` 下的 Lain 编译器源码。compiler core 与标准库通过阶段 ABI 协作；Meta 负责语言规则和 lowering，编译期计算通过 LAINIR `#eval` 执行。

当前第一个失败命令是：

```text
python scripts/build_formal_stdlib.py
```

它返回 `3003`，即 `invalid enum variant payload`。因此现存的 `build/lainir/formal_stdlib.l1` 不能视为当前源码的有效构建结果。

## 1. 恢复正式标准库构建

目标：从当前源码重新得到有效的 formal stdlib artifact。

工作：

- 找到产生 `3003` 的 enum variant、payload 类型和源位置；
- 检查类型值经过 closure specialization 后是否正确进入 physical lowering；
- 修复 payload 的物理类型、size 或 alignment 传播；
- 不在 host 或 LAINIR parser 中增加类型特例或兼容语法；
- 重新生成 formal stdlib、ABI probe 和 manifest；
- 清除测试对旧 formal artifact 的依赖。

完成条件：删除现有 formal 构建产物后，`build_formal_stdlib.py` 仍成功；新产物通过当前 LAINIR parser、verifier 和标准库 ABI probe。

## 2. 让正式标准库成为默认语义实现

目标：compiler core 可以加载正式标准库完成完整的 expand、elaborate 和 lower。

工作：

- 补齐 binding 和 type elaboration；
- 补齐一般函数调用、控制流和完整 LAINIR module 生成；
- 统一 scalar、type、module、AST 四类 `#eval` 结果及 owner 转交；
- 让标准库 artifact 产生稳定诊断并保留 source span；
- 所有标准库测试经公开阶段 ABI 调用生成的 artifact。

完成条件：完整 `std/**/*.lain` 由 bootstrap 标准库编译，正式标准库 fixture 全部使用生成的 artifact 运行。

## 3. 建立标准库替换证明

目标：证明正式标准库可以取代 bootstrap 实现。

工作：

- 对 function、struct、module、import、type factory、attribute、generic、effect、bounds 和 `#eval` 建立正负例；
- 比较两套实现产生的 canonical AST、诊断、依赖图、canonical LAINIR 和运行结果；
- 差异报告定位到第一个 pass、source span 和 IR procedure；
- 建立单一的 stdlib conformance 测试入口。

完成条件：一致性矩阵通过，默认编译流程加载正式标准库；bootstrap 标准库只作为冷启动 snapshot。

## 4. 建立 `lainc -> LAINIR` API 边界

目标：`src/lainc` 依赖稳定的 LAINIR 能力约定，不依赖动态 L1 的具体存储、验证、打印和解释代码。

详细阶段、接口职责和验收条件见 [`lainc-lainir-api-migration.md`](lainc-lainir-api-migration.md)。

工作：

- 冻结 BuilderApi、ArtifactApi 和 EvalApi v1；
- 由当前 seed/LAINIR 实现这些能力并通过 contract tests；
- 依次迁移 lowering、compiler result 和 Meta eval；
- 删除 `src/lainc/l1_ir.lain`、builder、verifier、printer 和 interpreter；
- 在迁移过程中保持 canonical artifact、诊断和执行结果的差分测试。
- EvalApi 同时接入 [`lain-vm.md`](lain-vm.md) 定义的 session、activation、quota、
  capability 和 trap contract。

完成条件：`src/lainc` 中没有第二套 LAINIR 实现，所有 LAINIR 构造与执行都通过已版本化的能力 API。

## 5. 完成 `src/lainc`

目标：当前 bootstrap 编译器从 `src/lainc/COMPILER_SOURCES.txt` 所列源码产生能编译真实程序的 Lain 编译器。

工作：

- 完成一般表达式、函数调用和控制流；
- 完成 module factory、captured binding 和 qualified member；
- unresolved member 必须产生诊断，不能被转换为 extern、空地址、固定偏移或零值；
- `src/lainc` compiler API 的输出必须重新通过 LAINIR verifier、printer 和执行器；
- 用多文件、module、record、函数调用和控制流程序扩展真实程序 gate。

完成条件：格式验证、empty API、nonempty API 和真实用户程序四个 gate 同时通过。

## 6. 建立 `src/lainc` 固定点

目标：由 `src/lainc` 生成的编译器接管自身构建。

工作：

- bootstrap 编译器编译正式标准库和 `src/lainc`，生成 gen2；
- gen2 以相同输入生成 gen3；
- 比较 canonical artifact、verifier 结果、ABI manifest 和 procedure body hash；
- 从干净目录只用 seed、bootstrap snapshot 和源码重建整条链；
- 生成 native `lainc` 并运行真实程序回归。

完成条件：gen2 与 gen3 一致，clean rebuild 和 native 回归同时通过。

## 7. 切换工具链并清理过渡实现

目标：项目结构与既定编译边界一致。

工作：

- CLI、LSP 和测试 runner 使用同一正式标准库 artifact/version；
- 从默认构建、发布和测试中移除过渡编译器；
- 删除 compiler core 中的高级形式名称判断和 `src/lainc` 过渡物理偏移；
- 保留带 ABI version、source hash 和 artifact hash 的 bootstrap snapshot；
- 删除后重新运行固定点和 clean rebuild。

完成条件：没有过渡实现参与默认链，仍可从提交的 seed 和 snapshot 重建 native `lainc`。

## 当前范围之外

- 语言级 ownership、borrow checker 和生命周期推导；
- 新增另一套 parser、IR、Meta evaluator 或编译器；
- 在 C host 中实现 Lain 类型、module、effect 或宏语义；
- 通过 LAINIR compatibility mode 绕过 Meta 类型传播问题。
- 在编译器固定点之前实现完整 LAIN-VM 微内核对象或平台 lowering；当前只推进
  `#eval` 所需的 VM session contract，详见 [`lain-vm.md`](lain-vm.md)。
