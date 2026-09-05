# Lain-Lang（Scheme 时期设计快照）

> 历史资料：本文保存旧 Scheme Meta 架构下的语言总览，不定义当前实现。当前概览见 [`docs/00-intro.md`](../00-intro.md)。

Language Experiment Lain Lang

## 1. Core Design

我们能否让编译器本体尽量的精简，而让所有的高级语言特性都以库的方式实现？

我们希望它的 meta 功能尽可能强大，这里的 meta 指的是在编译期间对于 AST 的操作变换。

我们希望它的底层中间表示（也就是 Lain-IR）尽可能简洁而忠实反映现代的 CPU。

## 2. Compilation Pipeline

Lain 编译流程为：

```text
Source Text
  -> RawAst
  -> AstTree
  -> Middle AST
  -> Typed / Elaborated Middle AST
  -> LAIN-IR
  -> Backend Target
```

各层职责如下：

```text
RawAst
  C parser 产出的无语义拓扑树。

AstTree
  RawAst 的 Meta 层值表示，仍然只保存拓扑。

Middle AST
  Meta domain parser 产出的高层语义树。

Typed / Elaborated Middle AST
  完成名称解析、类型检查、effect 检查、comptime 特化、layout 计算后的语义树。

LAIN-IR
  物理执行层中间表示。

Backend Target
  C、LLVM IR、解释器或其他目标。
```

## 3. LAIN-AST

LAIN-AST 只描述源码文本的结构拓扑。

它只包含物理节点形状：

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
```

LAIN-AST 不包含语言语义节点：

```text
fn
struct
module
import
effect
type
call
field
param
block
stmt
expr
generic
attribute
macro
pattern
```

例如：

```lain
foo(x)
```

在 LAIN-AST 中只是：

```text
Postfix(Atom("foo"), Group(paren, ...))
```

它可能是 value call、type application、effect application、macro invocation 或 DSL form。具体语义由 Meta 层根据上下文解释。

## 4. Meta System

Meta 系统是 Lain 的语言定义层。

它接收 AstTree，并通过 domain parser 生成 Middle AST：

```text
fn/parse.scm          AstTree -> middle.fn
struct/parse.scm      AstTree -> middle.struct
types/parse.scm       AstTree -> type.*
expr/parse.scm        AstTree -> expr.*
effects/form.scm      AstTree -> effect.*
module/parse.scm      AstTree -> module.*
import/parse.scm      AstTree -> import.*
attrs/parse.scm       AstTree -> attr.*
```

Meta 层负责：

```text
domain parsing
macro expansion
attribute expansion
name resolution
type checking
effect checking
generic / comptime specialization
layout calculation
closure conversion
ABI lowering
LAIN-IR generation
```

初期 Meta 层可以由 Scheme 实现，后续逐步自举为 Lain 自身。

## 5. Middle AST

Middle AST 是 Lain 的高层语义表示。

它可以包含：

```text
middle.fn
middle.struct
middle.module
middle.import
expr.call
expr.if
expr.match
expr.block
type.ref
type.app
effect.set
effect.app
pattern.*
```

Middle AST 允许表达源语言语义，但不能直接进入 Backend。

它必须经过 elaboration，消解：

```text
unresolved name
unresolved type
unresolved overload
unexpanded macro
unchecked generic
unchecked effect
source-only attribute
```

## 6. LAIN-IR

LAIN-IR 是 Lain 的物理执行层中间表示。

它只表达 lower 后的底层计算：

```text
#bits<N>
#float<N>
#vec<N, T>
#addr
#unit
#never

#alloca
#offset
#load
#store
#add
#sub
#smul
#umul
#sdiv
#udiv
#block
#loop
#break
#continue
#if
#switch
#proc
#call
#tail_call
#ret
```

LAIN-IR 不保留高层语言结构：

```text
source-level fn
source-level struct
source-level module
source-level effect
generic
macro
method syntax
overload set
high-level pattern matching
```

高层结构体字段访问必须 lower 为：

```text
base address + byte offset + load/store type
```

例如：

```lain-ir
#set %field_ptr = #offset %obj 8
#set %field = #load #bits<64> %field_ptr none
```

## 7. Function Model

Lain 中的 callable 是由 Meta 层 `std::func` 构造器产生的高层对象。

LAIN-IR 中的 `#proc` 是物理 subroutine。

二者不等价：

```text
std::func callable != #proc
```

一个高层函数经过 Meta 展开后，可能 lower 为：

```text
一个 #proc
多个 specialized #proc
closure object + invoke #proc
foreign wrapper
trampoline
comptime-only function
inline 后不存在的代码片段
```

## 8. Generic and Comptime Model

Lain 的泛型不是 Parser 级特性。

泛型被建模为 comptime value parameter。

推荐形式：

```lain
let identity = std::func(comptime T: type, x: T) -> T {
    x
}

let y = identity(i32, 10);
```

类型是一等 comptime value。

因此 type application、effect application 和 value call 可以共享同一种拓扑：

```lain
Vec(i32)
Result(i32, Error)
Throws(i32)
foo(x)
```

Lain 不推荐将 `<T>` 作为核心泛型语法，因为 `<` 和 `>` 会迫使 Parser 判断类型上下文，从而破坏 LAIN-AST 的无语义边界。

## 9. Effect System

Effect 是 Meta / Middle AST 层语义。

Parser 不识别 effect。

LAIN-IR 不直接保留高层 effect set。

例如：

```lain
let read = std::func() -> String ! {IO, Throws(Error)} {
    ...
}
```

其中 effect set 由 `effects/form.scm` 解析，经过 effect checking 后，在 lowering 阶段转换为具体物理实现，例如：

```text
错误返回值
continuation passing
handler table
runtime token
state machine
trap / unwind path
```

## 10. Compile-Time Execution

Lain 支持编译期执行。

Meta 层可以执行纯 AST / Middle AST 变换，但不能直接执行任意宿主 I/O。

需要副作用的编译期行为必须 lower 为受约束的 LAIN-IR，并交给 Host evaluator 执行：

```text
Middle AST
  -> LAIN-IR comptime proc
  -> Host evaluator
  -> comptime value / generated AstTree
```

编译期副作用必须受 effect system 或 host capability 限制。

典型 capability 包括：

```text
ReadFile
WriteFile
ReadEnv
RunCommand
Network
PackageFetch
EmitDiagnostic
GenerateAst
```

## 11. Macro and Hygiene

Lain 的宏系统属于 Meta 层。

宏可以作用于：

```text
AstTree
Middle AST
Attribute + target
```

默认规则是：

```text
macro generates AstTree or Middle AST.
lowering generates LAIN-IR.
```

所有 AST 节点携带 `SyntaxContext`。

宏展开生成的新 identifier 必须通过 hygiene context 创建，避免：

```text
宏内部临时变量污染用户作用域。
用户变量意外捕获宏生成 identifier。
宏生成引用绑定到错误定义。
```

## 12. Backend

Backend 只消费 LAIN-IR。

它可以输出：

```text
C
LLVM IR
native code
interpreter bytecode
```

Backend 不重新理解 source-level Lain 语义。

例如高层结构体、泛型、effect、module、macro 都应在进入 Backend 前被消解为 LAIN-IR 中的物理表示。

## 13. Implementation Direction

当前 Lain-Lang 的实现方向包括：

```text
C parser
  负责 RawAst 构建。

Scheme Meta
  负责 canonicalization、domain parsing、macro / meta pass。

LAIN-IR
  负责解释执行、C emitter、未来 LLVM IR emitter。

Bootstrap Layer
  逐步将编译器核心从 Scheme / C 迁移到 Lain 自身。
```

核心迁移目标是：

```text
canonicalize.scm 只输出无语义 AstTree。
surface/tree.scm 只保留通用 tree helper。
type / expr / effect / fn / struct / module parser 分离到各自 domain。
Middle AST 和 elaboration pass 承担语言语义。
LAIN-IR 保持物理、显式、可验证。
```
