# Lain 文档索引

本目录把文档分为语言设计、当前实现、路线图和历史资料。文档正在逐篇校准；在校准完成前，当前实现事实以代码和 [`roadmaps/`](roadmaps/) 中的当前路线图为准。

## 文档优先级

文档按以下顺序作为单一事实源，前者覆盖后者：

```text
代码
  > docs/roadmaps/lain-roadmap.md
  > docs/00-04 规范稿
  > docs/implementation/
  > docs/history/（仅追溯）
```

`docs/history/` 中的文档只用于追溯设计演变和测量结果：其中的路径与命令反映归档时的状态，可能指向已经不存在的文件（例如已删除的 `tests/` 目录），不得作为当前实现的依据。

## 建议阅读顺序

1. [`00-intro.md`](00-intro.md)：语言目标、编译流程和各层边界。
2. [`01-lain-ir.md`](01-lain-ir.md)：LAINIR 语法和执行模型。
3. [`02-lain-ast.md`](02-lain-ast.md)：无语义语法树契约。
4. [`03-meta-system.md`](03-meta-system.md)：Meta 层的职责。

## 语言设计与规范

| 文档 | 内容 | 当前状态 |
| --- | --- | --- |
| [`00-intro.md`](00-intro.md) | Lain 总览、核心原则和编译器架构 | 已合并初稿，待逐节复核 |
| [`01-lain-ir.md`](01-lain-ir.md) | LAINIR 语言规范（**第一代方言**：用 `#let`、类型实参可省） | 代码示例已与实现不符。**第二代的实际规范是 [`../seed/docs/LAINIR.md`](../seed/docs/LAINIR.md)**（类型实参必写、`#loop` 体落下即离开循环）；两份尚未归一 |
| [`02-lain-ast.md`](02-lain-ast.md) | LAIN-AST 契约 | 待校准 |
| [`03-meta-system.md`](03-meta-system.md) | Meta 系统、标准阶段 ABI 与 `#eval` 边界 | 已重写初稿，待逐节复核 |

## 标准库设计

这些语言能力由标准库 Meta 层定义。compiler core 只提供通用 AST、LAINIR、诊断和 capability 底座。

| 文档 | 内容 | 当前状态 |
| --- | --- | --- |
| [`stdlib/effect-system.md`](stdlib/effect-system.md) | effect、错误处理、异步和编译期 capability | 待校准 |

## 当前实现说明

| 文档 | 内容 |
| --- | --- |
| [`implementation/ast-operations.md`](implementation/ast-operations.md) | RawAst 上的语义视图、遍历、变换和宏展开探针 |
| [`implementation/lain-written-backend.md`](implementation/lain-written-backend.md) | Lain 编写的 LAINIR-to-C 后端与 native driver 的当前状态 |
| [`implementation/lain-backend-capability-abi.md`](implementation/lain-backend-capability-abi.md) | backend capability ABI v1：`src/lainc/backend_c.lain` 的宿主边界、`backend.*` capability 集合与 manifest 契约 |
| [`implementation/native-backend-migration-report.md`](implementation/native-backend-migration-report.md) | native C 后端迁移状态：已验证项、待端到端执行的改动和当前阻塞 |
| [`implementation/c1-eval-contract-audit.md`](implementation/c1-eval-contract-audit.md) | C1 `#eval` 执行契约审计：临时 TCB、共享 VSpace、预算与 Trap 的冻结边界 |
| [`implementation/module-namespaces.md`](implementation/module-namespaces.md) | `packages::lain::` 逻辑命名空间与 import 解析规则 |
| [`implementation/meta-callable-unification.md`](implementation/meta-callable-unification.md) | 编码 1b 设计：统一 Meta callable 执行路径的现状、sink 与 Meta 值构造方案 |
| [`implementation/architecture-review-2026-09-20.md`](implementation/architecture-review-2026-09-20.md) | 架构评审与路线合并结论：两套 Meta ABI 的实测对照、证据分级（R/D/A/U）、四项可执行证明的验收设计、`std/` 归属决定、差异台账。**路线图 §5 的依据** |
| [`implementation/architecture-review-astra-2026-09-20.md`](implementation/architecture-review-astra-2026-09-20.md) | 同一轮的外部评审（未运行任何命令，纯读代码）；上一条的差异台账逐条对照它 |

## 当前路线图

| 文档 | 内容 |
| --- | --- |
| [`roadmaps/README.md`](roadmaps/README.md) | 当前进度、路线图阅读顺序和最近阻塞 |
| [`roadmaps/lain-roadmap.md`](roadmaps/lain-roadmap.md) | 编译器自举、LAINIR API、Eval VM 和物理层维护总路线图 |

## 历史资料

这些文档用于保存设计演变和测量结果，不定义当前架构。

| 文档 | 内容 |
| --- | --- |
| [`history/lainc-bootstrap-roadmap.md`](history/lainc-bootstrap-roadmap.md) | 旧自举路线快照 |
| [`history/README.md`](history/README.md) | 历史文档分类、保留理由和已清理的重复路线图 |
| [`history/roadmap-lain-1e-stages-2026-09-13.md`](history/roadmap-lain-1e-stages-2026-09-13.md) | bootstrap 库阶段交接实现与剩余语义边界 |
| [`history/roadmap-lain-1d-2026-09-13.md`](history/roadmap-lain-1d-2026-09-13.md) | 编码 1d 的实际归属与编译期 IR 生成路径审计 |
| [`history/roadmap-lain-completed-2026-09-13.md`](history/roadmap-lain-completed-2026-09-13.md) | 2026-09-13 整理移出的实现、验收及历史测量记录 |
| [`history/roadmap-lain-completed-2026-09-07.md`](history/roadmap-lain-completed-2026-09-07.md) | 2026-09-07 前已完成工作的路线快照 |
| [`history/roadmap-lainc-lainir-api-2026-09-07.md`](history/roadmap-lainc-lainir-api-2026-09-07.md) | lainc 与 LAINIR API 边界迁移记录 |
| [`history/lainc-meta-roadmap.md`](history/lainc-meta-roadmap.md) | 2026-09-02 标准库与旧 archive 自举实施记录 |
| [`history/lainir-implementation-roadmap.md`](history/lainir-implementation-roadmap.md) | LAINIR frontend 和 compiler boundary 的旧实现清单 |
| [`history/lainc-meta-system.md`](history/lainc-meta-system.md) | `src/lainc` 过渡 Meta 实现记录 |
| [`history/lainc-performance-analysis-2026-08-21.md`](history/lainc-performance-analysis-2026-08-21.md) | 2026-08-21 自举性能分析 |
| [`history/lainir-tools.md`](history/lainir-tools.md) | 已归档的 LAINIR formatter、highlight、parser 和 LSP 实现记录 |
| [`history/scheme-era-intro.md`](history/scheme-era-intro.md) | Scheme 时期的语言总览 |
| [`history/scheme-era-core-philosophy.md`](history/scheme-era-core-philosophy.md) | Scheme 时期的核心设计说明 |
| [`history/scheme-era-architecture.md`](history/scheme-era-architecture.md) | Scheme 时期的编译器架构说明 |
| [`history/scheme-era-meta-system.md`](history/scheme-era-meta-system.md) | Scheme Meta、旧自举记录和早期 Meta 规范合集 |
| [`history/scheme-era-lain-ast.md`](history/scheme-era-lain-ast.md) | Scheme/AstTree 时期的 LAIN-AST 契约 |

## 当前架构判定

当前主线采用以下边界：

```text
Lain source
  -> RawAst
  -> standard-library Meta: expand / elaborate / lower
  -> LAINIR
  -> verify and execute #eval
  -> runtime LAINIR
  -> interpreter or native backend
```

Meta 负责 AST 操作、语言规则和 lowering。编译期执行通过 LAINIR `#eval` 完成。compiler core 负责阶段调度、诊断、资源归属和标准库 ABI。
