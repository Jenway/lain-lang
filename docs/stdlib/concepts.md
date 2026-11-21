# 泛型机制（Concept）与编译期契约

Lain-Lang 编译器内核不内置专有的泛型类型系统（不内置 `trait` / `interface` / `impl` / `given`）。所有的参数化多态、类型构造与接口约束，均作为普通编译期值、`module` 关联字典与纯 Meta 函数实现。

> 说明：全文源码示例遵循现代 Lain 零裸指针规则（使用 `Ref(T)`、`Mut(T)`、`Slice(T)`）与统一 `let` 声明语法；仅在 1.4 节描述降解产物时涉及 LAINIR 原生类型。

---

## 1. 核心泛型与 Concept 机制

### 1.1 签名结构与三区正交解耦

函数通过统一 `let` 声明定义，其签名将不同阶段与控制流性质严格分离为三个正交参数区域：

```lain
let fn_name = func(runtime_params...)
  ?{ comptime_params... }
  !{ runtime_effects... }
  -> ReturnType
{
  // 函数体
};
```

```ebnf
FuncExpr ::= 'func' '(' ParamList? ')'
             ('?' '{' ComptimeParamList? '}')?
             ('!' '{' EffectList? '}')?
             ('->' TypeExpr)?
             Block
```

#### 三个区域的职责边界
1. **运行期输入 `(...)`**：程序在目标机器运行期消费的物理值，进入寄存器或调用栈；
2. **编译期输入 `?{ ... }`**：描述单态化特化所需的编译期值、类型与 Witness 约束。该区域在编译期完成求值、类型实参推导与约束核验，在降解时**完全消除**，不占用运行时栈帧；
3. **运行期效应 `!{ ... }`**：声明该过程在执行期可能发起的非局部控制流效应（详见 [effects.md](effects.md)），缺省即纯控制流。

#### 编译期值平权
`?{ ... }` 内部不区分专有的“泛型类型参数”与“值参数”，所有条目均为地位平等的编译期值：
* **类型元数据**（如 `T: Type`）；
* **整数与切片常量**（如 `N: u64`、`fmt: Slice(u8)`）；
* **操作见证与策略对象**（如 `ord: Ord(T)`、`layout: LayoutPolicy`）。

---

### 1.2 一等类型 (`Type`) 与类型工厂

#### 1. 一等类型值
在编译期环境中，类型是具象的不可变值（元类型为 `Type`）。每个 `Type` 实例携带完整物理布局元数据：
* 物理字节尺寸（`size_of(T)`）；
* 物理对齐约束（`align_of(T)`）；
* 结构体字段表（`fields_of(T)`：每项含字段名 `f.name` 与字段类型 `f.type`）。

#### 2. 类型工厂的两种构造形式
根据结构是否固定，类型工厂可以用两种零字符串拼接的方式直接构造新 `Type`：

1. **声明式结构体表达式 `return struct { ... };`**（结构固定时最简写法，适合定义容器或 Concept 字典）：
   ```lain
   let Opt = func(T: Type) -> Type {
     return struct {
       present: bool,
       value: T,
     };
   };
   ```
2. **指令式构造原语 `record_begin / record_add / record_end`**（配合 `comptime for` 动态反射任意结构 `T` 时使用，如 AoS $\to$ SoA 转换）：
   ```lain
   let SoA = func(T: Type, N: u64) -> Type {
     let b = record_begin();
     comptime for f in fields_of(T) {
       record_add(b, f.name, Array(f.type, N));
     }
     return record_end(b);
   };
   ```

#### 3. 类型同一性与记忆化 (Memoization)
为保证跨函数、跨模块的符号修饰与类型等价判断具备全局确定性，编译器对类型工厂执行纯函数记忆化：
* **纯度约束**：类型工厂严禁访问外部可变状态或执行任何 I/O；
* **单例缓存**：编译器以 `(工厂符号, 规范化编译期实参元组)` 为键维护全局缓存。在任意位置两次调用 `Opt(i64)`，返回完全相同的 `Type` 标识；
* **规范命名**：降解后的物理结构与单态化过程采用规范修饰名（如 `Opt$i64`）。

---

### 1.3 Concept 约束系统与基于 `module` 的 Witness 传递

Lain-Lang 遵循前置契约原则：**泛型算法所需的一切类型能力必须在 `?{ ... }` 签名处显式断定，严禁把鸭子类型错误拖进函数体展开后再报错。**

#### 1. Concept 规范定义
Concept 不是内核关键字，只是**返回操作见证结构体（或函数指针记录）的普通类型工厂**（也可写作模块签名 `sig`）：

```lain
let Ord = func(T: Type) -> Type {
  return struct {
    less: Fn((T, T) -> bool),
    equal: Fn((T, T) -> bool),
  };
};
```

#### 2. 消费端泛型函数：自动推导与签名级校验
```lain
let max = func(a: T, b: T)
  ?{
    T: Type,
    ord: Ord(T),
  }
  -> T
{
  return if ord.less(a, b) { b } else { a };
};
```
* **前置契约校验**：Meta 处理调用点时，先从实参类型推导出 `T`（例如 `Vec2`），立即校验 Witness `ord: Ord(T)` 是否完整且函数签名精确匹配；失败则直接在调用点给出签名级报错，不进入函数体展开；
* **显式局部调用**：函数体内统一通过 `ord.less(a, b)` 显式调用，无全局方法名污染。

#### 3. Witness 的三条来源路径（无需 `given` 关键字）
按照 [modules.md](modules.md) 确立的统一规则，调用点省略 `?{ ... }` 或显式覆盖 Witness 时遵循：

1. **基于 `module { pub let Self = T; }` 自动合成**：
   若当前词法作用域内可见某个导出了 `pub let Self = Vec2;` 且包含公开函数 `less`、`equal` 的模块 `Vec2Ord`（或通过 `use IntOrd;` 在局部作用域激活的模块），调用 `max(v1, v2)` 时 Meta 自动检索并打包生成 `Ord(Vec2)` Witness 字典：
   ```lain
   let Vec2 = struct { x: i64, y: i64 };

   let Vec2Ops = module {
     pub let Self = Vec2;
     pub let less = func(a: Vec2, b: Vec2) -> bool { return a.x < b.x; };
     pub let equal = func(a: Vec2, b: Vec2) -> bool { return a.x == b.x and a.y == b.y; };
   };

   let top: Vec2 = max(v1, v2); // 自动命中 Vec2Ops，合成 Ord(Vec2) Witness
   ```
   若同作用域下存在多个导出同一 `Self` 与同一接口的模块造成歧义，或完全找不到，编译期立即报错。
2. **显式传入覆盖模块 `?{ ord: CustomMod }`**（完美绕开孤儿规则，同一类型可随时切策略）：
   ```lain
   let ReverseI64 = module {
     pub let Self = i64;
     pub let less = func(a: i64, b: i64) -> bool { return a > b; };
     pub let equal = func(a: i64, b: i64) -> bool { return a == b; };
   };

   let smallest: i64 = max(10, 20) ?{ ord: ReverseI64 }; // 返回 10
   ```
3. **直接构造 Witness 值传入 `?{ ord: Ord(i64) { ... } }`**（适用于一次性匿名策略或闭环字典传递）：
   ```lain
   let rev_ord = Ord(i64) { less: gt_i64, equal: eq_i64 };
   let smallest: i64 = max(10, 20) ?{ ord: rev_ord };
   ```

---

### 1.4 具体化与 LAINIR 降解契约 (Lowering)

在 Meta 的 Lower 阶段，泛型参数、类型值与 Witness 字典全部消除：

1. **单态化特化（Monomorphization）**：
   * 每个唯一的 `(函数符号, 编译期实参组合)` 降解为一条独立的物理 `#proc`；
   * 所有 `?{ ... }` 参数从物理签名彻底消失，只保留运行期形参 `(...)` 与返回值。
2. **Witness 去虚拟化与内联**：
   * 当 `ord` 字段在编译期已知指向具体函数时，`ord.less(a, b)` 直接降解为静态直接 `#call`（乃至后续内联为单个 `#slt` 指令），**无任何运行时函数指针表查表开销**。
3. **物理布局映射**：
   * `Ref(T)` / `Mut(T)`、`Slice(T)`、`i64`/`u64` 映射为 LAINIR `#bits<N>`、`#float<N>`、`#addr`（仅切片长度随包携带）；结构体成员按编译期布局偏移降解为 `#lea` 计算。

---

## 2. 典型应用范例

### 2.1 范例一：零堆分配惰性格式化代理 (Lazy Format Proxy)

利用纯 Meta 宏与 `?{ ... }` 编译期参数，无需编译器特权宏即可实现零分配格式化（脱糖规则见 [desugaring.md](desugaring.md)）：

```lain
// 用户源码
io.print(f"total: {count}, ok: {status}");

// Expand 阶段由 AstOut 原位重写为瞬时栈代理
io.print(
  std.fmt.bind((count, status)) ?{ fmt: "total: {}, ok: {}" }
);
```

消费端签名：

```lain
let format_to = func(item: Item, sink: Mut(Sink))
  ?{
    Item: Type,
    Sink: Type,
    fmt: Formattable(Item),
    out: Writable(Sink),
  }
  !{ IoError }
{
  fmt.write_to(ref_of(item), sink, out)!;
};
```
* **降解实况**：代理结构与 `?{}` Witness 在单态化期间全部扁平消除，最终只产生连续向目标缓冲写出字节的直行指令序列，零中间字符串堆分配。

---

### 2.2 范例二：策略对象（Policy）与编译期内存布局控制

```lain
let LayoutPolicy = func() -> Type {
  return struct {
    compute_stride: Fn((u64, u64) -> u64),
  };
};

let row_stride = func(rows: u64, cols: u64) -> u64 { return cols; };
let col_stride = func(rows: u64, cols: u64) -> u64 { return rows; };

let RowMajor = LayoutPolicy() { compute_stride: row_stride };
let ColMajor = LayoutPolicy() { compute_stride: col_stride };

let Matrix = func(T: Type, Rows: u64, Cols: u64, Policy: LayoutPolicy()) -> Type {
  let stride: u64 = Policy.compute_stride(Rows, Cols);
  return struct {
    stride_val: u64,
    data: MutSlice(T),
    cells: Array(T, Rows * Cols),
  };
};

// 编译期特化：行优先 4x4 双精度矩阵
let Mat4 = Matrix(f64, 4, 4, RowMajor);
```
* **降解实况**：`Rows * Cols` 与跨步策略在编译期求值折叠完毕，生成的物理类型不含多余元信息开销。
