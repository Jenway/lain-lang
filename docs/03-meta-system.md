# Lain Meta 系统

Meta 是 Lain 的语言定义层。它读取无语义的 RawAst，解释语言规则，完成语法展开和静态语义处理，并生成 LAINIR。

Meta 负责决定程序的含义。LAINIR 负责验证已经确定的物理操作；LAINVM 负责执行它们。
所有编译期执行都通过显式的 LAINIR `#eval` 发生。

## 1. 在编译流程中的位置

```text
Lain source
  -> RawAst
  -> Meta expand
  -> Meta elaborate
  -> Meta lower
  -> LAINIR
  -> LAINIR verify
  -> LAINVM execute #eval
  -> remaining runtime LAINIR
  -> backend
```

Meta 覆盖 RawAst 与 LAINIR 之间的语言解释过程。它可以在内部使用 AST、符号表、类型对象、module 对象或其他语义表示；这些表示由标准库拥有，不属于 RawAst 或 LAINIR 的公共格式。

## 2. 核心边界

四层分别拥有不同信息：

| 层 | 拥有的信息 |
| --- | --- |
| Parser / RawAst | token、分隔符、拓扑、span、syntax context |
| Meta / 标准库 | 绑定、函数、类型、模块、宏、effect、特化、布局等语言语义 |
| LAINIR | 固定物理类型、过程、控制流、内存操作、调用和 `#eval` 语法 |
| LAINVM | TCB、VSpace、Trap、预算、capability 与 `#eval` 执行 |
| Backend | 目标代码布局、目标 ABI 和产物生成 |

Parser 不判断 `std::func`、`std::struct`、`std::module`、`import`、类型应用或 effect 的含义。Backend 不重新解释这些源语言概念。

例如：

```lain
value(arg)
```

RawAst 只记录一个表达式后面跟着圆括号组。Meta 查询 `value` 的绑定后，才能判断这是运行时调用、类型工厂、宏调用、effect 构造或其他库定义形式。

## 3. Meta 负责什么

Meta 和标准库负责：

- AST 查询、复制、替换和删除；
- 宏与 attribute 展开；
- 统一绑定和库定义形式的识别；
- 名字解析和作用域规则；
- 类型与 effect 检查；
- comptime 参数传播、closure specialization 和泛型工厂具体化；
- record layout、closure conversion 和 ABI 决策；
- module、import 和 export 解析；
- 诊断及其源码位置；
- 从已解释的程序生成 LAINIR；
- 在需要计算时生成 `#eval`。

Meta 不负责：

- 执行另一套源语言表达式解释器；
- 绕过 `#eval` 直接执行 Lain procedure；
- 把函数、类型、模块或 effect 固化成 Parser 节点；
- 把未消解的高层语义带入 LAINIR；
- 隐式访问文件、进程、环境变量或网络。

## 4. 三个标准阶段

标准库通过三个阶段向 compiler core 提供语言行为。

### 4.1 `expand`

`expand` 接收源码单元和 RawAst 根，返回展开后的 AST。它负责会产生或改写语法的规则，例如：

- 宏调用；
- attribute 展开；
- 派生声明；
- 库定义的语法糖。

展开必须保留 source origin 和 hygiene 信息。阶段失败时返回诊断，不得留下可继续编译的半成品结果。

### 4.2 `elaborate`

`elaborate` 把展开后的语法解释成已检查的语言对象。它负责：

- 建立 binding、scope 和 module 环境；
- 解析名字和成员；
- 建立类型、callable、effect 和编译期值；
- 检查调用参数、返回值和 effect；
- 完成所需的特化和布局计算。

阶段结果可以是 AST 句柄、语义对象句柄或两者组合。ABI 不规定标准库内部必须存在一种叫作 `Middle AST` 的固定数据结构。

### 4.3 `lower`

`lower` 把已解释的语言对象转换成 LAINIR。它负责：

- 选择物理类型和过程签名；
- 生成控制流、调用、地址计算、load 和 store；
- 把 module 和 export 转换成物理符号关系；
- 为编译期计算生成 `#eval`；
- 消除宏、泛型、effect 集和其他只存在于源语言的结构。

lowering 完成后，LAINIR 不再依赖 Meta 才能理解其物理执行含义。

## 5. Bootstrap Standard Library ABI v1

bootstrap LAINIR 标准库和正式 Lain 标准库实现同一组入口：

```text
lain_std_abi_version() -> usize
lain_std_initialize(context) -> status
lain_std_expand(context, source_unit, root) -> MetaPassResult
lain_std_elaborate(context, expanded_root) -> MetaPassResult
lain_std_lower(context, elaborated_root, l1_unit) -> MetaPassResult
```

compiler core 按以下顺序调用：

```text
initialize
  -> check ABI version
  -> expand
  -> elaborate
  -> lower
  -> publish artifact
```

每个 `MetaPassResult` 都带有所属 compile context 的 owner。compiler core 在使用结果前检查 owner 和 status，并在阶段交接后释放上一阶段结果。

ABI 只交换通用句柄、状态、诊断、依赖和变更信息。它不会出现 `make-function`、`make-struct` 或 `make-module` 这样的语言专用 host API。

## 6. Compiler core 提供的底座

compiler core 只提供普通库无法自行实现的机制：

```text
SourceApi
  source bytes, path, package identity, import input

AstApi
  node topology, text, span, syntax context
  copy, replace, remove, fresh symbol, attach origin

IrApi
  create unit, procedure, region, expression and instruction
  verify and print

LainVmApi
  execute a verified root procedure or child procedure
  return a physical value or propagate Trap
  # LAINIR owns the Eval effect; the VM contributes execution primitives only

DiagnosticApi
  record code, severity, span and message

ArtifactApi
  publish compiler outputs and dependency information
```

这些 API 提供数据和操作，不定义 Lain 的高级语言规则。

验证一项能力是否真的属于标准库，可以替换标准库 artifact 而保持 compiler core 不变。如果语言行为随标准库规则改变，这条边界就是有效的。

## 7. 编译期执行：`#eval`

`#eval` 是唯一的编译期执行边界：

```text
source AST
  -> Meta interprets language rules
  -> Meta generates LAINIR containing #eval
  -> LAINIR verifier checks the block
  -> LAINVM executes it through a temporary TCB
  -> declared LAINIR value returns to the compilation flow
```

例如：

```lain
let main = std::func() -> i64 {
    return 40 + 2;
};
```

可以 lower 为：

```lain-ir
#proc main() -> #bits<64> {
  #let %value: #bits<64> = #eval {
    #return #add(40, 2)
  }
  #return %value
}
```

Meta 生成 `#add` 和 `#eval`。LAINVM 执行加法。Meta 不需要实现第二套整数表达式求值语义。

`#eval` 正常完成时返回其声明的 LAINIR 物理值；执行失败时由 LAIN-VM 产生 Trap。LAIN-VM 不判断这个物理值在 Meta 中代表整数、类型、模块还是 AST，也不为它添加对象类别、资源归属或代际信息。

`Eval` 是 LAINIR 的概念：LAINIR 定义并验证「这段已 lowering 的代码在编译期执行」，
LAINVM 只提供执行所需的原语（过程入口、VSpace、预算、Trap）。因此 `Eval` effect 属于
LAINIR 契约，不属于 VM 契约；编译器边界安装它的 handler。见
[`04-lain-vm.md`](04-lain-vm.md) §8.4。

Meta 为 `#eval` 提供静态预期类型，并允许块引用外围的物理局部值。lowering 把每个自由
`%local` 转换为临时根过程的按值参数；这不是把 Meta frame、类型对象或 AST 对象交给
VM。若捕获值是 `#addr`，Meta 也不能借此延长该地址原有区域的生命周期。

一次失败的 `#eval` 没有普通值可供 Meta 检查。VM 将 Trap 交给编译器的诊断路径，当前
Meta 阶段停止处理该计算；不得用 `status` 字段、空值、对象 handle 或额外结果包装把失败
伪装成值。

## 8. AST 操作与 hygiene

Meta 通过稳定的 AstApi 操作 opaque AST handle。它不依赖宿主内存布局。

基本操作包括：

- 读取 node kind、delimiter、文本、span 和 syntax context；
- 遍历 child、sibling 和 parent；
- 创建 atom 或 group；
- 复制、替换或删除节点；
- 创建 fresh symbol；
- 把生成节点关联回调用位置和定义位置。

宏展开生成的 identifier 必须携带正确的 syntax context。普通复制保留已有上下文；宏内部临时名字使用 fresh symbol；有意引用调用方名字时必须通过明确的 capture 操作表达。

AST 操作的实现说明见 [`implementation/ast-operations.md`](implementation/ast-operations.md)，语法树契约见 [`02-lain-ast.md`](02-lain-ast.md)。

## 9. 语言对象

以下概念由 Meta 库定义：

```text
binding
callable
type
record
module
signature
interface
effect
attribute
macro
comptime value
```

它们可以由普通构造器产生。例如：

```lain
let add = std::func(left: i32, right: i32) -> i32 {
    return left + right;
};

let Pair: type = std::struct {
    left: i32,
    right: i32,
};

let math = std::module {
    // members
};
```

Parser 只看到统一绑定和 initializer 的拓扑。标准库查询 initializer 的绑定，调用对应的 Meta 构造器，并产生语言对象。

## 10. 类型、泛型和 specialization

类型是编译期值。类型参数通过普通的编译期函数或工厂参数传递：

```lain
let identity = std::func(comptime T: type, value: T) -> T {
    return value;
};

let answer = identity(i32, 42);
```

Meta 负责：

- 给类型值稳定的语义身份；
- 绑定 `T` 并检查 `value`；
- 按实参组合查找或创建 specialization；
- 把 specialization 传播到 closure 和被调用过程；
- 在 physical lowering 前确定参数、返回值和数据布局。

生成的 LAINIR 不包含未绑定的 Meta 类型参数。每个物理过程必须具有确定的签名。

## 11. Module、import 和 artifact

module 是 Meta 命名空间对象。import 解析源码或 package 输入并返回 module 引用。export 决定模块边界上可见的名字和接口 artifact。

Meta 负责：

- 构建 module member 和 export 表；
- 解析 qualified name；
- 检查 import 是否存在以及依赖是否成环；
- 建立稳定的物理符号名；
- 记录依赖和接口 artifact。

LAINIR 中不保留高层 module 对象。lowering 后只留下物理 procedure、数据、extern 和链接信息。

## 12. Effect 与 capability

effect 是 Meta 层语义。Meta 负责构造 effect 值、检查 effect 集并选择 lowering 规则。

编译期执行需要宿主副作用时，必须显式请求 capability，例如文件读取、artifact 写入或外部工具调用。capability 由 driver 注入，并受当前编译请求的 policy 和资源限制约束。

语法展开本身不会因为运行在编译期就自动获得宿主权限。详细设计见 [`stdlib/effect-system.md`](stdlib/effect-system.md)。

## 13. 诊断与失败原子性

Meta 诊断至少应包含：

```text
stable code
severity
primary span
message
optional notes and related spans
owner
```

每个阶段遵守失败原子性：

- 失败结果不暴露可发布的部分 LAINIR artifact；
- 临时 AST、语义对象和求值结果仍按 owner 释放；
- compiler core 传播标准库返回的稳定诊断；
- ABI 版本或 owner 不匹配在继续处理前失败。

这里的 owner 描述编译器运行时资源归属，不是 Lain 源语言的所有权或借用系统。

## 14. 当前实现状态

当前代码已经具备：

- compiler core 与 bootstrap stdlib 的物理分包；
- `lain_std_initialize`、`expand`、`elaborate`、`lower` ABI；
- RawAst 读取、复制、替换、删除和基础 hygiene；
- module、record、type、binding、call 和编译期求值的 bootstrap 实现；
- 正式 `std::meta` 编译为 LAINIR artifact 的路径；
- 通过显式 `#eval` 执行算术的最小 lowering；
- pass result 的 owner、status 和释放检查。

当前仍在迁移：

- 正式标准库尚未覆盖完整 Lain 语义；
- `elaborate` 入口已经存在，但完整类型、泛型、effect 和 module 语义仍需接入；
- `lower` 当前只接受已迁移的可执行子集，未覆盖形式返回稳定诊断 `5203`；
- `src/lainc` 尚未完成完整自举固定点；
- ownership/borrow checking 不属于当前必需自举路径。

进度和验收门槛见 [`roadmaps/lain-roadmap.md`](roadmaps/lain-roadmap.md)。

## 15. 边界总结

```text
RawAst owns source topology.
Meta owns language meaning and transformation.
LAINIR owns physical verification and execution.
Backend owns target emission.
```

Meta 通过标准库实现语言规则，通过稳定 ABI 与 compiler core 交接，通过 `#eval` 请求编译期执行。任何新增语言能力都应先判断它属于语法拓扑、语言语义、物理执行还是目标生成，再放入对应层。
