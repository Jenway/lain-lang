# Meta System Specification

本文档定义 Lain 的 Meta 系统。

本文档定义 Meta 层如何消费 `AstTree`、生成 `Middle AST`、执行宏展开、完成语义检查，并最终 lower 到 `LAIN-IR`。

## 1. Position in Pipeline

Meta 层位于 `AstTree` 之后，`LAIN-IR` 之前。

```text
Source Text
  -> RawAst
  -> AstTree
  -> Middle AST
  -> Typed / Elaborated Middle AST
  -> LAIN-IR
```

其中：

```text
RawAst
  C parser 产出的无语义拓扑树。

AstTree
  RawAst 的 Scheme / Meta 层值表示，仍然无语义。

Middle AST
  Meta domain parser 产出的语言语义树。

Typed / Elaborated Middle AST
  完成名称解析、类型检查、effect 检查、comptime 特化和 layout 后的语义树。

LAIN-IR
  物理执行层中间表示。
```

Meta 层负责：

```text
AstTree -> Middle AST -> Typed / Elaborated Middle AST -> LAIN-IR
```

Parser 不理解 Lain 语言语义。

Backend 不重新理解 Lain 语言语义。

语言语义归 Meta 层所有。

## 2. Core Principle

Meta 系统的核心原则是：

```text
Syntax topology is not language semantics.
```

也就是说：

```text
AstTree 只描述源码 token 如何组合。
Meta 层才决定这些组合是什么意思。
```

例如：

```lain
foo(x)
```

在 AstTree 中只是：

```text
Postfix(Atom("foo"), Group(paren, ...))
```

它可能被解释为：

```text
value call
type application
effect application
macro invocation
attribute argument
DSL-specific form
```

具体含义由 domain parser 和上下文决定。

因此，Meta 层不能假定某种拓扑形状天然等于某种语言语义。

## 3. Responsibilities

Meta 层可以做：

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
control form lowering
ABI lowering
lowering to LAIN-IR
diagnostic generation
```

Meta 层不应该做：

```text
直接修改 C parser 的 RawAst 内存
绕过 SyntaxContext 构造非卫生 AST
把 domain-specific parser 放入 AstTree helper
让 LAIN-IR 重新承载高层语义
直接执行不受约束的宿主 I/O
```

Meta 层是语言定义的主要位置。

高层 Lain 的这些概念都属于 Meta：

```text
fn
let
struct
module
import
effect
interface
impl
comptime
macro
attribute
type
generic
```

它们不是 Parser 内建概念，也不是 LAIN-IR 内建概念。

## 4. Host Boundary

Meta 层运行在宿主编译器提供的环境中。

初期实现中，Meta 层可以使用 Scheme。

后续自举阶段，Meta 层可以逐步迁移到 Lain 自身。

当前自举实现已经包含第一个由 Lain 所有的端到端切片：

```text
target source
  -> RawAst opaque handles
  -> mini_syntax.lain (zero-copy Middle-AST views)
  -> mini_elaborate.lain (scope and i32 semantics)
  -> mini_lower.lain (recursive lowering)
  -> mini_meta.lain (driver)
  -> packages/lain/compiler/l1_text_builder.lain
  -> canonical structured LAIN-IR
  -> host verifier / interpreter
```

M2 切片支持多个具名 `i32` 过程、`i32` 参数、顺序局部 `let`、参数与局部
作用域、规范十进制字面量、递归 `+`、直接函数调用、`==` 和最终位置的结构化
`if/else`。模块函数表负责重复声明、callee 和 arity 检查。目标程序的这些
语义不经过 Scheme domain lowerer；Scheme 只承担编译并启动这段 Lain Meta
实现的 stage-0 职责。

当前作用域环境使用函数体 syntax interval `[body-start, current-form)` 表示。
查找名字时，Lain Meta 遍历此前的 local forms。这使源码顺序、重复绑定和
先声明后使用的规则保持显式，并避免把作用域策略下放给 C host。

当前 Middle AST 采用 zero-copy semantic view：仍以 opaque RawAst node id 作为
存储身份，但只通过 `mini_syntax` 暴露的 `expr-tag`、`form-tag` 和 program view
访问。`mini_elaborate` 与 `mini_lower` 不直接调用 C AST capability。这样阶段
边界已经存在，同时不要求 C host 为高层语义节点提供分配器。

M2 使用明确的 `i32` / `bool` 类型对象和单次 parse 的 CompileResult 查询协议，
可取得成功状态、诊断、procedure count、procedure lookup 和完整 L1 unit 文本。
它仍不是完整 Meta 系统：尚未覆盖用户定义类型、泛型、effect、完整控制流和
跨源模块摘要。后续 domain 必须沿相同所有权边界扩展，而不是在 C parser 或
LAIN-IR 中加入这些高层语义。

### M3 reusable compiler artifact

M3 不再为每次目标编译递归 source-link 整个 Lain Meta 实现。Stage-0 按依赖
顺序分别生成 interface 与 L1，再把实际 procedure 合并、去除已由 artifact
提供的 extern，并保留真正的 host extern，形成可重复装载的：

```text
build/core-self-hosting/m3_meta_compiler.l1
```

运行 compiler artifact 与运行普通生成程序具有不同 capability policy：

```text
generated target L1
  capability-free by default

M3 compiler artifact
  binds an #extern only when it is also in the host's fixed compiler allowlist
```

M3 的固定 allowlist 仅包含 RawAst reader（`ast.parse!`、node topology/text
queries）以及纯字符串/整数文本 helpers。文件、进程、环境、网络和普通生成目标
执行能力不在名单中；artifact 声明未授权 extern 会在执行前得到 diagnostic
3006。构建脚本同时断言最终 artifact 的 extern 集恰好等于该名单，防止依赖静默
漂移。当前 RawAst bridge 是 single-active-session：下一次 `ast.parse!` 替换上一
次 arena，最后一个 session 随 VM 生命周期释放。

artifact 的首个真实输入是 `packages/lain/compiler/mini_type.lain`，随后覆盖
返回 `addr` 和字符串诊断的 `mini_diagnostic.lain`。这要求 Lain Meta 支持真实
`pub fn`、显式 `return`、语句式 `if`、`addr`/字符串类型，以及带参数调用。

### M4 Meta-owned frontend core

M4 把 zero-copy syntax view 之上的顶层解释正式放入
`packages/lain/compiler/mini_middle.lain`。它区分 attribute、import、procedure
declaration 和 procedure definition；callable table 同时包含声明和定义，而
lowering 只生成被选择的定义。

局部作用域不再用单一的 `[body-start, current-form)` 平面区间近似。Lain Meta
从函数 block 根开始查找目标 form，进入嵌套 `if` 时继承此前支配该分支的绑定，
同时拒绝分支之后才出现的声明。由此支持 parent lexical scope、branch-local
shadowing 和顺序可见性。

M4 artifact 验收把以下源文件合成一个语义闭包：

```text
mini_syntax + mini_middle + mini_type + mini_module
  + mini_elaborate + l1_text_builder + mini_lower
  + mini_diagnostic + mini_compile_result + mini_meta
```

整个闭包由 Lain-owned elaborator 验证。`mini_meta_compile_named` 随后选择一个
procedure 生成定义，并把其余已验证 callable 写成 extern contract；测试执行
自编译生成的 `mini_meta_schema_version() -> 4`。这是增量自举接口，不是长期
module linker；跨源 ModuleSummary/linking 仍属于 M5。

CompileResult 查询协议包含成功状态、diagnostic code/message、失败 procedure、
procedure summary 和 L1 unit。失败编译不会生成部分 L1。大型 bootstrap 文本
lowering 使用显式线性 capability `core.string-append-linear!`，其输入临时片段在
调用后失效，避免递归拼接保留平方级内存；结构化 L1Unit 将在后续阶段取代它。

无论宿主语言是什么，Meta 层都不直接操作 C 内存指针。

Meta 通过 Host API 操作 AST、IR、diagnostic 和编译期执行环境。

```text
Meta code
  -> Host API
  -> RawAst arena / IR builder / evaluator / diagnostics
```

Host API 必须保持：

```text
opaque
safe
minimal
hygienic
diagnostic-aware
```

## 5. RawAst and AstTree Access

Meta 层通过 opaque handle 查询 RawAst。

典型 Host API：

```scheme
;; node kind
(lain_ast_kind ctx node-id)

;; atom
(lain_ast_atom_text ctx node-id)

;; group
(lain_ast_group_delim ctx node-id)
(lain_ast_group_children ctx node-id)

;; prefix
(lain_ast_prefix_op ctx node-id)
(lain_ast_prefix_operand ctx node-id)

;; postfix
(lain_ast_postfix_operand ctx node-id)
(lain_ast_postfix_op ctx node-id)

;; infix
(lain_ast_infix_op ctx node-id)
(lain_ast_infix_left ctx node-id)
(lain_ast_infix_right ctx node-id)

;; juxt
(lain_ast_juxt_left ctx node-id)
(lain_ast_juxt_right ctx node-id)

;; sep
(lain_ast_sep_text ctx node-id)

;; metadata
(lain_ast_span ctx node-id)
(lain_ast_syntax_context ctx node-id)
```

`RawAst -> AstTree` 的转换可以由 `canonicalize.scm` 完成。

`AstTree` 是 Scheme 值表示，便于 pattern matching 和 domain parser 操作。

推荐表示：

```scheme
(atom text span ctx)
(group delimiter children span ctx)
(prefix op operand span ctx)
(postfix operand op span ctx)
(infix op left right span ctx)
(juxt left right span ctx)
(sep text span ctx)
```

早期实现可以省略 `span` 和 `ctx`，但 API 和数据结构设计必须保留位置。

## 6. AstTree Helper Boundary

AstTree helper 只能提供通用树操作。

允许的 helper：

```text
tree.kind
tree.span
tree.syntax-context
tree.children
tree.atom-text
tree.group-delim
tree.prefix-op
tree.postfix-op
tree.infix-op
tree.left
tree.right
tree.flatten-juxt
tree.split-by-sep
tree.strip-paren
tree.expect-atom
tree.match-infix
tree.match-prefix
tree.match-postfix-group
```

禁止放入 AstTree helper 的函数：

```text
tree-parse-type
tree-parse-block
tree-parse-params
tree-parse-effect-set
tree-lower-expr
tree-parse-fn
tree-parse-struct
tree-parse-call
tree-parse-module
```

这些函数依赖 Lain 语言语义，必须放入对应 domain parser。

AstTree 层只能知道：

```text
这是 atom。
这是 group。
这是 infix。
这是 postfix。
这是 sep。
```

它不能知道：

```text
这是函数。
这是类型。
这是调用。
这是字段。
这是 block。
这是 effect set。
```

## 7. Domain Parsers

Meta 层通过 domain parser 把 AstTree 转换为 Middle AST。

推荐划分：

```text
fn/parse.scm          AstTree -> middle.fn / middle.foreign-fn
struct/parse.scm      AstTree -> middle.struct
types/parse.scm       AstTree -> type.*
expr/parse.scm        AstTree -> expr.*
control/parse.scm     AstTree -> control.*
effects/form.scm      AstTree -> effect.*
interface/parse.scm   AstTree -> interface.*
module/parse.scm      AstTree -> module.*
import/parse.scm      AstTree -> import.*
attrs/parse.scm       AstTree -> attr.*
pattern/parse.scm     AstTree -> pattern.*
macro/parse.scm       AstTree -> macro.*
```

Domain parser 可以共享通用 helper，但不能把自己的语义规则塞回 AstTree 层。

例如：

```text
types/parse.scm 可以解释 Vec(i32) 是 type application。
expr/parse.scm 可以解释 Vec(i32) 是 value call。
effects/form.scm 可以解释 Throws(i32) 是 effect application。
```

但 `tree.scm` 不应该提供 `tree-parse-call` 或 `tree-parse-type-app`。

## 8. Middle AST

Middle AST 是 Meta domain parser 产出的语义树。

它已经脱离纯拓扑层。

Middle AST 可以包含：

```text
middle.fn
middle.struct
middle.module
middle.import
middle.interface
middle.impl

expr.literal
expr.ref
expr.call
expr.field
expr.block
expr.if
expr.match
expr.loop
expr.return

type.ref
type.app
type.fn
type.ptr
type.array
type.comptime

effect.ref
effect.app
effect.set

pattern.binding
pattern.tuple
pattern.struct
pattern.literal
```

Middle AST 仍然可以包含未解析的语义信息，例如：

```text
unresolved name
unresolved type reference
unresolved overload
unexpanded macro
unresolved trait / interface constraint
unchecked effect annotation
```

这些信息必须在 elaboration 阶段消解，不能进入 LAIN-IR。

## 9. Elaboration

Elaboration 是从普通 Middle AST 到 Typed / Elaborated Middle AST 的过程。

```text
Middle AST -> Typed / Elaborated Middle AST
```

它负责：

```text
name resolution
module resolution
import resolution
attribute expansion
macro expansion
type inference
type checking
effect checking
overload resolution
interface / trait resolution
comptime evaluation
generic specialization
layout calculation
ABI decision
closure conversion
control-flow desugaring
```

Elaboration 之后，程序中不应该继续存在：

```text
unresolved identifier
unresolved type name
unresolved overload set
unexpanded macro
unchecked generic
unchecked effect
source-only attribute
```

但它仍然可以保留高层语义信息，直到 lowering 阶段。

例如：

```text
typed expression
resolved symbol
resolved type
resolved function
resolved effect
layout metadata
ABI metadata
```

## 10. Core AST Naming

当前阶段不强制引入独立的 `Core AST`。

推荐使用：

```text
Typed / Elaborated Middle AST
```

如果后续引入 `Core AST`，必须严格定义为：

```text
Core AST = 已完成名称解析、类型检查、effect 检查、comptime 特化和必要 desugaring 的语义 AST。
```

也就是说：

```text
Middle AST -> Core AST -> LAIN-IR
```

其中：

```text
Middle AST
  已解析的语言语义。

Core AST
  已解析、已检查、已特化的语言语义。

LAIN-IR
  物理执行层。
```

Core AST 仍然可以包含语义结构。

LAIN-IR 不能包含高层语义结构。

因此：

```text
Core AST != LAIN-IR
```

## 11. Function Form

高层 `fn` 是 Meta 层定义的 form。

Parser 不知道 `fn` 是函数。

LAIN-IR 也不知道 source-level `fn`。

源码：

```lain
fn add(x: i32, y: i32) -> i32 {
    x + y
}
```

AstTree 只表达：

```text
Atom("fn")
Atom("add")
Group(paren, ...)
Infix("->", ...)
Group(brace, ...)
```

`fn/parse.scm` 才能把它解释为：

```text
middle.fn
  name: add
  params: ...
  return: ...
  body: ...
```

后续经过 elaboration 后，一个 `middle.fn` 可能 lower 为：

```text
一个 #proc
多个 specialized #proc
一个 closure object + invoke #proc
一个 wrapper / trampoline
一个 comptime-only function
一个 inline 后不存在的代码片段
```

因此：

```text
fn != #proc
```

## 12. Struct Form

高层 `struct` 是 Meta 层定义的 form。

源码：

```lain
struct Pair {
    a: i32,
    b: i32,
}
```

AstTree 不知道这是结构体。

`struct/parse.scm` 将其解释为：

```text
middle.struct
  name: Pair
  fields:
    a: type.ref(i32)
    b: type.ref(i32)
```

layout pass 之后得到：

```text
size
align
field offset
field ABI
```

最终 lower 到 LAIN-IR 时，不应该产生高层 `#struct`。

字段访问应该 lower 成：

```text
#addr + #offset + #load / #store
```

例如：

```lain-ir
#set %b_ptr = #offset %pair_addr 4
#set %b = #load #bits<32> %b_ptr none
```

## 13. Type System in Meta

高层类型系统属于 Meta / Middle AST 层。

LAIN-IR 只保留物理类型：

```text
#bits<N>
#float<N>
#vec<N, T>
#addr
#unit
#never
```

高层类型可以包括：

```text
i32
u32
usize
bool
Ptr(T)
Array(T, N)
Slice(T)
Fn(...)
Struct type
Enum type
Interface type
Comptime type
Effectful function type
```

这些类型必须在 elaboration / lowering 阶段映射到物理表示。

例如：

```text
i32      -> #bits<32>
bool     -> #bits<1>
Ptr(T)   -> #addr
Struct   -> layout metadata + address/offset operations
```

类型表达式由 `types/parse.scm` 负责解析。

类型检查由 elaboration pass 负责。

## 14. Generic and Comptime Model

Lain 的泛型是 comptime value parameter 的一种用法。

泛型不属于 Parser。

泛型不应该要求 Parser 理解 `<...>`。

推荐形式：

```lain
fn identity(comptime T: type, x: T) -> T {
    x
}

let y = identity(i32, 10);
```

类型是一等 comptime value。

因此，下面这些形式在 AstTree 层可以共享同一种拓扑：

```lain
Vec(i32)
Result(i32, Error)
Throws(i32)
foo(x)
```

它们都是：

```text
Postfix(Name, Group(paren, ...))
```

不同 domain parser 根据上下文解释：

```text
types/parse.scm       Vec(i32)       -> type.app
effects/form.scm      Throws(i32)    -> effect.app
expr/parse.scm        foo(x)         -> expr.call
```

`<...>` 泛型形式不推荐作为核心语法：

```lain
Vec<i32>
Result<i32, Error>
foo::<i32>(x)
```

因为它会强迫 Parser 区分比较表达式和类型参数，从而污染 LAIN-AST 边界。

## 15. Effect System

Effect 是 Meta 层语义。

Parser 不知道 effect。

LAIN-IR 不直接保留高层 effect set。

例如：

```lain
fn read() -> String ! {IO, Throws(Error)} {
    ...
}
```

AstTree 只描述 `!`、brace group、name、postfix group 等拓扑。

`effects/form.scm` 负责解析：

```text
effect.set
  IO
  Throws(Error)
```

Elaboration 负责：

```text
effect name resolution
effect checking
effect polymorphism
effect lowering decision
```

Lowering 之后，effect 可能变成：

```text
普通调用约束
错误返回值
continuation passing
handler table
runtime token
state machine
trap / unwind path
```

LAIN-IR 只承载 lower 后的物理控制流和调用。

## 16. Attribute System

Attribute 是 Meta 层 form。

例如：

```lain
@[foreign(link_name = "puts")]
fn puts(s: CStr) -> i32;
```

AstTree 只描述：

```text
Prefix("@", Group(bracket, ...))
```

`attrs/parse.scm` 负责把它解释为 attribute。

Attribute 可以作用于：

```text
module
import
fn
struct
field
param
type
expr
block
```

具体允许位置由对应 domain parser 决定。

Attribute expansion 可以：

```text
修改 Middle AST metadata
生成新的 Middle AST
触发 macro expansion
影响 ABI lowering
影响 linkage
影响 diagnostics
```

Attribute 不应该让 Parser 识别语义。

## 17. Macro System

Macro 是 Meta 层的语义变换机制。

Macro 可以分为：

```text
Ast macro
  AstTree -> AstTree

Middle macro
  Middle AST -> Middle AST

Attribute macro
  Attribute + target -> transformed target
```

推荐边界：

```text
Ast macro
  适合处理语法糖和 surface-level rewriting。

Middle macro
  适合处理已经有明确语义的 form。

Lowering pass
  负责从 semantic form 生成 LAIN-IR。
```

Macro 不应该直接生成未验证的 LAIN-IR，除非它本身就是明确的 IR-level macro，并经过 verifier。

默认规则：

```text
宏生成 AstTree 或 Middle AST。
LAIN-IR 由 lowering pass 生成。
```

## 18. Hygiene

所有 AST 节点都携带 `SyntaxContext`。

源码节点由 Parser 赋予初始 syntax context。

宏展开产生的新节点必须获得新的 hygiene context。

Meta 层构造 AST 时不能直接拼接裸 identifier。

应该通过 Host API 创建带上下文的 identifier：

```scheme
(lain_ast_make_ident ctx "x" syntax-context)
(lain_ast_fresh_ident ctx "tmp" parent-context)
```

Hygiene 的目标是避免：

```text
宏内部临时变量污染用户作用域。
用户变量意外捕获宏生成的 identifier。
宏生成的引用绑定到错误定义。
```

示例：

```lain
macro twice(expr) {
    #{
        {
            let tmp = ,expr;
            tmp + tmp
        }
    }
}
```

这里的 `tmp` 必须是 fresh identifier，而不是普通用户可捕获的 `tmp`。

## 19. Quasiquote

`#{ ... }` 是 Meta 层构造 AstTree 的语法糖。

它内部书写 Lain surface syntax。

但它的结果仍然是 AstTree，不是 Middle AST，也不是 LAIN-IR。

例如：

```scheme
(define (make-assert cond-node msg-node)
  #{
    if (!,(cond-node)) {
      panic(,(msg-node));
    }
  })
```

`#{ ... }` 的结果只表示拓扑。

之后仍然需要 domain parser 把它解释为：

```text
control.if
expr.call
expr.block
```

### 19.1 Escape

Quasiquote 中使用 `,(expr)` 逃逸到 Meta 层。

```scheme
#{
  println(,(message-node));
}
```

`expr` 的结果必须是可插入的 AST value。

### 19.2 Fresh Identifier

Quasiquote 应支持 fresh identifier 插入。

示例形式：

```scheme
(let ((tmp (fresh-ident ctx "tmp")))
  #{
    {
      let ,tmp = compute();
      ,tmp
    }
  })
```

具体语法可以后续确定，但语义必须支持 hygiene。

### 19.3 No Direct Semantics

下面的 quasiquote：

```scheme
#{
  Vec(i32)
}
```

不会直接生成 `type.app`。

它只生成：

```text
Postfix(Atom("Vec"), Group(paren, ...))
```

在类型上下文中，`types/parse.scm` 可以把它解释为 type application。

在表达式上下文中，`expr/parse.scm` 可以把它解释为 value call。

## 20. Name Resolution

Name resolution 属于 elaboration 阶段。

Parser 不解析名字。

AstTree 不绑定名字。

Middle AST 初期可以保留 unresolved name：

```text
expr.ref("x")
type.ref("Vec")
effect.ref("IO")
```

Name resolution 将其转换为 resolved symbol：

```text
expr.ref(symbol-id)
type.ref(type-id)
effect.ref(effect-id)
```

Name resolution 必须考虑：

```text
module scope
import
local binding
parameter binding
type namespace
value namespace
effect namespace
macro namespace
hygiene context
```

是否采用多 namespace 或统一 namespace 是语言设计问题，但必须在 Meta 层解决，不能下放给 Parser 或 LAIN-IR。

## 21. Type Checking

Type checking 属于 elaboration 阶段。

它输入 Middle AST，输出 typed Middle AST。

示例：

```text
expr.binary("+", expr.ref(x), expr.ref(y))
```

经过 type checking 后变成：

```text
typed.expr.binary
  op: integer.add
  type: i32
  lhs: typed.expr.ref(x: i32)
  rhs: typed.expr.ref(y: i32)
```

Lowering 到 LAIN-IR 时，才生成：

```lain-ir
#set %z = #add #bits<32> %x %y
```

因此：

```text
+ 的重载选择属于 type checking。
#add 是 lower 后的物理整数加法。
```

## 22. Layout Calculation

Layout calculation 属于 elaboration / lowering 边界。

它负责确定：

```text
size
align
field offset
enum tag representation
calling convention representation
capture layout
closure layout
```

例如：

```lain
struct Pair {
    a: i32,
    b: i32,
}
```

layout pass 产生：

```text
Pair.size = 8
Pair.align = 4
Pair.a.offset = 0
Pair.b.offset = 4
```

后续 field access lower 成：

```lain-ir
#set %b_ptr = #offset %pair_addr 4
#set %b = #load #bits<32> %b_ptr none
```

LAIN-IR 不应该直接知道 `Pair.b`。

## 23. Lowering to LAIN-IR

Lowering 负责把 Typed / Elaborated Middle AST 转换为 LAIN-IR。

```text
Typed / Elaborated Middle AST -> LAIN-IR
```

Lowering 必须消除：

```text
source fn
source struct
source module
source effect
generic
interface
method syntax
field syntax
pattern matching
high-level control syntax
overload
```

Lowering 输出：

```text
#proc
#extern_proc
#global
#bits
#float
#addr
#alloca
#offset
#load
#store
#add
#call
#block
#loop
#if
#ret
```

Lowering 不应该重新做语法解析。

Lowering 的输入应已经完成语义解析和检查。

## 24. Compile-Time Execution Boundary

Meta 本身不应该直接执行任意宿主 I/O。

需要编译期副作用时，必须 lower 成受约束的 LAIN-IR，并交给 Host evaluator 执行。

```text
Middle AST
  -> LAIN-IR comptime proc
  -> Host evaluator
  -> comptime value / generated AstTree
```

编译期执行可以返回：

```text
primitive value
string
bytes
type value
AstTree value
diagnostic
```

编译期副作用必须受 effect system 或 host capability 限制。

例如，读取文件生成 binding：

```lain
comptime fn generate_bindings() -> AstTree ! {HostRead, HostWrite} {
    ...
}
```

应先被 Meta 解析和检查，再 lower 成 comptime LAIN-IR，由 Host evaluator 执行。

Meta 不能直接绕过 effect system 调用宿主文件 API。

## 25. Host Capabilities

Host capability 用于限制编译期副作用。

可能的 capability：

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

默认 Meta expansion 不拥有危险 capability。

需要副作用的 comptime code 必须显式声明，并由编译器策略允许。

例如：

```text
普通 macro expansion
  不允许文件 I/O。

build script / package fetch
  可以在明确 capability 下访问文件系统或网络。

diagnostic emission
  可以通过受控 API 产生 warning / error。
```

Capability 是 Host boundary 的一部分，不是 Parser 语义，也不是 LAIN-IR 高层语义。

## 26. Diagnostics

Meta 层负责产生高质量 diagnostics。

所有 AstTree、Middle AST 和 Typed Middle AST 节点都应该保留 source span。

Diagnostic 至少包含：

```text
severity
message
primary span
secondary spans
notes
help
origin
```

示例：

```text
error: expected type expression
  --> main.lain:3:12
   |
 3 | let x: = 1;
   |        ^ expected type here
```

Meta diagnostic 应尽量基于 domain parser 语境，而不是只输出通用 parse error。

例如：

```text
fn/parse.scm
  可以输出 “expected function parameter list”。

types/parse.scm
  可以输出 “expected type argument”。

effects/form.scm
  可以输出 “expected effect set after !”。
```

Parser 只负责低层拓扑错误，例如括号不匹配、非法 token。

## 27. Expansion Order

Meta pipeline 的推荐顺序：

```text
1. RawAst -> AstTree canonicalization
2. module / import surface parsing
3. attribute collection
4. top-level domain parsing
5. macro registration
6. macro expansion
7. name resolution
8. type checking
9. effect checking
10. comptime evaluation / specialization
11. layout calculation
12. ABI lowering
13. control / closure lowering
14. LAIN-IR generation
15. LAIN-IR verification
```

具体实现可以分阶段演进，但边界不能反向污染。

尤其禁止：

```text
Parser 为了方便 type checking 而识别 type application。
AstTree helper 为了方便 lowering 而提供 expr parser。
Backend 为了方便 codegen 而重新解析 source-level struct。
```

## 28. Module and Import

Module 是 Meta 层语义。

Parser 不知道 module。

源码：

```lain
module math;

import std.io;

fn add(x: i32, y: i32) -> i32 {
    x + y
}
```

AstTree 只看到 atom、juxt、sep、group 等拓扑。

`module/parse.scm` 和 `import/parse.scm` 负责解析 module form 和 import form。

Module elaboration 负责：

```text
module name resolution
import path resolution
visibility
symbol table construction
linkage name generation
cycle checking
incremental compilation dependency
```

LAIN-IR 中不应保留高层 module form。

Module 信息最终体现为：

```text
symbol namespace
mangled name
linkage metadata
global / proc visibility
```

## 29. Interface and Impl

Interface / trait-like 机制属于 Meta / Middle AST 层。

Parser 不知道 interface。

Middle AST 可以表达：

```text
middle.interface
middle.impl
constraint
associated type
method requirement
```

Elaboration 负责：

```text
constraint solving
method resolution
dictionary generation
monomorphization
witness table generation
```

Lowering 后可以变成：

```text
static call
dictionary parameter
witness table
function pointer table
specialized #proc
```

LAIN-IR 不应该直接承载高层 interface 语义。

## 30. Pattern Matching

Pattern 是 Meta / Middle AST 层语义。

Parser 不知道 pattern。

例如：

```lain
match x {
    Some(v) => v,
    None => 0,
}
```

AstTree 只表达 token 拓扑。

`pattern/parse.scm` 和 `control/parse.scm` 负责解释 pattern 和 match form。

Elaboration 负责：

```text
pattern type checking
exhaustiveness checking
binding introduction
desugaring
```

Lowering 后生成：

```text
tag load
comparison
branch
field offset
load
```

LAIN-IR 不应该直接拥有 high-level `match`。

## 31. Current Implementation Notes

当前实现中，部分文件可能还没有完全符合本合同。

已知迁移方向：

```text
std/meta/canonicalize.scm
  应只负责 RawAst -> AstTree，不生成语义节点。

std/meta/surface/tree.scm
  应只保留通用 tree helper。
  其中 type、expr、block、effect 解析应逐步迁出。

types/parse.scm
  应负责 type expression 和 type application 解析。

expr/parse.scm
  应负责 value expression 和 call 解析。

effects/form.scm
  应负责 effect set 和 effect application 解析。

control/parse.scm
  应负责 block、if、loop、match、return 等控制 form。

fn/parse.scm
  应负责 source-level fn form。

struct/parse.scm
  应负责 source-level struct form。
```

特别注意：

```text
canonicalize.scm 不应该生成 (call callee args)。
```

括号后缀应保持为：

```text
(postfix callee (group paren args ...))
```

是否是 call，由 `expr/parse.scm` 决定。

是否是 type application，由 `types/parse.scm` 决定。

是否是 effect application，由 `effects/form.scm` 决定。

## 32. Migration Plan

建议按以下顺序迁移：

```text
1. 确认 LAIN-AST 合同：Atom / Group / Prefix / Postfix / Infix / Juxt / Sep。
2. 收紧 canonicalize.scm：只输出拓扑节点。
3. 从 surface/tree.scm 移除语义 parser。
4. 新增或完善 types/parse.scm。
5. 新增或完善 expr/parse.scm。
6. 新增或完善 effects/form.scm。
7. 新增或完善 control/parse.scm。
8. 新增或完善 fn/parse.scm。
9. 新增或完善 struct/parse.scm。
10. 建立 Middle AST 数据结构。
11. 建立 elaboration pass。
12. 建立 layout pass。
13. 建立 LAIN-IR lowering pass。
14. 建立 IR verifier。
15. 删除 legacy (call callee args) 兼容路径。
16. 决定 `<...>` 是兼容语法、实验语法，还是移除。
```

迁移时不需要一次性重写整个编译器。

优先级最高的是：

```text
不要让 LAIN-AST 层继续产生语义节点。
不要让 AstTree helper 继续包含 domain parser。
不要让 LAIN-IR 承载 source-level 高层语义。
```

## 33. Boundary Summary

Meta 系统边界总结：

```text
Parser
  只产出无语义拓扑。

AstTree
  只保存无语义拓扑的 Meta 层表示。

Domain Parser
  把拓扑解释为语言 form。

Middle AST
  保存 Lain 高层语言语义。

Elaboration
  完成名称解析、类型检查、effect 检查、comptime 特化、layout、ABI 等。

Lowering
  把已检查语义转换成物理 LAIN-IR。

LAIN-IR
  只表达物理执行。

Backend
  只做目标代码生成。
```

核心规则：

```text
LAIN-AST owns topology.
Meta owns language semantics.
LAIN-IR owns physical execution.
Backend owns target emission.
```
