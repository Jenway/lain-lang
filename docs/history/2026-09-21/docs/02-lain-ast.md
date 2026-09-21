> 历史记录。原路径：`docs/02-lain-ast.md`。归档日期：2026-09-21。
> 本文保留整理前的内容；其中的状态、命令、语法和结论不作为现行依据。
> 当前文档从 [文档索引](../../../README.md) 阅读。

# LAIN-AST 规范

LAIN-AST 是 Lain 源码的无语义 token tree。它精确保留 token、分隔符嵌套、源码位置和宏卫生信息，但不解释函数、类型、模块、调用、运算符或其他语言概念。

```text
Source text
  -> lexer and delimiter grouping
  -> LAIN-AST
  -> standard-library Meta
  -> LAINIR
```

## 1. 核心边界

LAIN-AST 只回答：

```text
源码中有哪些 token？
哪些 token 位于同一个 delimiter group？
它们的顺序、位置和 syntax context 是什么？
```

下面的问题由 Meta 回答：

```text
这是 binding、callable、type 还是 module 吗？
圆括号表示调用、类型工厂参数还是宏参数吗？
花括号表示函数体、record body 还是 effect set 吗？
某个名字绑定到什么值？
运算符的优先级、结合性和具体语义是什么？
表达式具有哪种类型？
```

因此 Parser 不内建 `let`、`std::func`、`std::struct`、`std::module`、`import`、`effect`、`macro` 等语言形式。

## 2. 节点模型

LAIN-AST 只有两种节点：

```text
Atom
Group
```

根节点也是一个 Group，包含源码顶层的有序节点序列。

### 2.1 Atom

Atom 表示一个 lexer token，例如：

```text
let
main
=
std
::
func
40
+
"hello"
;
```

Atom 至少携带：

```text
token kind
source start
source length
origin
syntax context
next sibling
```

Atom 的文本由 source span 取得。`Atom("i64")` 只是文本为 `i64` 的 token，并不天然是类型。

### 2.2 Group

Group 表示一对匹配分隔符包围的子节点序列：

```text
( ... )
[ ... ]
{ ... }
```

Group 至少携带：

```text
delimiter kind
source span
first child
last child
child count
origin
syntax context
next sibling
```

Group 不记录它的语言用途。圆括号可能是参数、实参、分组表达式或宏输入；花括号可能是 callable body、module body、record body、effect set 或 DSL 数据。

## 3. 为什么没有 Prefix、Postfix 和 Infix

LAIN-AST 不定义：

```text
Prefix
Postfix
Infix
Juxt
Sep
Call
Block
Statement
Expression
TypeApplication
```

建立这些节点需要 Parser 预先知道运算符类别、优先级、结合性，以及相邻 token 或 separator 的意义。这些规则属于标准库 Meta。

例如：

```lain
a + b * c
```

LAIN-AST 保存为同一 Group 中的五个 Atom：

```text
Atom("a")
Atom("+")
Atom("b")
Atom("*")
Atom("c")
```

Meta 的表达式 parser 再根据当前语言环境决定优先级、重载和结果类型。

同样：

```lain
value(arg)
```

保存为：

```text
Atom("value")
Group(paren)
└── Atom("arg")
```

它可能被 Meta 解释成运行时调用、类型工厂调用、宏调用或其他库定义形式。

## 4. 完整例子

源码：

```lain
let main = std::func() -> i64 {
    return 40 + 2;
};
```

LAIN-AST：

```text
Group(root)
├── Atom("let")
├── Atom("main")
├── Atom("=")
├── Atom("std")
├── Atom("::")
├── Atom("func")
├── Group(paren)
├── Atom("->")
├── Atom("i64")
├── Group(brace)
│   ├── Atom("return")
│   ├── Atom("40")
│   ├── Atom("+")
│   ├── Atom("2")
│   └── Atom(";")
└── Atom(";")
```

LAIN-AST 中没有 function declaration、return statement 或 addition expression。Meta 按顺序完成：

```text
识别统一 binding shell
  -> 解析 std::func Meta 构造器
  -> 建立 callable 和 i64 type value
  -> 解析 body、return 和 operator precedence
  -> 完成检查
  -> lower 为 LAINIR #proc 和 #add
```

## 5. Source span 与 origin

直接来自源码的节点携带 source span：

```text
source identity
byte start
byte length
```

生成节点还可以携带 origin，用来记录它来自哪个宏定义、调用位置或被复制节点。诊断系统通过 source span 和 origin 把错误映射回用户源码。

复制节点时保留原 span 和 origin。新建节点必须显式附加 origin；不得用宿主指针或临时字符串地址代替稳定的源码身份。

## 6. Trivia 与无损性

lexer 保存注释、空白和行列信息。RawAst 节点引用原 source buffer，因此工具可以通过相邻 span 恢复节点之间的原始文本。

语义 Meta pass 不把 trivia 当作语言节点。formatter、source rewriter 和诊断工具可以通过 lexer/source API 读取它。

宏生成的文本不在原始 source buffer 中时，由 syntax context 持有的生成文本池提供稳定存储。

## 7. Syntax context 与 hygiene

每个节点携带 syntax context：

- 源码节点初始使用 source context；
- 复制用户输入时保留原 context；
- 每次宏展开获得 fresh context；
- 宏模板引入的名字使用展开 context；
- 宏参数替换保留调用方节点的 context；
- 有意捕获调用方名字必须使用明确的 capture 操作。

例如宏内部生成临时名字 `value` 时，它不能意外绑定到调用位置已有的 `value`。名字解析使用文本与 syntax context 共同决定标识符身份。

## 8. 通用 AST API

compiler core 向 Meta 暴露通用 AstApi：

```text
node_kind
delimiter
token_kind
source_span
origin
syntax_context
first_child
last_child
next_sibling
child_count

copy
replace
remove
append
new_atom
new_group
set_origin
set_syntax_context
fresh_identifier
```

API 使用 opaque handle。标准库不能通过裸地址偏移依赖 compiler core 的节点布局。

AstApi 不提供：

```text
parse_function
parse_type
parse_call
resolve_module
check_effect
lower_expression
```

这些属于 Meta 标准库。

## 9. 变换不变量

每次 AST 变换都必须保持：

- Group 的 child chain 有限且无环；
- first、last 和 child count 一致；
- sibling 只连接同一 Group 中的节点；
- delimiter 成对且不可通过普通 Atom 伪造；
- source span 位于所属 source buffer 内；
- 生成节点具有有效文本存储和 origin；
- syntax context 不因普通复制或移动而丢失；
- nil handle 可以安全查询，不等同于宿主空指针。

变换失败必须返回诊断，不能把部分修改的树交给下一个 Meta 阶段。

## 10. Parser 诊断

Parser 只报告词法和拓扑错误，例如：

```text
非法字节或未终止字符串
未终止注释
不匹配或未闭合的 delimiter
资源或深度限制超出
```

下面的错误不属于 Parser：

```text
未知名字
重复 binding
调用参数数量不匹配
类型不匹配
未知 module member
无效 effect
宏参数数量不匹配
```

这些由相应 Meta 阶段诊断。

## 11. 生命周期与资源归属

一个 LAIN-AST workspace 拥有：

```text
source buffers
lexer tokens and trivia
RawAst nodes
generated syntax text pools
syntax contexts
diagnostics
```

AST handle 只能在所属 compile context 存活期间使用。跨 Meta pass 传递结果时，pass result 必须记录 owner；复制到另一个 context 或转移 owner 必须通过显式 API 完成。

AST 资源归属是编译器运行时生命周期规则，不是 Lain 源语言的 ownership 或 borrow checking。

## 12. 当前实现状态

当前 `bootstrap/compiler/raw_ast.l1` 已实现：

- Atom 和 Group；
- source span、origin 和 syntax context；
- child/sibling 遍历；
- copy、replace、remove 和 append；
- generated text pool 和 fresh syntax context；
- 基础宏模板复制、参数替换和 hygiene 探针；
- AST 从 `#eval` 返回的对象路径。

当前仍需继续收敛：

- 把宏与语言形式识别从 `raw_ast.l1` 的 bootstrap probe 移入正式 `std::meta`；
- 完善 parent/insert 等编辑 API；
- 统一所有变换的失败原子性；
- 为深度、节点数量和生成文本设置 compile-context 限制；
- 删除文档和代码中残留的 `AstTree`、固定 Middle AST 和 Scheme 表述。

Meta 的阶段与语义职责见 [`03-meta-system.md`](03-meta-system.md)。
