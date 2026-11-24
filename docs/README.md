# Lain-Lang 架构总览与规范索引

> [!QUOTE]
> 我们能否让编译器本体尽量的精简，让所有的高级语言特性都以库的方式实现？
>
> 我们希望它的 Meta 功能尽可能强大，这里的 Meta 指的是在编译期间对于 AST 的操作变换。
>
> 我们希望它的底层中间表示（也就是 LAINIR）尽可能简洁而忠实反映现代的 CPU。
>
> 我们希望它的编译期运行在一种虚拟机环境（Lain-VM）里，安全地给予最多的编译期能力。

---

## 文档目录

### 1. 内核、虚拟机与元编程底座（`docs/01~04`）

| 文档 | 内容 |
| :--- | :--- |
| [01-lain-ir.md](01-lain-ir.md) | **LAINIR 规范**：物理类型（`#bits<N>`、`#float<N>`、`#vec<N>`、`#addr`）、SSA 算子与无 UB 访存契约 |
| [02-lain-vm.md](02-lain-vm.md) | **Lain-VM 规范**：确定性隔离沙箱模型（`Image`、`VSpace`、`TCB`、`CSpace`、`Endpoint` 与 `Trap`） |
| [03-lain-meta.md](03-lain-meta.md) | **Lain-Meta 规范**：零关键字 `Reader`、`RawAst`、`AstOut` 游标事务、`eval/apply` 与 `Expand -> Elaborate -> Lower` 三阶段管线 |
| [04-compilation-and-build.md](04-compilation-and-build.md) | **编译单元与构建系统规范**：`.l1b` 编译单元、双哈希增量编译、内存链接 / 原生 AOT 链接与受限 `build.lain` 构建沙箱 |

### 2. 源语言与标准库元机制（`docs/stdlib/`）

| 文档 | 内容 |
| :--- | :--- |
| [stdlib/modules.md](stdlib/modules.md) | **一等模块系统规范**：`module { ... }`、`pub let` 可见性、`sig { ... }` 签名核验、函子、`use`/`open` 与 `pub let Self` 三位一体机制 |
| [stdlib/concepts.md](stdlib/concepts.md) | **泛型与 Concept 规范**：三区函数签名 `func(...) ?{...} !{...} -> Ret`、一等 `Type` 与类型工厂、基于 `module` 的 Witness 合成与覆盖 |
| [stdlib/effects.md](stdlib/effects.md) | **单次代数效应规范**：`!{ ... }` 效应集、`perform` / `!` 传播、`handle ... with` 单次续体契约、效应行多态与底层降解 |
| [stdlib/desugaring.md](stdlib/desugaring.md) | **标准语法脱糖协议**：基于 `AstOut` 展开阶段的 UFCS 方法重写、`f"..."` 零分配惰性格式化与 `match` 决策树编译 |

### 3. 自举构建、标准库与工具链现状
- **单文件冷启动与纯 Lain 构建**：仓库通过根目录 [build.lain](../build.lain)（默认快速模式 + `--bootstrap` 全量 Stage-1/2/3 不动点）与 [test.lain](../test.lain)（11 套件 / 173 项断言）驱动自举和全量回归，仅依赖单文件 `bootstrap/lainc_boot.c` 与 C99 编译器。
- **编译器能力与工具链**：支持 `comptime if` + `--target <windows|linux|macos>` 跨平台条件编译、`--opt` LAINIR 优化（常量/代数/分支事实/`#load` CSE/Store-to-Load 转发/DCE）、`--split-c <prefix>` 多文件并行 C99 编译、`--trace` Perfetto JSON、`path:line:col: error:` 源码 `^` 定位诊断、跨过程尾可恢复效应（`!= Never`），以及纯 Lain 格式化工具 [tools/lainfmt.lain](../tools/lainfmt.lain)。
- **标准库（`std/`，共 18 个模块）**：涵盖 `option`、`mem`、`collections`、`fmt`、`io`、`os`（`@extern` C ABI + `Option`/`IoErr`）、`fs`、`json`（`JsonKind` 枚举 + `match` + Arena DOM + `comptime for`）、`sync`（显式内存序原子原语与 `SpinLock`）、`thread`（Windows + POSIX）、`pool`、`sched`（`Scheduler` `sig` + `!{ Spawn }`）、`coro`（`Yield`/`Async`/`Cancel`、`Gen(T)`、`Chan(T)`、`run_fiber_local`，Windows + POSIX）、`meta`（`AstOut` / `AstView`）、`build`、`test`、`bench`、`trace`（Perfetto JSON）。
