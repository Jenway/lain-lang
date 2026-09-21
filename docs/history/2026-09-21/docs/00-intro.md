> 历史记录。原路径：`docs/00-intro.md`。归档日期：2026-09-21。
> 本文保留整理前的内容；其中的状态、命令、语法和结论不作为现行依据。
> 当前文档从 [文档索引](../../../README.md) 阅读。

# Lain 概览与架构

> [!QUOTE]
> Language Experiment Lain Lang
>
> 我们能否让编译器本体尽量的精简，而让所有的高级语言特性都以库的方式实现？
>
> 我们希望它的 meta 功能尽可能强大，这里的 meta 指的是在编译期间对于 AST 的操作变换。
>
> 我们希望它的底层中间表示（也就是 Lain-IR）尽可能简洁而忠实反映现代的 CPU。

Lain 是一个原生语言实验。它研究的问题是：编译器能否只保留稳定的通用底座，把函数、类型、模块、泛型、effect 等语言规则放进可替换、可自举的标准库 Meta 层。

这里的 **Meta** 指编译期间对语法树和语言对象进行解释、检查与变换。**LAINIR** 是 Meta 最终生成的物理执行层中间表示。

## 1. 设计原则

Lain 当前遵循四条原则：

- Parser 只建立源码拓扑，不识别函数、类型、模块等语言含义。
- Meta 和标准库定义高层语言规则，并负责展开、语义处理和 lowering。
- 编译期程序由 Meta 生成 LAINIR `#eval`，再由 LAINVM 执行。
- Backend 只消费已经完成 lowering 的 LAINIR。

编译器核心提供通用机制：源码和语法树访问、阶段调度、诊断、资源归属、artifact 输出以及 capability。

## 2. 编译流程

当前主线的编译流程是：

```text
Lain source
  -> RawAst
  -> Meta expand
  -> Meta elaborate
  -> Meta lower
  -> LAINIR
  -> verify and execute #eval
  -> remaining runtime LAINIR
  -> interpreter or native backend
```

其中：

| 阶段 | 负责的事情 |
| --- | --- |
| RawAst | 保存 token、分隔符分组、相邻关系和源码位置 |
| expand | 展开宏和其他产生新语法的规则 |
| elaborate | 解释语言对象，完成名字、类型、effect、特化和布局等语义工作 |
| lower | 把高层语言对象转换成 LAINIR 的过程、控制流、地址和物理操作 |
| `#eval` | 执行明确标记为编译期计算的 LAINIR |
| Backend | 解释 LAINIR，或把它转换成 C、LLVM IR、机器码等目标 |

`expand`、`elaborate`、`lower` 是标准库向 compiler core 提供的阶段 ABI。当前实现仍在自举迁移中，部分 `elaborate` 语义尚未完整落地；这张表描述稳定的职责边界，不表示每项语言能力都已经完成。

## 3. LAIN-AST 边界

Parser 产生的 RawAst 只描述源码的结构拓扑。它只有两种节点：

```text
Atom
Group
```

它不定义 `Prefix`、`Postfix`、`Infix`、`Juxt`、`Sep` 这类运算符或相邻关系节点：
识别它们要求 Parser 预先知道运算符类别、优先级和结合性，这些规则属于标准库
Meta。它也不直接保存这些语义节点：

```text
function
struct
module
import
effect
type
call
field
parameter
statement
generic
attribute
macro
```

例如：

```lain
foo(x)
```

在 RawAst 中只表示为一个名字后面跟着圆括号组。它可能是运行时调用、类型工厂调用、effect 应用、宏调用或 DSL 形式。Meta 根据绑定和上下文决定它的含义。

完整的语法树契约以 [`02-lain-ast.md`](02-lain-ast.md) 为权威来源。

## 4. Meta 边界

Meta 是 Lain 的语言定义层，负责：

- 解释统一绑定以及 `std::func`、`std::struct`、`std::module` 等构造器；
- 宏和 attribute 展开；
- 名字解析、类型检查和 effect 检查；
- comptime 参数传播和具体化；
- record layout、closure conversion 和 ABI 决策；
- 生成 LAINIR。

Meta 可以维护类型、模块、callable、effect 和编译期值等高层对象。这些对象在 lowering 后只留下运行所需的物理表示。

当前 bootstrap 标准库使用 LAINIR 实现这套阶段 ABI，正式标准库使用 Lain 编写并编译成 LAINIR。两者应遵守同一接口。Meta 的实现语言不改变它在编译流程中的职责。

详细规则见 [`03-meta-system.md`](03-meta-system.md)，当前实现路线见 `roadmaps/lain-roadmap.md`（原引用：`roadmaps/lain-roadmap.md`）。

## 5. 编译期执行边界

Meta 决定需要执行什么编译期计算，并生成显式的 LAINIR `#eval` 块。LAINVM 负责执行这个块。

例如：

```lain
let main = std::func() -> i64 {
    return 40 + 2;
};
```

当前算术 lowering 可以生成：

```lain-ir
#proc main() -> #bits<64> {
  #let %value: #bits<64> = #eval {
    #return #add(40, 2)
  }
  #return %value
}
```

这里有两条边界：

- Meta 把 `std::func`、`i64` 和 `+` 解释并转换成 `#proc`、`#bits<64>` 和 `#add`。
- LAINVM 执行 `#eval`，把结果交还给后续编译过程。

编译期文件、进程、网络或 artifact 操作必须通过显式 capability 获得宿主能力。Meta 语义对象本身不隐式获得这些能力。

## 6. LAINIR 边界

LAINIR 表达物理执行，包括：

- 固定位宽整数、浮点数、地址、unit 和 never；
- 算术、比较和位宽转换；
- 局部绑定、分支和循环；
- 地址计算、load 和 store；
- 物理过程、直接调用、间接调用和外部过程；
- 显式的编译期 `#eval`。

进入 LAINIR 前，高层函数、结构体、模块、泛型、effect、宏、重载和名字解析都应当完成处理。

LAINIR 的规范见 [`01-lain-ir.md`](01-lain-ir.md)。

## 7. 高层 callable 与物理过程

Lain 中由 `std::func` 构造的 callable 是 Meta 对象。LAINIR 中的 `#proc` 是一个具有固定物理签名的过程。

一个 callable 经过展开、特化和 lowering 后，可能产生：

- 一个 `#proc`；
- 多个具有不同物理签名的 `#proc`；
- closure 数据和对应的 invoke 过程；
- foreign wrapper 或 trampoline；
- 完全内联的代码；
- 只在编译期存在、没有运行时产物的过程。

因此，callable 的身份和类型属于 Meta 层，`#proc` 的参数宽度、返回宽度和调用约定属于 LAINIR 层。

## 8. 类型参数与 comptime

类型是编译期值。带类型参数的构造器使用普通的编译期参数和工厂调用，例如：

```lain
let identity = std::func(comptime T: type, value: T) -> T {
    return value;
};

let answer = identity(i32, 42);
```

`Vec(i32)`、`Result(i32, Error)` 和普通的 `foo(x)` 在 RawAst 中可以具有相同的后缀调用拓扑。Meta 根据被调用对象决定这是类型构造、effect 构造、宏展开还是值调用。

具体化完成后，类型参数必须传播到物理 lowering，使每个生成的 LAINIR 过程和数据布局都具有确定的物理形式。

## 9. Effect 和模块

effect 和 module 都是 Meta 层的语言对象。

- effect 规则负责描述和检查能力集合，并在 lowering 时选择错误返回、handler、状态机或其他物理实现。
- module 规则负责名字空间、import、export 和接口 artifact，并在 lowering 时留下物理链接所需的信息。

它们不会作为未经处理的高层对象进入 LAINIR。effect 与 capability 的详细设计见 [`stdlib/effect-system.md`](stdlib/effect-system.md)。

## 10. Backend

Backend 接收经过验证的 LAINIR。它可以直接解释执行，也可以生成 C、LLVM IR 或机器码。

Backend 只处理物理类型、控制流、内存和调用。函数构造器、结构体语法、模块、泛型、effect 和宏已经由 Meta 层处理完毕。

当前后端说明见 `implementation/lain-written-backend.md`（原引用：`implementation/lain-written-backend.md`）。已归档的 LAINIR 工具说明见 `history/lainir-tools.md`（原引用：`history/lainir-tools.md`）。

## 11. 当前自举结构

当前自举代码分为三个位置：

```text
seed/                         C 编写的 LAINIR 解释器和 LAINIR 编译器源码
bootstrap/compiler/*.l1       手写的启动编译器源码
bootstrap/std/*.l1            手写的启动标准库源码
src/lainc/*.lain              Lain 编写的正式编译器源码
build/bootstrap/lainc.l1      生成的启动编译器 bundle
```

目标是让 compiler core 通过稳定 ABI 调用标准库的 `expand`、`elaborate` 和 `lower`，由正式标准库接管语言规则，并最终让 `src/lainc` 编译器完成自举固定点。
