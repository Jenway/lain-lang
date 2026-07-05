# LAIN-AST Contract

本文档定义 LAIN-AST 的职责边界。

LAIN-AST 只描述源码文本的结构拓扑。它不描述 Lain 语言语义。

## 1. Layer Names

前端分三层命名，避免把不同阶段混在一起：

```text
Source Text -> RawAst -> AstTree -> Middle AST -> LAIN-IR
```

`RawAst` 是 C parser 产出的 arena 节点树。节点用 `NodeId` 引用。

`AstTree` 是 `RawAst` 的 Scheme 值表示。它仍然是无语义拓扑树，只是更方便 meta pass 操作。

`Middle AST` 是 meta domain parser 产出的语义树，例如 `middle.fn`、`middle.struct`、`expr.call`、`type.ref`。

`LAIN-IR` 是物理执行层。它承载 lower 后的类型、内存、控制流、调用和 comptime evaluation。

## 2. Core Rule

LAIN-AST 只能回答一个问题：

```text
源码 token 如何组合成嵌套结构？
```

它不能回答这些问题：

```text
这是不是函数？
这是不是类型？
这是不是函数调用？
这是不是泛型参数？
这是不是 effect set？
这个 identifier 绑定到哪里？
```

这些问题属于 meta 层。

## 3. Parser Responsibilities

C parser 允许做：

- tokenization
- bracket grouping
- prefix / postfix / infix association
- operator precedence
- sibling ordering
- span recording

C parser 不允许做：

- 识别 `fn`、`let`、`struct`、`effect`、`interface`、`import` 等 keyword 的语义
- 生成 `call`、`type`、`block`、`param`、`field` 等语义节点
- 判定 `<...>` 是比较表达式、泛型参数还是别的 DSL 结构
- 判定 `(...)` 是调用参数、分组表达式、类型参数列表还是 attribute 参数
- 判定 `{...}` 是函数体、struct body、effect set、match arms 还是普通 block
- 做 name resolution、type checking、effect checking

## 4. RawAst Node Kinds

`RawAst` 只有物理节点形状。

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
```

当前 C 实现可以继续把 `Juxt` 表示为空白 `Infix`，把 `Sep` 表示为普通 atom。语义合同上，它们仍然是独立形状。

### Atom

不可再拆分的 token。

字段：

```text
text
span
syntax_context
```

例子：

```text
fn
main
i32
42
"hello"
+ 
::
```

`Atom("fn")` 不表示函数声明。它只是文本 `fn`。

### Group

由成对 delimiter 包住的子节点序列。

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

```text
(a, b)       -> Group(paren, ...)
{ x; y; }    -> Group(brace, ...)
[T; 4]       -> Group(bracket, ...)
```

`Group(paren, ...)` 不表示函数调用参数。是否为调用参数由 meta parser 决定。

### Prefix

前缀 operator 和一个 operand。

字段：

```text
op
operand
span
syntax_context
```

例子：

```text
!cond
&x
*ptr
@foreign
```

`Prefix("@", x)` 不表示 attribute。它只是 `@` 绑定到右侧结构。

### Postfix

一个 operand 和一个后缀 operator 或 delimiter group。

字段：

```text
operand
op
span
syntax_context
```

例子：

```text
x?
foo(...)
arr[0]
```

`Postfix(foo, Group(paren, ...))` 不表示 call。函数调用、macro 调用、attribute 参数都可以从相同拓扑解析出来。

### Infix

中缀 operator 和左右子树。

字段：

```text
op
left
right
span
syntax_context
```

例子：

```text
a + b
x: i32
T::method
obj.field
```

`Infix(":", x, i32)` 不表示 typed binding。它只是冒号的左右关联。

### Juxt

相邻节点形成的并列关系。

字段：

```text
left
right
span
syntax_context
```

例子：

```text
fn main
return x
handle Throws
```

`Juxt(Atom("return"), x)` 不表示 return statement。它只是 token 并列。

### Sep

分隔符 token。

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

`Sep(";")` 不表示 statement terminator。它只是分隔符。

## 5. AstTree Representation

`AstTree` 是 RawAst 的 Scheme 表示，建议使用这些形状：

```scheme
(atom text span ctx)
(group delimiter children span ctx)
(prefix op operand span ctx)
(postfix operand op span ctx)
(infix op left right span ctx)
(juxt left right span ctx)
(sep text span ctx)
```

实现可以在早期省略 `span` 和 `ctx`，但 API 设计要给它们留位置。

当前 `std/meta/canonicalize.scm` 的固定输出 tag 白名单是：

```text
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

中缀 operator 可以作为动态 tag 出现：

```scheme
(+ left right)
(:: left right)
(. left right)
(: left right)
```

这仍然是拓扑信息，不是语义节点。新增固定 tag 前，必须先判断它是不是 LAIN-AST 层应该知道的物理形状。如果答案依赖 Lain 语言语义，就不能放进 `canonicalize.scm`。

明确禁止的 AstTree tag 包括：

```text
call
type
type-app
fn
struct
effect
interface
import
param
field
block
stmt
expr
generic
```

`AstTree` helper 只能提供通用树操作：

```scheme
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

`AstTree` helper 不应该提供：

```scheme
tree-parse-type
tree-lower-expr
tree-parse-block
tree-parse-params
tree-parse-effect-set
```

这些函数属于对应 domain。

## 6. Generic Syntax Direction

LAIN-AST 的边界会影响泛型语法。

Rust / C++ 风格的 `<T>` 对无语义 AST 不友好。`<` 和 `>` 同时可以表示比较、类型参数、约束、DSL 片段。parser 如果要把 `Vec<T>` 解析成专门的 generic node，就必须知道当前位置是类型上下文。这会让 RawAst 偷跑语义。

Lain 更适合 Zig 风格泛型：

```lain
fn identity(comptime T: type, x: T) -> T {
    x
}

let y = identity(i32, 10);
```

类型是一等值。泛型就是带 comptime type 参数的普通函数、普通 struct、普通 effect。

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

- `(...)` 仍然只是 `Group(paren, ...)`
- type application、effect application、value call 可以共用同一种拓扑
- domain parser 根据上下文解释 `Name(...)`
- RawAst 不需要理解 type context
- turbofish 语法不再需要

因此，`Postfix(Name, Group(paren, ...))` 在 AstTree 中不叫 call，也不叫 type application。它只是一段拓扑。`expr/parse.scm`、`types/parse.scm`、`effects/form.scm` 可以分别把它解释成 value call、type application、effect application。

## 7. Middle AST Ownership

只有 meta domain parser 能把拓扑赋予语义。

建议归属：

```text
fn/parse.scm          AstTree -> middle.fn / middle.foreign-fn
struct/parse.scm      AstTree -> middle.struct
types/parse.scm       AstTree -> type.*
expr/parse.scm        AstTree -> expr.*
control/parse.scm     AstTree -> control forms
effects/form.scm      AstTree -> effect forms
interface/parse.scm   AstTree -> interface forms
module/parse.scm      AstTree -> module forms
import/parse.scm      AstTree -> import forms
```

Domain parser 可以共享通用 helper，但不能把自己的语义 helper 放回 AstTree 层。

## 8. Examples

这些例子使用简化 AstTree，省略 `span` 和 `syntax_context`。

### Function Declaration

Source:

```lain
fn main() -> i32 {
    return 0;
}
```

AstTree:

```scheme
(juxt
  (juxt
    (atom "fn")
    (postfix
      (atom "main")
      (group paren ())))
  (infix "->"
    (atom "i32")
    (group brace
      ((juxt (atom "return") (atom "0"))
       (sep ";")))))
```

`fn/parse.scm` 才能把它解释成 `middle.fn`。

### Method-Looking Call

Source:

```lain
a.b(c)
```

AstTree:

```scheme
(postfix
  (infix "."
    (atom "a")
    (atom "b"))
  (group paren
    ((atom "c"))))
```

`expr/parse.scm` 可以把它解释成 method call。别的 DSL 也可以解释成别的东西。

### Type Application

Source:

```lain
Vec(i32)
```

AstTree:

```scheme
(postfix
  (atom "Vec")
  (group paren
    ((atom "i32"))))
```

`types/parse.scm` 可以把它解释成 type application。`expr/parse.scm` 在表达式上下文里也可以把同样拓扑解释成普通 call。

### Generic Function

Source:

```lain
fn identity(comptime T: type, x: T) -> T {
    x
}
```

AstTree 中 `comptime T: type` 只是 parameter list 里的普通拓扑。`fn/parse.scm` 和 `types/parse.scm` 负责把它解释成 comptime type parameter。

### Effect Set

Source:

```lain
fn f() -> i32 ! {Throws(i32), Suspend} {
    0
}
```

AstTree 中 `! {Throws(i32), Suspend}` 仍然只是 prefix 或 infix 结构加 brace group。`effects/form.scm` 负责把它解释成 effect set。

### Ambiguous Angle Brackets

Source:

```lain
a < b > c
Vec<i32>
```

如果 parser 不偷跑语义，这两段源码都会产生由 `<` 和 `>` 组成的 infix 拓扑。它无法只凭局部形状知道第二个是不是泛型。

这就是 Lain 不推荐 `<T>` 泛型语法的原因。类型既然是一等公民，`Vec(i32)` 更符合 AST 合同。

### Attribute

Source:

```lain
#[foreign(link_name = "puts")]
fn puts(s: CStr);
```

AstTree:

```scheme
(prefix "#"
  (group bracket
    ((postfix
       (atom "foreign")
       (group paren
         ((infix "="
            (atom "link_name")
            (atom "\"puts\""))))))))
```

`attrs` 或 `fn/parse.scm` 才能把它解释成 attribute。

## 9. Current Implementation Notes

当前代码已经接近这个方向，但还没有完全符合本合同。

`std/meta/canonicalize.scm` 会把 C Pratt AST 转成 Scheme tree，这是 `RawAst -> AstTree` 的位置。

`std/meta/surface/tree.scm` 现在同时包含通用 tree helper 和一些语义 parser，比如 type、block、expr、effect set 解析。后续应该逐步拆到对应 domain。

`canonicalize.scm` 不应该生成 `(call callee args)`。括号后缀应保持为 postfix/group 表示，再由 `expr/parse.scm` 判定它是否是 call。

当前 `surface/tree.scm` 支持 `<...>` 形式的 type application。后续如果采用 Zig 风格泛型，应优先支持 `Name(TypeArg, ...)`，再把 `<...>` 降级为兼容语法或移除。

## 10. Migration Plan

建议按小步迁移：

1. 先把本文档作为 LAIN-AST 合同。
2. 新增代码只依赖无语义 AstTree helper。
3. 在 `types/parse.scm` 中优先支持 `Name(TypeArg, ...)` type application。
4. 在 `effects/form.scm` 中优先支持 `Effect(TypeArg, ...)` effect application。
5. 把 `tree-parse-type` 迁到 `types/parse.scm`。
6. 把 `tree-lower-expr` 迁到 `expr/parse.scm`。
7. 把 block parsing 迁到 `control/parse.scm` 或专门的 block parser。
8. 把 effect set parsing 迁到 `effects/form.scm`。
9. 逐步删除 domain parser 对 legacy `(call callee args)` 形状的兼容。
10. 决定 `<...>` 是兼容语法、实验语法，还是完全移除。

迁移过程中，Middle AST 和 LAIN-IR 不需要一起重写。重点是把 LAIN-AST 的边界收紧。
