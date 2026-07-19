# Architecture

本文档定义 Lain 编译器的整体分层结构，以及各层之间的职责边界。

Lain 的核心原则是：

```text
LAIN-AST 只描述源码拓扑。
Middle AST 描述语言语义。
LAIN-IR 描述物理执行。
```

因此，Parser、Meta、Middle AST、LAIN-IR、Backend 之间必须保持严格分层，不能互相偷跑职责。

## 1. Compilation Pipeline

Lain 编译流程分为以下阶段：

```text
Source Text
  -> RawAst
  -> AstTree
  -> Middle AST
  -> Typed / Elaborated Middle AST
  -> LAIN-IR
  -> Backend Target
```

其中：

```text
Source Text                  原始源码文本
RawAst                       C parser 产出的无语义拓扑树
AstTree                      RawAst 的 Scheme / Meta 层值表示
Middle AST                   Meta domain parser 产出的语义树
Typed / Elaborated Middle AST 完成名称解析、类型检查、effect 检查、comptime 特化后的语义树
LAIN-IR                      物理执行层中间表示
Backend Target               C、LLVM IR、机器码或其他目标
```

## 2. Parser Layer

Parser 只负责把源码文本解析为无语义拓扑结构。

```text
Source Text -> RawAst
```

Parser 可以做：

- tokenization
- bracket grouping
- prefix / postfix / infix / juxt association
- operator precedence
- sibling ordering
- span recording
- initial syntax context assignment

Parser 不允许做：

- 识别 `fn`、`let`、`struct`、`effect`、`module`、`interface`、`import` 等语言语义
- 生成 `call`、`type`、`block`、`param`、`field`、`stmt`、`expr` 等语义节点
- 判断 `(...)` 是调用参数、类型参数、分组表达式还是 attribute 参数
- 判断 `{...}` 是函数体、block、struct body、effect set 还是 match body
- 判断 `<...>` 是比较表达式、泛型参数还是 DSL 片段
- 做 name resolution
- 做 type checking
- 做 effect checking
- 做 macro expansion

Parser 的唯一问题是：

```text
源码 token 如何组合成嵌套结构？
```

Parser 不回答：

```text
这是不是函数？
这是不是类型？
这是不是函数调用？
这是不是泛型？
这个 identifier 绑定到哪里？
```

这些问题属于 Meta 层。

## 3. RawAst and AstTree

`RawAst` 是 C parser 产出的 arena 节点树。节点通过 `NodeId` 引用。

`AstTree` 是 `RawAst` 的 Meta 层值表示。它仍然是无语义拓扑树，只是更适合 Scheme / Meta pass 操作。

```text
RawAst -> AstTree
```

`RawAst` 和 `AstTree` 都只能包含物理语法形状，例如：

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
```

它们不能包含语言语义节点，例如：

```text
fn
struct
effect
interface
module
import
call
type
type-app
param
field
block
stmt
expr
generic
```

例如源码：

```lain
let main = std::func() -> i32 {
    return 0;
}
```

在 LAIN-AST 层不能被表示为“函数声明”。

它只能被表示为由以下拓扑元素组成的树：

```text
Atom("main")
Atom("=")
Atom("std")
Atom("::")
Atom("func")
Group(paren, ...)
Infix("->", ...)
Group(brace, ...)
```

只有 Meta 层先识别统一的 `let NAME [: EXPECTED] = INITIALIZER`
绑定，再由 `std::func` 构造器把 initializer 解释为 `middle.fn`。

## 4. Meta Frontend

Meta 层接收 `AstTree`，并通过 domain parser 把无语义拓扑转换为语言语义。

```text
AstTree -> Middle AST
```

不同语言 form 由不同 domain parser 负责：

```text
fn/parse.scm          AstTree -> middle.fn
struct/parse.scm      AstTree -> middle.struct
types/parse.scm       AstTree -> type.*
expr/parse.scm        AstTree -> expr.*
control/parse.scm     AstTree -> control.*
effects/form.scm      AstTree -> effect.*
interface/parse.scm   AstTree -> interface.*
module/parse.scm      AstTree -> module.*
import/parse.scm      AstTree -> import.*
attrs/parse.scm       AstTree -> attr.*
```

Meta 层可以做：

- domain parsing
- macro expansion
- attribute expansion
- name resolution
- type checking
- effect checking
- generic / comptime specialization
- layout calculation
- closure conversion
- control form lowering
- lowering to LAIN-IR

Meta 层不应该把 domain-specific helper 放回 AstTree 层。

AstTree helper 只能提供通用树操作，例如：

```text
tree.kind
tree.span
tree.syntax-context
tree.children
tree.atom-text
tree.group-delim
tree.flatten-juxt
tree.split-by-sep
tree.left
tree.right
```

AstTree helper 不应该提供：

```text
tree-parse-type
tree-parse-block
tree-lower-expr
tree-parse-effect-set
tree-parse-params
```

这些函数必须属于对应 domain parser。

## 5. Middle AST

`Middle AST` 是 Meta domain parser 产出的语义树。

它可以包含 Lain 高层语义，例如：

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
```

`Middle AST` 已经不再是纯拓扑树。它表达的是 Lain 语言语义。

例如：

```lain
let add = std::func(x: i32, y: i32) -> i32 {
    x + y
}
```

经过绑定解析与 `std::func` 构造器 elaboration 后，可以变成：

```text
middle.fn
  name: add
  params:
    x: type.ref(i32)
    y: type.ref(i32)
  return: type.ref(i32)
  body:
    expr.binary("+", expr.ref(x), expr.ref(y))
```

这个阶段允许存在高层语言结构。

但 Middle AST 还不一定完成：

- name resolution
- type checking
- effect checking
- generic specialization
- ABI lowering
- memory layout calculation

这些属于后续 elaboration 阶段。

## 6. Typed / Elaborated Middle AST

`Typed / Elaborated Middle AST` 是完成语义检查和展开后的 Middle AST。

```text
Middle AST -> Typed / Elaborated Middle AST
```

这个阶段负责：

- name resolution
- overload resolution
- type inference
- type checking
- effect checking
- comptime evaluation
- generic specialization
- trait / interface resolution
- layout calculation
- ABI decision
- closure conversion
- high-level control form desugaring

它仍然可以保留语义信息，例如：

```text
typed expr
resolved symbol
resolved function
resolved type
resolved effect
layout reference
ABI annotation
```

但这些信息不能进入 LAIN-AST 层。

是否引入独立的 `Core AST` 名称，需要谨慎。

如果后续引入 `Core AST`，它应该等价于：

```text
Core AST = Typed / Elaborated Middle AST
```

也就是：

```text
Middle AST -> Core AST -> LAIN-IR
```

其中：

```text
Middle AST 表示已解析的语言语义。
Core AST 表示已解析、已检查、已特化的语言语义。
LAIN-IR 表示物理执行。
```

在没有明确需要之前，优先使用 `Typed / Elaborated Middle AST`，避免引入模糊的新层。

## 7. LAIN-IR

`LAIN-IR` 是物理执行层中间表示。

```text
Typed / Elaborated Middle AST -> LAIN-IR
```

LAIN-IR 负责表达：

- primitive storage type
- address
- load / store
- arithmetic primitive
- branch
- block
- loop
- physical subroutine
- call
- tail call
- compile-time evaluation target
- low-level concurrency primitive

LAIN-IR 不负责表达：

- source-level `fn`
- source-level `struct`
- source-level `module`
- source-level `interface`
- source-level `effect`
- generic
- macro
- method syntax
- overload set
- unresolved name
- high-level type expression
- high-level pattern matching

这些必须在 Meta / Middle AST / Elaboration 阶段处理完毕。

## 8. High-Level Function vs Physical Procedure

Lain 源码中的 callable 是由 Meta 层 `std::func` 构造器产生的高层对象。

LAIN-IR 中的 `#proc` 是物理 subroutine。

二者不是同一个概念。

```text
std::func callable != #proc
```

一个由 `std::func` 构造并绑定的高层函数经过 Meta 展开后，可能 lower 为：

- 一个 `#proc`
- 多个 specialized `#proc`
- 一个 closure object 加一个 invoke `#proc`
- 一个 comptime-only function
- 一个 inline 后不再单独存在的代码片段
- 一个 foreign binding declaration
- 一个 wrapper / trampoline / adapter procedure

例如：

```lain
let identity = std::func(comptime T: type, x: T) -> T {
    x
}
```

这不是一个固定的 LAIN-IR `#proc`。

它可能在实例化后生成：

```text
#proc identity_i32(...)
#proc identity_f64(...)
#proc identity_ptr(...)
```

也可能被完全 inline，不产生任何 `#proc`。

因此，`std::func` 构造的 callable 是语言语义；`#proc` 是物理实现。

## 9. Type, Struct, Module, Effect

Lain 的高层概念由 Meta 层定义，不属于 Parser，也不属于 LAIN-AST。

统一绑定右侧可以调用这些 Meta 构造器：

```text
std::func
std::struct
std::module
import("path")
```

Parser 只看到 token 和拓扑。

例如：

```lain
let Vec: type = std::struct(comptime T: type) {
    ptr: Ptr(T),
    len: usize,
    cap: usize,
}
```

Parser 不知道这是结构体。

AstTree 只描述：

```text
Postfix(Atom("Vec"), Group(paren, ...))
Atom(":")
Atom("type")
Atom("=")
Atom("std")
Atom("::")
Atom("struct")
Group(brace, ...)
```

只有统一绑定解析器和 `std::struct` 构造器可以把它解释成
`middle.struct`。

之后，layout pass 决定字段偏移、对齐、大小，并最终 lower 到 LAIN-IR 的地址、offset、load、store。

LAIN-IR 不应该直接拥有高层 `struct` 类型。

## 10. Generic and Comptime Model

Lain 的泛型不应该是 Parser 级特性。

泛型是 comptime value parameter 的一种用法。

推荐语法：

```lain
let identity = std::func(comptime T: type, x: T) -> T {
    x
}

let y = identity(i32, 10);
```

类型是一等 comptime value。

因此，类型应用、effect 应用和值调用可以共享相同的拓扑形状：

```lain
Vec(i32)
Result(i32, Error)
Throws(i32)
Map(String, User)
```

在 LAIN-AST 中，它们都只是：

```text
Postfix(Name, Group(paren, ...))
```

具体含义由上下文和 domain parser 决定：

```text
types/parse.scm       可以解释为 type application
effects/form.scm      可以解释为 effect application
expr/parse.scm        可以解释为 value call
```

不推荐把泛型设计成 Parser 必须理解的形式：

```lain
Vec<i32>
Result<i32, Error>
foo::<i32>(x)
```

因为 `<...>` 会强迫 Parser 判断类型上下文，从而污染 LAIN-AST 边界。

## 11. Compile-Time Evaluation

Lain 支持编译期执行，但编译期执行必须有明确边界。

Meta 层可以做纯 AST / Middle AST 变换，但不应该直接执行任意 I/O。

需要编译期副作用时，必须 lower 为受约束的 LAIN-IR，并交给 Host evaluator 执行。

```text
Middle AST
  -> LAIN-IR comptime proc
  -> Host evaluator
  -> comptime value / generated AstTree
```

编译期执行可以返回：

- primitive value
- string
- bytes
- type value
- AstTree value
- diagnostic

编译期副作用必须受 effect 系统或 host capability 限制。

这保证：

```text
Meta expansion 是可控的。
Comptime execution 是显式的。
Host side effect 不会偷偷混入 AST transform。
```

## 12. Backend

Backend 接收 LAIN-IR，并输出目标代码。

```text
LAIN-IR -> Backend Target
```

Backend 可以包括：

- C emitter
- LLVM IR emitter
- native code emitter
- interpreter
- verifier
- debug printer

Backend 不应该重新理解 Lain 高层语义。

也就是说，Backend 不应该处理：

```text
fn
struct
module
generic
effect
interface
macro
```

Backend 只处理已经 lower 完成的 LAIN-IR。

## 13. Boundary Summary

各层职责总结：

```text
Source Text
  原始文本。

RawAst
  C parser 产出的无语义拓扑树。

AstTree
  RawAst 的 Meta 层值表示，仍然无语义。

Middle AST
  Meta domain parser 产出的语言语义树。

Typed / Elaborated Middle AST
  完成名称解析、类型检查、effect 检查、comptime 特化和 layout 后的语义树。

LAIN-IR
  物理执行层中间表示。

Backend Target
  C、LLVM IR、机器码或其他目标。
```

最重要的边界是：

```text
LAIN-AST 不知道语言语义。
Middle AST 不负责物理执行细节。
LAIN-IR 不保留高层语言语义。
Backend 不重新解释源码语言。
```
