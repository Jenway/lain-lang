# Bootstrap Core

本文档定义旧世界必须实现的最小 Lain 子集。

Bootstrap Core 的目标不是好看，也不是完整。它只需要足够写第一批 Lain-meta compiler code。

## 1. Goal

Bootstrap Core 必须能表达：

```text
compiler data structures
tree traversal
parser helpers
diagnostic construction
small lowering passes
IR builder calls
comptime execution
```

它不追求：

```text
full language ergonomics
complete stdlib
advanced type system
production diagnostics
optimizer
package ecosystem
```

## 2. Required Syntax

旧世界至少要支持：

```text
module/import declarations
fn declarations
foreign fn declarations
struct declarations
enum or tagged-union declarations
let local bindings
assignment where needed for builders
if expressions/statements
match expressions/statements
return
function call
field access
method-looking call if it lowers to ordinary call
paren type/value application: Name(T)
brace blocks
line comments
```

`<T>` generic syntax 不属于 Bootstrap Core。

推荐泛型形态：

```lain
Option(T)
Result(T, E)
List(T)
Slice(T)
```

如果完整泛型还没准备好，旧世界可以把这些作为 bootstrap-known type constructors。这个 special case 必须留在 Bootstrap Core 范围内，后续由新世界实现的泛型替换。

## 3. Required Types

Primitive:

```text
unit
bool
i32
i64
usize
addr
CStr
String or string-view equivalent
```

Compiler data:

```text
Symbol
Span
Diagnostic
AstTree
MiddleNode
IrModule
IrBlock
IrValue
```

Containers:

```text
Option(T)
Result(T, E)
List(T)
Slice(T)
```

`Map(K, V)` 可以晚于 `0.1.0`，除非 parser migration 明确需要。

## 4. Required Control Flow

必须支持：

```text
if condition { ... } else { ... }
match value { ... }
early return Result.Err(...)
simple loops or list traversal helper
```

如果 loop 语法还不稳定，可以先用标准库 traversal helper 支撑 parser code：

```lain
list_each(nodes, fn(node) { ... })
list_fold(nodes, init, fn(acc, node) { ... })
```

但这种 helper 必须能由旧世界编译。

## 5. Required Data Modeling

写 compiler 需要 tagged data。

首选：

```lain
enum AstTree {
    Ident(Symbol),
    Number(i64),
    String(Symbol),
    Sep(Symbol),
    Juxt(AstTree, AstTree),
    Prefix(Symbol, AstTree),
    Postfix(AstTree, AstTree),
    Group(GroupKind, List(AstTree)),
}
```

如果 enum 暂时无法完整实现，允许 bootstrap tagged struct：

```lain
struct AstTree {
    tag: AstTag,
    payload: addr,
}
```

但 `match` 或等价 dispatch 必须可用。不能让 Lain-meta 代码退化成大量裸整数 tag 判断。

## 6. Required Comptime

Bootstrap Core 必须能在编译期执行：

```text
pure functions
small parser helpers
diagnostic formatting helpers
IR builder wrapper calls
file read through explicit host capability
```

comptime 执行路径必须经过 LAIN-IR interpreter。旧世界不能绕过 IR 单独实现第二套 evaluator。

## 7. Required IR Builder Surface

Lain-meta 不应直接操作 C FFI 细节。

Bootstrap Core 需要一层 Lain wrapper：

```lain
struct IrBuilder {
    module: IrModule,
}

fn fn_begin(b: IrBuilder, name: Symbol, sig: FnSig) -> Result(IrFn, Diagnostic)
fn block_new(b: IrBuilder, fn: IrFn) -> IrBlock
fn const_i32(b: IrBuilder, value: i32) -> IrValue
fn call(b: IrBuilder, callee: IrFn, args: Slice(IrValue)) -> Result(IrValue, Diagnostic)
fn ret(b: IrBuilder, block: IrBlock, value: IrValue) -> Result(unit, Diagnostic)
```

旧世界可以在下面继续调用 Scheme/C FFI，但 Lain-meta 代码只能依赖这层 wrapper。

## 8. Required Diagnostics

最低诊断模型：

```lain
struct Diagnostic {
    span: Option(Span),
    code: Symbol,
    message: String,
}
```

parser/lowerer 返回：

```lain
Result(T, Diagnostic)
```

`panic` 式错误只能用于 compiler bug，不能用于普通用户源码错误。

## 9. Out Of Scope For 0.1

这些不进入 Bootstrap Core 完成线：

```text
full effect handlers
multi-effect ABI
spawn/suspend semantics
full interface/typeclass dispatch
dynamic dispatch polish
borrow checker
macro expansion system
complete generic constraints
operator overloading
async runtime
package manager
optimizer
incremental compilation
rich diagnostic rendering
```

已经存在的旧实现可以保留，但不应阻塞 `0.1.0`。

## 10. Acceptance Slices

`0.1.0` 至少需要这些自举切片：

```text
AstTree schema compiles
Option/Result/List/Slice compile and run simple tests
enum/tagged union plus match compiles
parser helper can split comma-separated groups
type parser slice parses Name(T)
fn parser slice parses fn name(params) -> ret
lowering slice emits a simple LAIN-IR function
comptime helper builds a small AstTree or IR fragment
```

每个切片应该有对应 `tests/bootstrap-core/` 测试。

## 11. Stop Rule

旧世界开发时，任何新工作都先问：

```text
这是否直接支撑 Bootstrap Core 自举切片？
```

如果答案是否定，就推迟到新世界。

