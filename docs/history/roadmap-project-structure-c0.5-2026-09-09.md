# C0.5：项目结构分离完成记录

完成日期：2026-09-09。

完成了 `seed -> bootstrap -> src` 的源码边界：

- `seed/` 保留 C 实现的 LAINIR 执行底座及 LAINIR 写的 LAINIR 编译器；
- 原 `src/lainir/lain/` 迁入 `bootstrap/compiler/`，原
  `src/lainir/bootstrap_std/` 迁入 `bootstrap/std/`；
- 冻结启动产物迁为 `bootstrap/lainc.l1`，snapshot 迁为
  `bootstrap/lainc.l1.snapshot.json`；
- 新增 `src/lainvm/` 作为正式 LAINVM 的源码边界；实际执行代码仍留在
  `src/lainir/api/l1_interpreter.lain`，等待 C1 定义接口、C3 迁移；
- 构建脚本、检查脚本、CI 和当前文档切换到新路径。

验证：C seed 可构建；冻结产物的暂存版本可由 seed 的 `lainir-print` 加载并执行
`compiler_compile`；snapshot 记录的 artifact SHA-256 为
`040db3e890b523674f2d49d57277b422de14e5efd3a09f9f12984793dfb131cf`。

工作区原有的 `bootstrap/lainc.l1`、`raw_ast.l1` 和 snapshot 编辑未纳入本阶段提交。
它们在迁移后保留为未提交修改，后续单独处理。

后续修正：生成的 `lainc.l1` 和 snapshot 不属于 `bootstrap/` 源码树，统一改为写入
`build/bootstrap/`。`bootstrap/` 只保留手写 LAINIR 源码。
