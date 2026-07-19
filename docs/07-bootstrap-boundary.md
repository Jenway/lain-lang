# Bootstrap Boundary

当前分支布局从 `v0.1.0-alpha.1` 起已经落实为：

```text
main                 Lain-owned compiler development
bootstrap/stage0     C/Scheme cold-start implementation
```

Scheme frontend 只存在于 `bootstrap/stage0`。主线通过相邻的
`lain-bootstrap` worktree 运行显式 bootstrap/兼容性检查；普通编译使用
自举 artifact。下面的 Old World/New World 列表描述所有权边界，不再表示
这些文件同时存在于 `main`。

本文档定义 Lain 的旧世界和自举新世界之间的版本分界。

这个分界不是发布包装。它是工程停工线：旧世界只负责制造第一版可用的 Lain 自举工具链，不能继续无限扩张。

## 1. Names

```text
Old World       C runtime + Scheme meta compiler
Bootstrap Core  旧世界必须支持的最小 Lain 子集
New World       Lain 写的 meta/compiler code
```

旧世界包括：

```text
src/compiler/
src/lainast/
src/lainir/
std/meta/*.scm
```

新世界从这里开始：

```text
std/bootstrap/*.lain
std/meta-lain/*.lain
```

这些目录现在可以不存在。它们表示自举后的目标归属。

## 2. Version Line

```text
0.1.0-alpha.1  第一个 stage2 == stage3 的自举固定点；Scheme 分支隔离
0.1.0          bootstrap branch 与主线 seed 协议冻结
0.2.0          持久化模块产物和增量依赖图
1.0.0          语言、LAIN-IR 和模块 artifact 兼容性承诺
```

`0.1.0` 的定义：

```text
旧世界可以稳定编译 Bootstrap Core 写成的编译器切片。
```

它不表示 Lain 语言完成。

## 3. Old World Duties

旧世界必须保留这些能力：

```text
RawAst topology parser
AstTree canonicalization
minimal meta pipeline
LAIN-IR data model
LAIN-IR interpreter for comptime
C emitter or runnable backend
file/process host capabilities
bootstrap diagnostics
```

旧世界可以实现语言特性，但只在 Bootstrap Core 需要时实现。

## 4. Old World Freeze Rule

到 `0.1.0` 之后，旧世界不得新增普通语言高级特性。

允许继续改：

```text
bootstrap bug fix
host capability bridge
IR execution correctness
diagnostic plumbing
boundary cleanup
Lain-meta migration support
```

禁止继续扩张：

```text
full effect system
full interface/typeclass system
full generic system
macro system
advanced build/package manager
optimizer
language feature experiments
```

这些能力应在新世界里继续发展。

## 5. C Boundary

C 的长期职责：

```text
memory arena
file IO
process/env capability
RawAst physical parser
LAIN-IR storage
LAIN-IR parser/text emitter
LAIN-IR interpreter
C backend
VM/FFI bridge
```

C 不应拥有：

```text
source-level import policy
stdlib type aliases
implicit extern declarations
fn/struct/effect/interface semantics
generic syntax policy
```

旧世界现有违反项要么迁出，要么标成 bootstrap debt，并由 boundary test 追踪。

## 6. Scheme Boundary

Scheme 的过渡职责：

```text
load old meta compiler
compile Bootstrap Core
bridge old meta data to LAIN-IR
host migration shims
```

`0.1.0` 之后，新 compiler pass 不应默认写成 Scheme。

Scheme 文件可以继续存在，但它们的目标是退场，不是成为长期扩展面。

## 7. New World Entry Criteria

第一批 Lain-meta 代码必须能完成这个闭环：

```text
Lain source for a meta pass
  -> old world compiler
  -> executable/comptime artifact
  -> consumes AstTree or Middle AST
  -> emits Middle AST or LAIN-IR
```

第一批迁移目标应该小而硬：

```text
AstTree schema
generic tree traversal
diagnostic builder
type parser slice
expr atom/call parser slice
simple std::func initializer elaborator
```

暂时不要迁：

```text
effect lowering
multi-module build
full callable lowering
interface dispatch
```

这些模块耦合更高，适合等第一批闭环稳定后处理。

## 8. Completion Tests

`0.1.0` 至少需要这些测试通过：

```text
tests/core/
tests/bootstrap-core/
```

`tests/core/` 守编译器层边界。

`tests/bootstrap-core/` 守旧世界是否能支撑自举。

旧 `tests/fixtures/` 可以继续跑，但不能作为 `0.1.0` 的唯一质量指标。

## 9. Release Gate

`0.1.0` release gate：

```text
1. LAIN-AST topology contract is enforced.
2. Bootstrap Core spec is frozen.
3. Bootstrap Core tests exist and fail/pass for explicit reasons.
4. Old-world-only feature expansion is frozen.
5. At least one Lain-written compiler slice can be compiled or has a precise pending test.
```

如果某个功能不服务自举，就不阻塞 `0.1.0`。
