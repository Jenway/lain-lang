# 泛型机制（或称 concept）与编译期契约

TO BE DONE in the future

Lain-Lang 会在标准库里实现这一高级特性。

Lain-Lang 编译器内核不内置专有的泛型类型系统。所有的参数化多态、类型构造与接口约束，均作为普通编译期值与纯 Meta 函数在标准库层面实现。

## 1. 核心泛型与 Concept 机制 (Mechanism)

### 1.1 签名结构与阶段解耦
函数签名将不同执行阶段的输入与效应严格分离为三个正交的参数区域：

```plain
let func = std::func(runtime_params...)
    ?{ comptime_params... }
    !{ runtime_effects... }
    -> ReturnType
{
    // 函数体
}
```

```ebnf
FunctionDecl ::= 'let' Ident '=' 'std::func' '(' ParamList? ')' 
                 ('?' '{' ComptimeParamList? '}')?
                 ('!' '{' EffectList? '}')?
                 ('->' TypeExpr)?
                 Block
```

#### 区域职责与阶段规则
1. **运行时输入 `(...)`**：描述程序在机器运行期消费的物理值，占用目标平台的寄存器或调用栈；
2. **编译期输入 `?{ ... }`**：描述单态化特化所需的编译期值及其约束。该区域在编译期完成求值并完全消除，不占用运行时栈帧；
3. **运行时效应 `!{ ... }`**：声明该过程在执行期可能触发的非局部控制流（如挂起、中止），由外层效应处理器（Handler）拦截。

#### 编译期值平权
`?{ ... }` 内部不设立专有的“泛型类型参数”类别，所有条目均为地位平等的编译期值：
* **类型元数据**（如 `T: Type`）；
* **整型常量**（如 `N: usize`）；
* **操作见证与策略对象**（如 `cmp: Comparator(T)`、`layout: LayoutPolicy(T)`）。

---

### 1.2 类型模型与类型工厂

#### 1. 一等公民类型 (`Type`)
在编译期环境中，类型是具象的不可变值（元类型标记为 `Type`）。每个 `Type` 实例包含该类型的物理布局元数据：
* 物理字节尺寸（Size）；
* 物理对齐约束（Alignment）；
* 字段物理偏移表（Field Offsets）。

#### 2. 类型工厂 (Type Factory)
复合类型（如容器、智能指针、元组）由编译期纯函数直接生成：

```lain
let Vec = fn(T: Type) -> Type {
    return std::meta::define_struct(?{
        data: #addr,
        len: usize,
        cap: usize,
    });
};
```

#### 3. 类型同一性与记忆化 (Memoization)
为保证类型判断与符号命名的全局确定性，编译器对类型工厂执行严格的纯函数记忆化规则：
* **纯度约束**：类型工厂函数严禁读取外部可变状态或非确定性环境输入；
* **单例缓存**：编译器以 `(工厂函数符号, 规范化参数元组)` 为键建立全局单例表。两次传入相同参数调用 `Vec(i32)`，必返回物理标识完全相同的同一个 `Type` 实例；
* **符号修饰（Name Mangling）**：降解生成的物理结构体符号按 `工厂名$参数名` 规范化编码（例如 `@Vec$i32`）。

---

### 1.3 Concept 约束系统
Lain-Lang 遵循 C++20 Concepts 的前置契约哲学：**所有类型约束必须在接口签名处显式断定，严禁鸭子类型错误穿透至函数体内部。**

#### 1. Concept 规范定义
Concept 是操作见证规范的集合，定义一组类型必须具备的能力：

```lain
let Ord = std::concept(T: Type) {
    less(a: T, b: T) -> #bits<1>;
    equal(a: T, b: T) -> #bits<1>;
};
```

#### 2. Witness 传递语义
在泛型函数内部，Concept 约束体现为编译期传入的操作见证对象（Witness）：

```lain
let max = std::func(a: T, b: T)
    ?{
        T: Type,
        ord: Ord(T),
    }
    -> T
{
    return ord.less(a, b) ? b : a;
};
```

* **校验时机**：Meta 在细化阶段处理调用点时，首先对 `?{ ... }` 内的实参执行类型检查。若传入的 `T` 未满足 `Ord(T)`，立即在调用点终止编译并输出签名级诊断，函数体内部代码不参与展开；
* **显式分派**：函数体内通过 `ord.less(a, b)` 显式发起操作，消除全局隐式查找与方法名污染；
* **见证构造途径**：
  * **自动合成**：若调用方未显式传入 `ord`，Meta 在调用点尝试根据 `T` 已注册的能力自动合成见证字典；
  * **显式覆盖**：调用方可显式传入自定义的 Witness（如逆序比较器 `ReverseOrd(T)`），实现算法行为的编译期定制。

---

### 1.4 具体化与 LAINIR 降解契约 (Lowering)

在 Meta 的降解（Lower）阶段，泛型与编译期参数被完全抹平：

1. **单态化特化（Monomorphization）**：
   * 每个独立的 `(函数符号, 编译期实参组合)` 降解为一个独立的物理 LAINIR 过程；
   * 签名中的所有 `?{ ... }` 参数被彻底移除，仅保留运行时参数 `(...)` 与运行时物理返回值；
2. **Witness 完全内联**：
   * 类似 `ord.less(a, b)` 的调用直接内联为具体的底层物理指令（如 `#slt`）或特定过程的直接调用，**不存在运行时虚表查找开销**；
3. **物理类型映射**：
   * 具象类型映射为 LAINIR 原生标量（`#bits<N>`、`#float<N>`、`#addr`）；
   * 复合结构体成员寻址降解为基址与固定字节偏移的 `#lea` 算术计算。

---

## 2. 典型应用范例 (Application Patterns)

以下范例展示如何利用上述机制解决具体的系统级工程问题。

### 2.1 范例一：零分配惰性格式化代理 (Lazy Format Proxy)

该模式展示如何利用纯 Meta 宏与 `?{}` 编译期参数，在完全不依赖编译器内置特权宏的前提下，实现零堆分配的字符串插值。

#### 1. 展开模式 (Expand Phase)
用户编写的高级插值语法，在展开阶段由库宏 `f` 原地改写为瞬时栈代理结构体：

```lain
// 用户源码
print(f"total: {count}, ok: {status}");

// f 宏原地重写为未装箱代理结构体
print(
    std::fmt::bind((count, status)) ?{ fmt: "total: {}, ok: {}" }
);
```

#### 2. 消费端签名与 Concept 约束
```lain
let print = std::func(item: Item)
    ?{
        Item: Formattable,
        sink: Writable,
    }
    !{ async, error(IoError) }
{
    // 流式直写，无中间 String 堆分配
    item.write_to(sink);
};
```

* **降解实况**：`std::fmt::bind` 生成的对象仅包含变量引用的栈上元组，单态化后该结构体被 Meta 完全优化消除，直接降解为向 `sink` 的连续物理写入指令序列，无任何中间堆内存分配。

---

### 2.2 范例二：策略模式与编译期内存布局控制

该模式展示如何将策略对象（Policy）与数值常量平权传入 `?{}`，直接控制数据结构的物理存储布局。

```lain
let Matrix = fn(T: Type, Rows: usize, Cols: usize, Layout: StorageOrder) -> Type {
    return std::meta::define_struct(?{
        // 根据 Layout（行优先/列优先）在编译期计算物理对齐与跨步步长
        stride: usize = Layout.compute_stride(Cols, sizeof(T)),
        data: #addr,
    });
};

// 使用示例：声明一个 4x4 行优先的双精度浮点矩阵
let mat: Matrix(f64, 4, 4, RowMajor);
```

* **降解实况**：`Rows`、`Cols` 与 `Layout` 在编译期完成全部乘法与对齐计算，降解生成的 LAINIR 结构体仅包含计算完成的字面量常数与裸物理地址，无额外存储开销。
