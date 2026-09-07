# Lain 文档索引

本目录把文档分为语言设计、当前实现、路线图和历史资料。文档正在逐篇校准；在校准完成前，当前实现事实以代码和 [`roadmaps/`](roadmaps/) 中的当前路线图为准。

## 建议阅读顺序

1. [`00-intro.md`](00-intro.md)：语言目标、编译流程和各层边界。
2. [`01-lain-ir.md`](01-lain-ir.md)：LAINIR 语法和执行模型。
3. [`02-lain-ast.md`](02-lain-ast.md)：无语义语法树契约。
4. [`03-meta-system.md`](03-meta-system.md)：Meta 层的职责。

## 语言设计与规范

| 文档 | 内容 | 当前状态 |
| --- | --- | --- |
| [`00-intro.md`](00-intro.md) | Lain 总览、核心原则和编译器架构 | 已合并初稿，待逐节复核 |
| [`01-lain-ir.md`](01-lain-ir.md) | LAINIR 语言规范 | 当前规范，仍需与实现逐项核对 |
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
| [`implementation/ast-operations.md`](implementation/ast-operations.md) | AST 查询、遍历、变换和宏展开 |
| [`implementation/lain-written-backend.md`](implementation/lain-written-backend.md) | Lain 编写的后端和 native driver |
| [`implementation/lainir-tools.md`](implementation/lainir-tools.md) | LAINIR 工具链、构建命令和边界 |

## 当前路线图

| 文档 | 内容 |
| --- | --- |
| [`roadmaps/README.md`](roadmaps/README.md) | 当前进度、路线图阅读顺序和最近阻塞 |
| [`roadmaps/compiler-bootstrap.md`](roadmaps/compiler-bootstrap.md) | 正式标准库、`src/lainc` 固定点和工具链切换主线 |
| [`roadmaps/lainc-lainir-api-migration.md`](roadmaps/lainc-lainir-api-migration.md) | `lainc` 到 LAINIR 能力 API 的依赖反转和迁移步骤 |
| [`roadmaps/lainir-maintenance.md`](roadmaps/lainir-maintenance.md) | LAINIR 后续物理能力和维护工作 |
| [`roadmaps/lain-vm.md`](roadmaps/lain-vm.md) | `#eval` 执行环境、VM contract 和后续平台下沉 |

## 历史资料

这些文档用于保存设计演变和测量结果，不定义当前架构。

| 文档 | 内容 |
| --- | --- |
| [`history/lainc-bootstrap-roadmap.md`](history/lainc-bootstrap-roadmap.md) | 旧自举路线快照 |
| [`history/lainc-archive-plan.md`](history/lainc-archive-plan.md) | 旧 `src/lainc` archive 编译计划 |
| [`history/lainc-meta-roadmap.md`](history/lainc-meta-roadmap.md) | 2026-09-02 标准库与旧 archive 自举实施记录 |
| [`history/lainir-compiler-roadmap.md`](history/lainir-compiler-roadmap.md) | 已完成的 LAINIR 编译器自举路线 |
| [`history/lainir-implementation-roadmap.md`](history/lainir-implementation-roadmap.md) | LAINIR frontend 和 compiler boundary 的旧实现清单 |
| [`history/lainc-meta-system.md`](history/lainc-meta-system.md) | `src/lainc` 过渡 Meta 实现记录 |
| [`history/lainc-performance-analysis-2026-08-21.md`](history/lainc-performance-analysis-2026-08-21.md) | 2026-08-21 自举性能分析 |
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
