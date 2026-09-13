# 历史文档索引

这里保存已经完成、被替代或只用于解释设计来源的文档。它们不构成当前计划，也不应作为实现状态的依据；当前状态只看 [`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md) 和代码中的 contract checks。

## 当前完成快照

- [`roadmap-lain-completed-2026-09-07.md`](roadmap-lain-completed-2026-09-07.md)：截至 2026-09-07 已有可执行证据的 compiler、backend、bootstrap、Eval contract 和 LAINIR 物理语义。

### Eval 契约与项目结构完成记录（2026-09-09）

以下五份是 C0–C3 与 C0.5 的完成快照，记录了 `#eval` 执行契约的固定和 `seed → bootstrap → src`
源码边界的分离过程：

- [`roadmap-eval-c0-2026-09-09.md`](roadmap-eval-c0-2026-09-09.md)：删除错误的求值结果协议（`EvalResult`、结果种类、资源归属、代际、sidecar、payload adapter 和 `LainirVmSession`）。
- [`roadmap-eval-c1-2026-09-09.md`](roadmap-eval-c1-2026-09-09.md)：固定 `#eval`、临时 TCB 与 Trap 的执行契约（按值捕获、共享 VSpace、共同预算、只交付普通 LAINIR 值）。
- [`roadmap-eval-c2-2026-09-09.md`](roadmap-eval-c2-2026-09-09.md)：C seed 改为通过临时 TCB 执行 `#eval`（独立 VM control 与根 frame）。
- [`roadmap-eval-c3-2026-09-09.md`](roadmap-eval-c3-2026-09-09.md)：把 Lain 写的解释器从 `src/lainir/api/` 分离到 LAINVM。
- [`roadmap-project-structure-c0.5-2026-09-09.md`](roadmap-project-structure-c0.5-2026-09-09.md)：完成 `seed → bootstrap → src` 的源码边界分离。

**注意**：C3 记录的 Lain LAINVM 解释器后来被**暂停并归档**（见
[`formal-implementations/`](formal-implementations/) 与本索引末尾的说明），该快照描述的是
归档前的状态。

## 架构和迁移决策

- [`roadmap-lainc-lainir-api-2026-09-07.md`](roadmap-lainc-lainir-api-2026-09-07.md)：lainc 与 LAINIR provider/API 边界的详细迁移记录。
- [`lainc-meta-system.md`](lainc-meta-system.md)：早期 `src/lainc` Meta 表、编译期求值和模块系统的实现记录。
- [`lainc-meta-roadmap.md`](lainc-meta-roadmap.md)：正式标准库、`#eval` 和 Meta 分工从旧方案迁移到当前方案的实施记录。

## 编译器和自举实施记录

- [`lainc-bootstrap-roadmap.md`](lainc-bootstrap-roadmap.md)：bootstrap 标准库、正式标准库和 compiler source closure 的阶段性实施记录。
- [`lainir-implementation-roadmap.md`](lainir-implementation-roadmap.md)：LAINIR frontend、verifier、C backend 和 self-host 的已完成里程碑。
- [`lainc-performance-analysis-2026-08-21.md`](lainc-performance-analysis-2026-08-21.md)：旧 bootstrap 路径的性能测量、瓶颈分析和优化结果。
- [`lainir-tools.md`](lainir-tools.md)：已归档的 LAINIR formatter、highlight、parser 和 LSP 实现记录。
- [`lainir-tools/`](lainir-tools/)：上述工具的归档源码，保留归档前实现，不参与当前构建。

`lainir-lsp` 的 C 宿主（`seed/src/host/lsp_host.c`、`seed/src/host/lsp_host.h`、
`seed/src/cli/lsp_main.c`）已删除，不再参与任何构建；LAIN-IR 侧实现保留在
`lainir-tools/`，Lain 成熟后将直接用 Lain 重写 LSP。

## Scheme 时期设计快照

以下文档保留早期语言设计、AST、Meta 和编译流水线，目的是解释当前设计的来源，不代表当前语法或实现：

- [`scheme-era-intro.md`](scheme-era-intro.md)
- [`scheme-era-core-philosophy.md`](scheme-era-core-philosophy.md)
- [`scheme-era-architecture.md`](scheme-era-architecture.md)
- [`scheme-era-lain-ast.md`](scheme-era-lain-ast.md)
- [`scheme-era-meta-system.md`](scheme-era-meta-system.md)

## 已清理的重复路线图

以下内容已从仓库删除，因为其信息已合并到当前路线图或完成快照，或者描述的对象已经不存在：

- 独立 `src/compiler-archive` 的计划；
- 2026-09-07 的旧 bootstrap 快照；
- 2026-09-07 的旧 VM 路线快照；
- 2026-09-07 的旧 LAINIR maintenance 路线快照；
- 重复的 LAINIR compiler roadmap。
