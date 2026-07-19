# LAIN-AST Contract

本文档定义 LAIN-AST 的职责边界。

LAIN-AST 只描述源码文本的结构拓扑。它不描述 Lain 语言语义。

核心原则：

```text
LAIN-AST owns topology.
Meta owns language semantics.
LAIN-IR owns physical execution.
```

## 1. Layer Names

Lain 前端分为以下阶段：

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
Source Text
  原始源码文本。

RawAst
  C parser 产出的 arena 节点树。
  节点用 NodeId 引用。
  RawAst 只保存无语义拓扑结构。

AstTree
  RawAst 的 Scheme / Meta 层值表示。
  它仍然是无语义拓扑树，只是更方便 meta pass 操作。

Middle AST
  Meta domain parser 产出的语义树。
  例如 middle.fn、middle.struct、expr.call、type.ref、effect.set。

Typed / Elaborated Middle AST
  完成名称解析、类型检查、effect 检查、comptime 特化、layout 计算后的语义树。

LAIN-IR
  物理执行层。
  它承载 lower 后的类型、内存、控制流、调用和 comptime evaluation。
```

LAIN-AST 包含：

```text
RawAst
AstTree
```

LAIN-AST 不包含：

```text
Middle AST
Typed / Elaborated Middle AST
LAIN-IR
```

## 2. Core Rule

LAIN-AST 只能回答一个问题：

```text
源码 token 如何组合成嵌套结构？
```

它不能回答这些问题：

```text
这是不是函数？
这是不是变量绑定？
这是不是类型？
这是不是函数调用？
这是不是泛型参数？
这是不是 effect set？
这是不是模块声明？
这是不是 import？
这个 identifier 绑定到哪里？
这个表达式的类型是什么？
```

这些问题属于 Meta 层。

也就是说：

```text
Atom("std") 不表示标准构造器命名空间。
Infix("::", Atom("std"), Atom("func")) 不表示 callable construction。
Postfix(foo, Group(paren, ...)) 不表示函数调用。
Infix(":", x, i32) 不表示 typed binding。
Group(brace, ...) 不表示 block。
```

这些拓扑形状只有经过 domain parser 解释后，才获得语言语义。

## 3. Parser Responsibilities

C parser 允许做：

```text
tokenization
bracket grouping
prefix association
postfix association
infix association
juxt association
operator precedence
sibling ordering
span recording
initial syntax context assignment
```

C parser 不允许做：

```text
识别 fn、let、struct、effect、interface、module、import 等 keyword 的语义
生成 call、type、type-app、block、param、field、stmt、expr 等语义节点
判定 <...> 是比较表达式、泛型参数还是别的 DSL 结构
判定 (...) 是调用参数、分组表达式、类型参数列表还是 attribute 参数
判定 {...} 是函数体、struct body、effect set、match arms 还是普通 block
做 name resolution
做 type checking
做 effect checking
做 macro expansion
做 layout calculation
```

Parser 可以知道 operator precedence，但 precedence 是物理语法规则，不是语言语义。

例如：

```lain
a + b * c
```

Parser 可以根据优先级产生：

```scheme
(+ a (* b c))
```

但它不能判断：

```text
+ 是整数加法还是浮点加法。
* 是乘法、解引用、DSL operator 还是 overloaded operator。
a、b、c 分别绑定到哪里。
表达式类型是什么。
```

这些属于 Meta / Elaboration 阶段。

## 4. RawAst Node Kinds

`RawAst` 只有物理节点形状。

规范节点形状为：

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
```

短期实现可以兼容旧表示，例如：

```text
把 Juxt 表示为空白 Infix
把 Sep 表示为普通 Atom
```

但规范目标上，`Juxt` 和 `Sep` 应成为独立节点形状。

否则后续 block、param list、argument list、effect set、import list 都会被迫写大量 ad-hoc splitter。

## 5. Atom

`Atom` 表示不可再拆分的 token。

字段：

```text
text
span
syntax_context
```

例子：

```text
std
let
main
i32
42
"hello"
+
::
```

`Atom("std")` 不表示标准库或 Meta 构造器命名空间。

`Atom("i32")` 不表示类型。

`Atom("+")` 不表示加法语义。

它们只是源码中的文本原子。

示例：

```text
std
```

RawAst：

```text
Atom("std")
```

AstTree：

```scheme
(atom "std" span ctx)
```

## 6. Group

`Group` 表示由成对 delimiter 包住的子节点序列。

字段：

```text
delimiter
children
span
syntax_context
```

delimiter 取值：

```text
paren
brace
bracket
```

例子：

```lain
(a, b)
{ x; y; }
[T; 4]
```

对应拓扑：

```text
Group(paren, ...)
Group(brace, ...)
Group(bracket, ...)
```

`Group(paren, ...)` 不表示调用参数。

`Group(brace, ...)` 不表示函数体或 block。

`Group(bracket, ...)` 不表示数组类型或 attribute 参数。

这些含义由 Meta domain parser 决定。

例如：

```lain
foo(a, b)
```

不应直接解析为：

```scheme
(call foo (a b))
```

而应保持为：

```scheme
(postfix
  (atom "foo")
  (group paren
    ((atom "a")
     (sep ",")
     (atom "b"))))
```

之后由 `expr/parse.scm` 判断它是不是 value call。

## 7. Prefix

`Prefix` 表示前缀 operator 和一个 operand。

字段：

```text
op
operand
span
syntax_context
```

例子：

```lain
!cond
&x
*ptr
@foreign
#foo
```

对应拓扑：

```text
Prefix("!", cond)
Prefix("&", x)
Prefix("*", ptr)
Prefix("@", foreign)
Prefix("#", foo)
```

`Prefix("@", x)` 不表示 attribute。

`Prefix("*", x)` 不表示 pointer dereference。

`Prefix("&", x)` 不表示 address-of。

它们只是前缀关联。

具体语义由对应 domain parser 和 type checking 决定。

## 8. Postfix

`Postfix` 表示一个 operand 后接一个 postfix operator。

字段：

```text
operand
op
span
syntax_context
```

`op` 可以是：

```text
atom operator
delimiter group
```

例子：

```lain
x?
foo(...)
arr[0]
T*
```

对应拓扑：

```text
Postfix(x, "?")
Postfix(foo, Group(paren, ...))
Postfix(arr, Group(bracket, ...))
Postfix(T, "*")
```

`Postfix(foo, Group(paren, ...))` 不表示 call。

它可能表示：

```text
value call
type application
effect application
macro invocation
attribute argument
DSL-specific form
```

例如：

```lain
Vec(i32)
```

AstTree：

```scheme
(postfix
  (atom "Vec")
  (group paren
    ((atom "i32"))))
```

在类型上下文中，`types/parse.scm` 可以把它解释为 type application。

在表达式上下文中，`expr/parse.scm` 可以把它解释为 value call。

在 effect 上下文中，`effects/form.scm` 可以把它解释为 effect application。

## 9. Infix

`Infix` 表示中缀 operator 和左右子树。

字段：

```text
op
left
right
span
syntax_context
```

例子：

```lain
a + b
x: i32
T::method
obj.field
a -> b
link_name = "puts"
```

对应拓扑：

```text
Infix("+", a, b)
Infix(":", x, i32)
Infix("::", T, method)
Infix(".", obj, field)
Infix("->", a, b)
Infix("=", link_name, "puts")
```

`Infix(":", x, i32)` 不表示 typed binding。

`Infix(".", obj, field)` 不表示 field access。

`Infix("->", a, b)` 不表示 function return type。

`Infix("=", a, b)` 不表示 assignment 或 binding。

这些只是中缀关联。

具体语义属于 Meta domain parser。

## 10. Juxt

`Juxt` 表示相邻节点形成的并列关系。

字段：

```text
left
right
span
syntax_context
```

例子：

```lain
let main
return x
comptime T
handle Throws
@export
```

对应拓扑：

```text
Juxt(Atom("let"), Atom("main"))
Juxt(Atom("return"), Atom("x"))
Juxt(Atom("comptime"), Atom("T"))
Juxt(Atom("handle"), Atom("Throws"))
Juxt(Atom("@"), Atom("export"))
```

`Juxt(Atom("return"), x)` 不表示 return statement。

`Juxt(Atom("let"), x)` 不表示 binding。

`Juxt(Atom("return"), x)` 不表示 return statement。

它们只是 token 并列。

`Juxt` 对 Lain 很重要，因为很多语言 form 都是由相邻拓扑表达的。

如果没有独立 `Juxt`，大量语义 parser 会被迫依赖空白、特殊 infix 或 ad-hoc token list。

## 11. Sep

`Sep` 表示分隔符 token。

字段：

```text
text
span
syntax_context
```

例子：

```text
,
;
```

对应拓扑：

```text
Sep(",")
Sep(";")
```

`Sep(";")` 不表示 statement terminator。

`Sep(",")` 不表示 argument separator。

它们只是分隔符 token。

是否按 `;` 切分 statement，属于 block / control parser。

是否按 `,` 切分参数，属于 fn / expr / type / effect parser。

例如，同样的逗号分隔拓扑可以被解释为：

```text
function parameter list
function argument list
type argument list
effect argument list
struct field list
tuple element list
import list
DSL list
```

因此 `Sep` 不能提前带有语义。

## 12. Metadata

每个 RawAst / AstTree 节点应携带以下元数据：

```text
span
syntax_context
```

## 13. Span

`Span` 表示源码位置。

它至少应该能定位：

```text
file
start offset
end offset
line
column
```

Span 用于：

```text
parse error
domain parse error
type error
effect error
macro expansion diagnostic
debug info
source map
```

Parser 负责给源码节点记录初始 Span。

Meta 展开生成的新节点也必须保留 origin 信息。

宏生成节点可以携带：

```text
generated span
origin span
call-site span
definition-site span
```

具体 representation 可以后续演进，但 diagnostic 不能丢失来源。

## 14. SyntaxContext

`SyntaxContext` 用于 macro hygiene。

Parser 只给源码节点赋予初始 SyntaxContext。

新的 hygiene context 由 Meta expansion 产生，不由 C parser 推导。

也就是说：

```text
Parser 创建初始 context。
Macro expansion 创建 fresh context。
Name resolution 使用 context 避免错误捕获。
```

Meta 构造 AST 时，不能直接拼接裸 identifier。

应该通过 Host API 创建带上下文的 identifier：

```scheme
(lain_ast_make_ident ctx "x" syntax-context)
(lain_ast_fresh_ident ctx "tmp" parent-context)
```

Hygiene 要避免：

```text
宏内部临时变量污染用户作用域。
用户变量意外捕获宏生成的 identifier。
宏生成引用绑定到错误定义。
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

这里的 `tmp` 必须是 fresh identifier，而不是可被用户作用域捕获的普通 `tmp`。

## 15. AstTree Representation

`AstTree` 是 RawAst 的 Scheme 表示。

推荐形状：

```scheme
(atom text span ctx)
(group delimiter children span ctx)
(prefix op operand span ctx)
(postfix operand op span ctx)
(infix op left right span ctx)
(juxt left right span ctx)
(sep text span ctx)
```

早期实现可以省略 `span` 和 `ctx`，但 API 和数据结构必须给它们留位置。

简化表示可以写成：

```scheme
(atom "x")
(group paren (...))
(prefix "!" expr)
(postfix callee (group paren args))
(infix "+" lhs rhs)
(juxt lhs rhs)
(sep ",")
```

但规范意义上，每个节点都应携带 span 和 syntax context。

## 16. Canonical AstTree Tags

`std/meta/canonicalize.scm` 的职责是：

```text
RawAst -> AstTree
```

它只能输出拓扑节点。

允许的固定 tag：

```text
atom
ident
number
string
sep
juxt
prefix
postfix
group
paren
brace
bracket
root
```

其中：

```text
ident
number
string
```

可以作为 Atom 的细分表示，但不能携带语言语义。

也就是说：

```scheme
(ident "i32")
```

仍然不表示类型。

```scheme
(ident "std")
```

仍然不表示标准构造器命名空间。

中缀 operator 可以作为动态 tag 出现：

```scheme
(+ left right)
(:: left right)
(. left right)
(: left right)
(-> left right)
(= left right)
```

这仍然只是拓扑信息，不是语义节点。

新增固定 tag 前，必须先判断它是不是 LAIN-AST 层应该知道的物理形状。

如果答案依赖 Lain 语言语义，就不能放进 `canonicalize.scm`。

## 17. Forbidden AstTree Tags

明确禁止的 AstTree tag 包括：

```text
call
type
type-app
fn
struct
effect
interface
module
import
param
field
block
stmt
expr
generic
macro
attribute
pattern
match
return
let
```

这些都是语言语义，不是源码拓扑。

例如，`canonicalize.scm` 不应该输出：

```scheme
(call callee args)
```

而应该输出：

```scheme
(postfix callee (group paren args))
```

是否是 call，由 `expr/parse.scm` 决定。

是否是 type application，由 `types/parse.scm` 决定。

是否是 effect application，由 `effects/form.scm` 决定。

## 18. AstTree Helper Boundary

`AstTree` helper 只能提供通用树操作。

允许的 helper：

```text
tree.kind
tree.span
tree.syntax-context
tree.children
tree.atom-text
tree.group-delim
tree.group-children
tree.prefix-op
tree.prefix-operand
tree.postfix-op
tree.postfix-operand
tree.infix-op
tree.infix-left
tree.infix-right
tree.juxt-left
tree.juxt-right
tree.sep-text
tree.flatten-juxt
tree.split-by-sep
tree.strip-paren
tree.expect-atom
tree.match-infix
tree.match-prefix
tree.match-postfix-group
```

禁止的 helper：

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
tree-parse-import
tree-parse-pattern
```

这些函数依赖 Lain 语言语义，必须放在对应 domain parser 中。

## 19. Function Construction Is Not Primitive Syntax

高层 callable 由 Meta 层的 `std::func` 构造器产生，不是 Parser 或
LAIN-AST 的内建语义。源码声明始终使用统一绑定。

源码：

```lain
let add = std::func(x: i32, y: i32) -> i32 {
    x + y
}
```

在 LAIN-AST 中只是：

```text
Atom("let")
Atom("add")
Atom("=")
Atom("std")
Atom("::")
Atom("func")
Group(paren, ...)
Infix("->", ...)
Group(brace, ...)
```

只有绑定 parser 与 `std::func` 构造器可以把它解释成：

```text
middle.fn
```

`middle.fn` 继续经过：

```text
name resolution
type checking
effect checking
generic specialization
ABI lowering
closure conversion
```

之后，才可能产生 LAIN-IR 的：

```text
#proc
```

因此：

```text
std::func callable != #proc
```

一个由 `std::func` 构造的高层 callable 可能 lower 为：

```text
一个 #proc
多个 specialized #proc
一个 closure object + invoke #proc
一个 wrapper / trampoline
一个 comptime-only function
一个 inline 后不存在的代码片段
```

LAIN-AST 不知道这些。

## 20. Struct Construction Is Not Primitive Syntax

高层结构类型由 Meta 层的 `std::struct` 构造器产生，并通过统一绑定命名。

源码：

```lain
let Pair: type = std::struct {
    a: i32,
    b: i32,
}
```

LAIN-AST 只能表达：

```text
Atom("let")
Atom("Pair")
Atom(":")
Atom("type")
Atom("=")
Atom("std")
Atom("::")
Atom("struct")
Group(brace, ...)
```

只有绑定 parser 与 `std::struct` 构造器可以把它解释为：

```text
middle.struct
```

字段列表、字段类型、layout、align、offset 都不是 LAIN-AST 的职责。

后续 layout pass 可以产生：

```text
Pair.size = 8
Pair.align = 4
Pair.a.offset = 0
Pair.b.offset = 4
```

最终 LAIN-IR 中应该是：

```lain-ir
#set %b_ptr = #offset %pair_addr 4
#set %b = #load #bits<32> %b_ptr none
```

而不是：

```text
#struct Pair { ... }
#field Pair.b
```

## 21. Type Is Not Primitive Syntax

类型表达式属于 Meta / Middle AST 层。

LAIN-AST 不知道类型。

例如：

```lain
x: i32
```

LAIN-AST 只能表达：

```scheme
(infix ":"
  (atom "x")
  (atom "i32"))
```

它不表示：

```text
typed binding
```

在 parameter context 中，`fn/parse.scm` 可以把它解释为参数类型标注。

在 variable binding context 中，`expr/parse.scm` 或 binding parser 可以把它解释为 binding annotation。

在其他 DSL context 中，它也可以有别的含义。

类型解析属于：

```text
types/parse.scm
```

类型检查属于：

```text
elaboration
```

物理类型 lower 属于：

```text
LAIN-IR lowering
```

## 22. Call Is Not Primitive Syntax

函数调用不是 LAIN-AST 原语。

源码：

```lain
foo(a, b)
```

LAIN-AST 只能表达：

```scheme
(postfix
  (atom "foo")
  (group paren
    ((atom "a")
     (sep ",")
     (atom "b"))))
```

它可能表示：

```text
value call
type application
effect application
macro invocation
attribute argument
DSL-specific form
```

因此 LAIN-AST 中不能出现：

```scheme
(call foo (a b))
```

`call` 是表达式语义，应该由 `expr/parse.scm` 生成：

```text
expr.call
```

## 23. Attribute Is Not Primitive Syntax

Attribute 是 Meta 层 form。

源码：

```lain
@foreign(link_name = "puts")
let puts = std::func(s: CStr) -> i32;
```

LAIN-AST 只能表达：

```scheme
(prefix "@"
  (postfix
    (atom "foreign")
    (group paren
      ((infix "="
         (atom "link_name")
         (string "\"puts\""))))))
```

`attrs/parse.scm` 或对应 domain parser 才能把它解释成 attribute。

Parser 不知道：

```text
foreign 是 attribute。
link_name 是 attribute field。
这个 attribute 作用于后面的 canonical binding。
```

这些是 Meta 语义。

## 24. Module and Import Are Not Primitive Syntax

Module 和 import 属于 Meta 层。

源码：

```lain
let io = import("std::io");

let math: Module = std::module {
    @export
    let answer = 42;
};
```

LAIN-AST 只能表达 token 并列和分隔：

```scheme
(juxt
  (juxt (juxt (atom "let") (atom "io")) (atom "="))
  (postfix (atom "import") (group paren ((string "\"std::io\"")))))
(sep ";")
(juxt
  (juxt
    (juxt
      (juxt
        (juxt (atom "let") (atom "math"))
        (atom ":"))
      (atom "Module"))
    (atom "="))
  (juxt
    (infix "::" (atom "std") (atom "module"))
    (group brace (...))))
```

统一绑定 parser 先取得 initializer；只有：

```text
module/parse.scm
import/parse.scm
```

可以把构造器调用解释为：

```text
middle.module
middle.import
```

Parser 不做 module resolution。

LAIN-AST 不保存 symbol table。

## 25. Generic Syntax Direction

LAIN-AST 的边界会影响泛型语法。

Rust / C++ 风格的 `<T>` 对无语义 AST 不友好。

原因是 `<` 和 `>` 同时可以表示：

```text
比较表达式
类型参数
约束
bit operation DSL
HTML-like DSL
其他嵌入语法
```

Parser 如果要把 `Vec<T>` 解析成专门的 generic node，就必须知道当前位置是类型上下文。

这会让 RawAst 偷跑语义。

Lain 更适合 Zig 风格泛型：

```lain
let identity = std::func(comptime T: type, x: T) -> T {
    x
}

let y = identity(i32, 10);
```

类型是一等 comptime value。

泛型是 comptime value parameter 的一种用法。

推荐形态：

```lain
Vec(i32)
Result(i32, Error)
Throws(i32)
Map(String, User)
```

不推荐把泛型写成：

```lain
Vec<i32>
Result<i32, Error>
Throws<i32>
foo::<i32>(x)
```

这样做的收益是：

```text
(...) 仍然只是 Group(paren, ...)
type application、effect application、value call 可以共用同一种拓扑
domain parser 根据上下文解释 Name(...)
RawAst 不需要理解 type context
不需要 turbofish 语法
泛型不成为 parser-level 特性
```

因此：

```text
Postfix(Name, Group(paren, ...))
```

在 AstTree 中不叫 call，也不叫 type application。

它只是一段拓扑。

不同 domain parser 可以分别解释：

```text
expr/parse.scm      value call
types/parse.scm     type application
effects/form.scm    effect application
attrs/parse.scm     attribute argument
```

## 26. Angle Bracket Ambiguity

源码：

```lain
a < b > c
```

和：

```lain
Vec<i32>
```

如果 Parser 不偷跑语义，这两段源码都会产生由 `<` 和 `>` 组成的 infix 拓扑。

Parser 无法只凭局部形状知道第二个是不是泛型。

因此 `<...>` 不应作为核心泛型语法。

`<...>` 不属于核心语法，也不提供兼容入口。类型和值应用统一使用
`Name(...)`；旧写法在 domain phase 被拒绝。

也就是说，不能新增：

```text
GenericApply
TypeApply
Turbofish
```

这类 RawAst 节点。

## 27. Middle AST Ownership

只有 Meta domain parser 能把拓扑赋予语义。

建议归属：

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

Domain parser 可以共享通用 helper。

但不能把自己的语义 helper 放回 AstTree 层。

## 28. Example: Callable Binding

Source:

```lain
let main = std::func() -> i32 {
    return 0;
}
```

AstTree：

```scheme
(juxt
  (juxt
    (juxt
      (juxt (atom "let") (atom "main"))
      (atom "="))
    (postfix
      (infix "::" (atom "std") (atom "func"))
      (group paren ())))
  (infix "->"
    (atom "i32")
    (group brace
      ((juxt (atom "return") (atom "0"))
       (sep ";")))))
```

这不是 Parser 内建的函数声明。

它只是拓扑树。

绑定 parser 先解析 `let` shell，`std::func` 构造器再把 initializer
解释成：

```text
middle.fn
  name: main
  params: []
  return: type.ref(i32)
  body: ...
```

`control/parse.scm` 或 `expr/parse.scm` 才能解释：

```text
return 0;
```

## 29. Example: Method-Looking Call

Source:

```lain
a.b(c)
```

AstTree：

```scheme
(postfix
  (infix "."
    (atom "a")
    (atom "b"))
  (group paren
    ((atom "c"))))
```

LAIN-AST 不知道这是 method call。

`expr/parse.scm` 可以把它解释为：

```text
expr.call
  callee: expr.field(expr.ref(a), b)
  args: [expr.ref(c)]
```

也可以在后续 type checking 中把它改写为：

```text
expr.call
  callee: resolved method
  receiver: a
  args: [c]
```

但这些都不是 LAIN-AST 的职责。

## 30. Example: Type Application

Source:

```lain
Vec(i32)
```

AstTree：

```scheme
(postfix
  (atom "Vec")
  (group paren
    ((atom "i32"))))
```

在类型上下文中，`types/parse.scm` 可以把它解释成：

```text
type.app
  callee: type.ref(Vec)
  args: [type.ref(i32)]
```

在表达式上下文中，同样拓扑可以被解释成：

```text
expr.call
  callee: expr.ref(Vec)
  args: [expr.ref(i32)]
```

## 31. Example: Generic Function

Source:

```lain
let identity = std::func(comptime T: type, x: T) -> T {
    x
}
```

AstTree 中：

```lain
comptime T: type
```

只是 parameter list 里的普通拓扑。

大致形状：

```scheme
(infix ":"
  (juxt
    (atom "comptime")
    (atom "T"))
  (atom "type"))
```

`fn/parse.scm` 和 `types/parse.scm` 负责把它解释成 comptime type parameter。

Parser 不知道：

```text
comptime 是参数 modifier。
T 是 type parameter。
type 是类型的类型。
```

这些属于 Meta。

## 32. Example: Effect Set

Source:

```lain
let f = std::func() -> i32 ! {Throws(i32), Suspend} {
    0
}
```

AstTree 中：

```lain
! {Throws(i32), Suspend}
```

仍然只是 prefix 或 infix 结构加 brace group。

可能形状：

```scheme
(prefix "!"
  (group brace
    ((postfix
       (atom "Throws")
       (group paren ((atom "i32"))))
     (sep ",")
     (atom "Suspend"))))
```

`effects/form.scm` 负责把它解释成：

```text
effect.set
  effect.app(Throws, [i32])
  effect.ref(Suspend)
```

LAIN-AST 不知道 effect。

## 33. Example: Attribute

Source:

```lain
@foreign(link_name = "puts")
let puts = std::func(s: CStr);
```

AstTree：

```scheme
(prefix "@"
  (postfix
    (atom "foreign")
    (group paren
      ((infix "="
         (atom "link_name")
         (string "\"puts\""))))))
```

这不表示 attribute。

`attrs/parse.scm` 才能解释：

```text
attr.foreign
  link_name: "puts"
```

top-level binding parser 决定这个 attribute 作用于后面的 `let` binding；
`@` 只承担 attribute 前缀，不引入另一类声明。

## 34. Example: Struct Binding

Source:

```lain
let Pair: type = std::struct {
    a: i32,
    b: i32,
}
```

AstTree：

```scheme
(juxt
  (juxt
    (juxt
      (juxt
        (juxt
          (juxt (atom "let") (atom "Pair"))
          (atom ":"))
        (atom "type"))
      (atom "="))
    (infix "::" (atom "std") (atom "struct")))
  (group brace
    ((infix ":" (atom "a") (atom "i32"))
     (sep ",")
     (infix ":" (atom "b") (atom "i32"))
     (sep ","))))
```

这不表示结构体。

绑定 parser 与 `std::struct` 构造器才能解释：

```text
middle.struct
  name: Pair
  fields:
    a: type.ref(i32)
    b: type.ref(i32)
```

## 35. Example: Block-Looking Brace Group

Source:

```lain
{
    x;
    y;
}
```

AstTree：

```scheme
(group brace
  ((atom "x")
   (sep ";")
   (atom "y")
   (sep ";")))
```

这不表示 block。

它可能是：

```text
expression block
function body
struct body
effect set
match body
DSL body
```

具体含义由上下文决定。

## 36. Example: Ambiguous Angle Brackets

Source:

```lain
a < b > c
```

AstTree：

```scheme
(>
  (<
    (atom "a")
    (atom "b"))
  (atom "c"))
```

Source:

```lain
Vec<i32>
```

如果没有 parser-level type context，它也只能被解析成 `<` / `>` 的 infix 拓扑。

Parser 不应该将其特判为：

```text
type application
```

这就是 Lain 不推荐 `<T>` 泛型语法的原因。

## 37. Quasiquote Boundary

Meta 层可以提供：

```scheme
#{ ... }
```

作为构造 AstTree 的语法糖。

例如：

```scheme
#{
  if (!,(cond)) {
    panic(,(msg));
  }
}
```

它生成的是 AstTree，不是 Middle AST，也不是 LAIN-IR。

也就是说，它生成的仍然是：

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
```

之后还需要 domain parser 判断：

```text
if 是不是控制 form。
panic(...) 是不是 call。
{...} 是不是 block。
```

Quasiquote 不能绕过 LAIN-AST 合同直接生成：

```text
expr.if
expr.call
#proc
#call
```

除非那是另一个明确的 Middle AST quote 或 IR quote 机制。

## 38. Current Implementation Notes

当前代码已经接近这个方向，但还没有完全符合本合同。

已知位置：

```text
std/meta/canonicalize.scm
  C Pratt AST -> Scheme AstTree。
  它应该只负责 RawAst -> AstTree。

std/meta/surface/tree.scm
  当前同时包含通用 tree helper 和一些语义 parser。
  后续应该逐步拆到对应 domain。

types/parse.scm
  应负责 type expression 和 type application。

expr/parse.scm
  应负责 value expression 和 call。

effects/form.scm
  应负责 effect set 和 effect application。

control/parse.scm
  应负责 block、if、loop、match、return。

fn/parse.scm
  应负责 std::func initializer。

struct/parse.scm
  应负责 std::struct initializer。
```

`canonicalize.scm` 不应该生成：

```scheme
(call callee args)
```

括号后缀应保持为：

```scheme
(postfix callee (group paren args))
```

然后由 domain parser 判断：

```text
expr/parse.scm      call
types/parse.scm     type application
effects/form.scm    effect application
attrs/parse.scm     attribute argument
```

当前 `surface/tree.scm` 如果支持 `<...>` 形式的 type application，后续应优先迁移到：

```lain
Name(TypeArg, ...)
```

`<...>` 不属于核心语法；类型和值应用统一写成 `Name(...)`。

## 39. Migration Plan

建议按小步迁移：

```text
1. 先把本文档作为 LAIN-AST 合同。
2. 新增代码只依赖无语义 AstTree helper。
3. 让 canonicalize.scm 只输出拓扑节点。
4. 不再从 canonicalize.scm 生成 call/type/fn/struct 等语义 tag。
5. 在 types/parse.scm 中优先支持 Name(TypeArg, ...) type application。
6. 在 effects/form.scm 中优先支持 Effect(TypeArg, ...) effect application。
7. 把 tree-parse-type 迁到 types/parse.scm。
8. 把 tree-lower-expr 迁到 expr/parse.scm。
9. 把 block parsing 迁到 control/parse.scm 或专门的 block parser。
10. 把 effect set parsing 迁到 effects/form.scm。
11. 让顶层 parser 只接受 let NAME [: EXPECTED] = INITIALIZER。
12. 把 std::func initializer elaboration 放在 fn/parse.scm。
13. 把 std::struct initializer elaboration 放在 struct/parse.scm。
14. 在 domain phase 拒绝旧式独立 fn、struct、module、import 声明。
15. 类型和值应用统一使用圆括号，不接受 <...>。
16. 让 Juxt 和 Sep 在规范与实现中逐渐成为独立节点。
```

迁移过程中，Middle AST 和 LAIN-IR 不需要一起重写。

重点是先把 LAIN-AST 的边界收紧。

## 40. Boundary Summary

LAIN-AST 只包含：

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
Span
SyntaxContext
```

LAIN-AST 不包含：

```text
fn
let
struct
module
import
interface
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
match
```

职责边界：

```text
Parser
  只构建 RawAst 拓扑。

RawAst
  C 层无语义节点树。

AstTree
  Meta 层无语义树表示。

Domain Parser
  把 AstTree 解释成 Middle AST。

Middle AST
  表示高层语言语义。

Typed / Elaborated Middle AST
  表示已解析、已检查、已特化的语言语义。

LAIN-IR
  表示物理执行。

Backend
  生成目标代码。
```

最终规则：

```text
LAIN-AST 只描述拓扑。
Middle AST 描述语义。
LAIN-IR 描述物理执行。
```
