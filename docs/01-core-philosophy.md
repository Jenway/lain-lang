# Lain 语言核心设计

## 设计哲学

**编译器只提供无法在库中实现的原语。** 几乎所有"语言特性"都是库，库是可读的、可替换的。

三条基本原则：
- **正交性**：特性之间不重叠，可以独立组合
- **显式优于隐式**：副作用、内存、控制流都可见
- **零成本抽象**：不用的特性不付代价

---

## 两层语言架构

Lain 最核心的创新：编译期语言和运行时语言是两种不同的语言。

```
层 1（编译器 / 运行时语言）
  - 暴露硬件：CPU 寄存器、指令、内存访问
  - 暴露 LLVM IR
  - 静态类型、效果系统、所有权转移语义
  - 程序员写业务逻辑的地方
  - 风格：类似 Rust，有类型标注

层 2（编译期语言 = Scheme 方言）
  - 完整的 Lisp，60 年的 macro 理论
  - 所有"语言特性"在这里实现
  - 操作层 1 的 AST
  - 风格：标准 Scheme + 内置编译器 API

桥梁：
  #{ ... }  quasiquote —— Scheme 里嵌入运行时代码
  ,( ... )  unquote     —— 切换回 Scheme 求值
  ,@( ... ) splice      —— 展开列表
```

判断标准：**如果一个特性需要编译器"理解"它才能正确工作 → 必须内置。如果只需要编译器提供足够的原语 → 可以是库。**

---

## 编译器必须内置的最小集合

### 类型系统原语

```
bits<N>            // N 个 bit，仅宽度，无符号无类型语义
addr               // 地址（本质是 u64，但语义上表示内存地址）
bool
struct { ... }     // 积类型
enum { ... }       // 和类型（tagged union）
fn(A) -> B         // 函数类型
!                  // never 类型，发散函数
()                 // unit 类型
```

说明：
- 汇编层面只有 bit 和操作，没有"有符号整数""无符号整数"之分。i32 / u32 是 meta 层给 bits<32> 贴上"默认用有符号/无符号操作集合"的标签。
- 汇编层面没有"指针"，只有地址和内存操作。层 1 只有 addr（本质是 u64）+ @load/@store。Ptr<T>、Ref<T>、Box<T> 是 meta 给 addr 附加语义。
- 没有 null —— 引入 addr 而非 `*T` 从根本上消灭了 null 的概念。Option<T> 是表达"可能没有值"的唯一方式。

### 内存原语

```
@load(addr, bits<N>, ordering) → bits<N>
@store(addr, val, ordering) → ()
@lea(base, index, scale, offset) → addr

// 原子操作（直接映射到硬件指令）
__atomic_load<T, Ord>(ptr) → T
__atomic_store<T, Ord>(ptr, val) → ()
__atomic_rmw<T, Ord, Op>(ptr, val) → T
__fence<Ord>() → ()

// 编译期布局信息
__size_of<T>() → usize
__align_of<T>() → usize
__offset_of<T>(field) → usize

// 所有权转移语义（编译器内置）
move / copy / drop
```

### SIMD 原语

```
Simd<T, N>              // 编译器内置类型，映射到 SIMD 寄存器
__vec_load / __vec_store
__vec_add / __vec_mul / __vec_fma
__vec_shuffle / __vec_gather / __vec_scatter
__vec_reduce_add / __vec_reduce_min / __vec_reduce_max
```

平台自动选择：程序员写 `Simd<f32, 8>`，编译器根据目标平台选择 256-bit AVX2 或 128-bit NEON × 2 或 SVE 自动适配。

### 位操作原语（直接映射到单条指令）

```
__popcount<T>(x) → u32       // popcnt / cnt
__clz<T>(x) → u32            // lzcnt / clz
__ctz<T>(x) → u32            // tzcnt / rbit+clz
__bswap<T>(x) → T            // bswap / rev
__rotate_left<T>(x, n) → T   // rol
__rotate_right<T>(x, n) → T  // ror
__pext<T>(x, mask) → T       // pext (x86 BMI2)
__pdep<T>(x, mask) → T       // pdep (x86 BMI2)
```

### 控制流原语

```
if / else / loop / break / continue / return
defer                         // 作用域退出，与所有权交互，必须内置
@tail_call foo(args)          // 保证生成 jmp，不是 call
@unreachable()                // 生成 ud2 / udf
@likely(cond) / @unlikely(cond)  // 分支预测提示
```

### FFI 原语

```
extern "C" fn foo(x: i32) → i32
extern "win64" fn ...         // Windows x64 ABI
extern "sysv" fn ...          // System V AMD64 ABI
extern "aapcs" fn ...         // ARM ABI
@asm("...")                    // 类型化内联汇编
#[repr(C)] / #[repr(packed)] / #[repr(align(N))]
```

### 调试和观测原语

```
__file__() / __line__() / __function__() / __column__()
__assert(condition, message)
__address_of(x) / __size_of<T>() / __align_of<T>()
```

### 代码生成控制

```
@inline(always) / @inline(never)
@optimize(size) / @optimize(speed)
@section(".hot") / @section(".cold")
@export / @hidden / @weak
```

### Meta 原语

```
comptime 求值（嵌入 Scheme）
#{ } quasiquote 桥梁
反射原语（fields_of / methods_of / 等）
```

---

## 编译器不内置（全部是库 / Scheme 宏）

| 特性 | 实现位置 |
|------|----------|
| i32 / u32 / f32 / f64 等类型 | `std/meta/core/types.scm` —— bits<N> + 操作集合选择 |
| interface / vtable / 动态分发 | `std/meta/interface.scm` |
| @derive(Clone, Debug, Json, ...) | `std/meta/derive/*.scm` |
| @effect / handler / resume | `std/meta/effects/base.scm` |
| Result<T, E> / Throws<E> | `std/meta/effects/throws.scm` |
| ? 操作符 | `std/meta/operators/question.scm` |
| async / await / Suspend | `std/meta/effects/suspend.scm` |
| defer | `std/meta/control/defer.scm` |
| panic (unwind 语义) | `std/meta/effects/panic.scm` |
| borrow checker | 不内置，可以是 meta 层的可选库 |
| RC / Arc / Arena / GC | 纯库，基于内存原语构建 |
| Mutex / Channel / RwLock | 纯库，基于原子原语构建 |
| Vec / HashMap / BTree | 纯库 |
| format! / println! | 纯库 |

### 所有权 / Borrow Checker

```
不是不重要，而是不强制一种策略

程序员可以选择：
  Arena / Region    一批对象共同生命周期
  RC / Arc          引用计数
  手动管理          自己保证，用 RawPtr 效果标注
  将来：可以靠 meta 系统实现 borrow checker 作为库

编译器提供：
  所有权转移语义（移动而非复制）
  RawPtr 效果（标注危险操作）
  但不做完整的生命周期分析
```

### 具体的并发原语

```
Mutex / Channel / RwLock / Semaphore
→ 都是库，基于内存序原语构建
```

### 具体的效果类型

```
Throws<E> / Suspend / IO / Alloc
→ 都是 @effect 宏定义的，不是编译器内置
```

---

## 关于指针：addr 而非 `*T`

汇编层面没有"指针"，只有地址（整数）和内存操作。

```
汇编世界：
  数据 = 整数 / 浮点 / 向量
  地址 = 也是整数，恰好用来寻址
  内存 = 一个大字节数组，用整数索引
  "指针"是高层语言发明的概念
```

**层 1 设计：没有 `*T`，只有 addr（u64）+ @load/@store。**

带来的好处：
1. **消灭 null**：没有指针类型，就没有 null。`Option<T>` 是表达"可能没有值"的唯一方式。十亿美元的错误在语言设计层面就不可能发生。
2. **类型系统诚实**：`Ref<T>` 永远有效，不存在"有效的 Ref<T>"和"无效的 Ref<T>"之分。
3. **清晰分层**：层 1 是忠实的硬件抽象；"指针"是 meta 层给 addr 附加的概念（Ptr<T>、Ref<T>、Box<T> 都是）。

---

## 类型家族的命名清晰分离

```
[T; N]          静态数组       栈/全局     编译期固定     普通内存访问
Simd<T, N>      SIMD 向量      寄存器      编译期固定     向量指令
DynArray<T>     动态数组       堆          运行时可变     普通内存访问
&[T]            切片（胖指针）  —           运行时已知     普通内存访问

常用 SIMD 别名：
type f32x4  = Simd<f32, 4>    // 128-bit，SSE / NEON
type f32x8  = Simd<f32, 8>    // 256-bit，AVX2
type f32x16 = Simd<f32, 16>   // 512-bit，AVX-512
type f32xN  = Simd<f32, auto> // 平台最优宽度
```

---

## 内存序作为类型系统的一部分

受 ARM 硬件设计哲学启发：**内存序是内存访问的固有属性，不是并发的附加物**。

### 带序引用类型

```lain
&Rlx<T>     // Relaxed，无顺序保证
&Acq<T>     // Acquire，读屏障
&Rel<T>     // Release，写屏障
&AR<T>      // AcqRel，读写屏障
&SC<T>      // SeqCst，全序
```

### 子类型关系

```
&SC<T> <: &AR<T> <: &Acq<T> <: &Rlx<T>   // 读方向
&SC<T> <: &AR<T> <: &Rel<T> <: &Rlx<T>   // 写方向
```

### 单线程代码完全透明

```lain
// 编译器证明局部变量不可能跨线程
// ordering 标注被完全优化掉
// 程序员感知不到任何差别

fn fibonacci(n: i32) -> i32 {
    if n <= 1 { return n }
    fibonacci(n-1) + fibonacci(n-2)
}
// 生成的汇编和 C 完全一样
```

### 跨线程时必须显式说明

```lain
// shared 声明共享意图
let shared counter: i32 = 0

// 函数签名里的效果说明访问强度
fn publish(flag: &Rel<bool>) -> () ! {MemRelease} {
    *flag = true    // ARM: stlr，x86: 普通写（TSO）
}

fn consume(flag: &Acq<bool>) -> bool ! {MemAcquire} {
    *flag           // ARM: ldar，x86: 普通读
}
```

### ordering 选错是类型错误

```lain
// 不是运行时 bug，是编译期错误
fn wrong(flag: &Rlx<bool>) -> () {
    *flag = true    // 编译器警告：
                    // publish-consume 模式需要 Rel/Acq 配对
}
```

---

## SIMD 作为一等类型

> 类型家族的完整说明见上方 [类型家族的命名清晰分离](#类型家族的命名清晰分离)。此处聚焦 SIMD 特有的使用方式。

### 类型别名

```lain
type f32x4  = Simd<f32, 4>    // 128-bit
type f32x8  = Simd<f32, 8>    // 256-bit
type f32x16 = Simd<f32, 16>   // 512-bit
type f32xN  = Simd<f32, auto> // 平台最优宽度
```

### 操作符和标量统一

```lain
let a: f32x8 = f32x8::load(ptr)
let b: f32x8 = f32x8::load(ptr2)
let c: f32x8 = a + b             // vaddps，和标量 + 一样的语法
let d: f32x8 = @fma(a * b + c)   // vfmadd，保证一次舍入
```

### 平台自动选择

```lain
// 程序员写 Simd<f32, 8>
// x86 AVX2:  ymm 寄存器
// x86 SSE4:  两个 xmm 寄存器
// ARM NEON:  两个 128-bit 寄存器
// ARM SVE:   自动适配
// 编译器决定怎么实现
```

---

## 现代 CPU 特性的一等抽象

C 的 intrinsic 是在类型系统外打洞，Lain 把这些变成类型化的一等公民。

### 位操作

```lain
x.popcount()        // popcnt / cnt
x.leading_zeros()   // lzcnt / clz
x.trailing_zeros()  // tzcnt
x.bswap()           // bswap / rev
x.rotate_left(n)    // rol，不是 UB
x.rotate_right(n)   // ror
x.extract_bits(mask) // pext（BMI2）
x.deposit_bits(mask) // pdep（BMI2）
```

### 控制流原语

```lain
@tail_call foo(args)    // 保证生成 jmp，不是 call
@unreachable()          // ud2 / udf
@likely(cond)           // 分支预测提示
@unlikely(cond)
```

### 内存原语

```lain
@lea(base + index * 4 + 8)    // 保证生成 LEA 指令
ptr.prefetch(Read, L1)        // 手动预取
@fma(a * b + c)               // 保证 FMA，一次舍入
```

---

## 语言整体感觉

用户写的代码是干净的表面，所有"高级特性"都是普通的库调用：

```lain
import std.net

@interface
struct Animal {
    fn speak(self: &Self) -> str
}

@derive(Clone, Debug)
struct Dog { name: str, age: u32 }

@impl(Animal) struct Dog {
    fn speak(self: &Self) -> str { "woof" }
}

fn main() -> () ! {IO} {
    let d = Dog { name: "Rex", age: 3 }
    let sound = d.speak()
    println(sound)
}
```

实际上发生的事：
- `@interface` → Scheme 宏生成 vtable + 胖指针类型
- `@derive` → Scheme 宏生成 clone() 和 debug() 方法
- `@impl` → Scheme 宏验证方法完整性，生成 vtable 实例
- `println` → 基于 IO 效果的标准库函数

没有 unsafe 关键字，没有 GC，没有隐藏的运行时。所有宏展开逻辑都在 .scm 文件里，用户可读、可改、可替换。

---

## 继承与拒绝

**继承了什么：**
- C 的性能和硬件控制
- Haskell 的副作用显式性（效果系统）
- Lisp 的 meta 能力（Scheme 宏）
- Zig 的"类型是值"（comptime）
- Rust 的"无隐藏运行时"

**拒绝了什么：**
- C 的隐式类型（null、有符号整数、指针）
- Rust 的内置借用检查器（可以是 meta）
- Haskell 的强制纯函数（效果系统更灵活）
- Zig 的 comptime 语言就是运行时语言（用 Scheme 更强）
- 所有语言的"语言特性"和"库"的区别

---

## 一句话

**编译器只是一个有 Scheme 的汇编器。语言是 Scheme 宏展开的结果。** 编译器提供硬件的完整抽象，Scheme 宏提供语言特性，用户在两层之间自由选择抽象层次。
