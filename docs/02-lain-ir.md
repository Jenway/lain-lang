# LAIN-IR Specification

LAIN-IR 是 Lain 编译器的物理执行层中间表示。

它位于 `Typed / Elaborated Middle AST` 之后，位于 Backend 之前：

```text
Typed / Elaborated Middle AST -> LAIN-IR -> Backend Target
```

LAIN-IR 的职责是表达已经完成语义检查之后的物理执行逻辑。

它描述：

- primitive storage type
- address
- load / store
- arithmetic primitive
- branch
- block
- loop
- physical subroutine
- call
- tail call
- compile-time evaluation target
- low-level concurrency primitive

它不描述：

- source-level `fn`
- source-level `struct`
- source-level `module`
- source-level `interface`
- source-level `effect`
- generic
- macro
- method syntax
- overload set
- unresolved name
- high-level type expression
- high-level pattern matching

这些高层语义必须在 Meta / Middle AST / Elaboration 阶段处理完毕。

## 1. Design Principle

LAIN-IR 的核心原则是：

```text
LAIN-IR is physical, typed, explicit, and executable.
```

具体含义：

```text
physical
  不保留高层语言语义，只表达物理执行。

typed
  每个 value 都有明确的 IR type。

explicit
  内存、控制流、调用、编译期执行都必须显式表示。

executable
  LAIN-IR 可以被解释器执行，也可以被后端编译。
```

LAIN-IR 不等价于高层 Lain 语言。

高层 Lain 的 `fn` 不等于 LAIN-IR 的 `#proc`。

高层 Lain 的 `struct` 不等于 LAIN-IR 的类型。

高层 Lain 的 `effect` 不等于 LAIN-IR 的控制流节点。

LAIN-IR 是 lower 之后的物理层。

## 2. Text Format

LAIN-IR 文本格式使用 `#` 前缀表示 IR keyword。

虚拟寄存器使用 `%` 前缀。

过程名、块名、外部符号名使用普通 identifier。

示例：

```lain-ir
#proc add_i32(%x: #bits<32>, %y: #bits<32>) -> #bits<32> {
  #set %z = #add #bits<32> %x %y
  #ret %z
}
```

基本形式：

```text
#proc name(args...) -> ret_type {
  instruction*
}
```

局部值绑定：

```text
#set %name = instruction
```

`#set` 是不可变绑定。

LAIN-IR 使用 SSA-like 规则：

```text
同一个 %name 在同一作用域内只能绑定一次。
```

## 3. Type System

LAIN-IR 只支持物理类型。

### 3.1 Primitive Types

```text
#bits<N>
#float<N>
#vec<N, T>
#addr
#unit
#never
```

### 3.2 `#bits<N>`

`#bits<N>` 表示 N 位整数存储。

推荐基础位宽：

```text
#bits<1>
#bits<8>
#bits<16>
#bits<32>
#bits<64>
```

`#bits<N>` 本身不区分 signed / unsigned。

有符号和无符号语义由具体指令决定，例如：

```text
#sdiv
#udiv
#scmp
#ucmp
```

比较结果使用：

```text
#bits<1>
```

### 3.3 `#float<N>`

`#float<N>` 表示浮点存储。

推荐基础位宽：

```text
#float<32>
#float<64>
```

浮点语义由具体浮点指令决定。

### 3.4 `#vec<N, T>`

`#vec<N, T>` 表示 SIMD 向量。

```text
#vec<4, #float<32>>
#vec<8, #bits<16>>
```

`T` 必须是：

```text
#bits<N>
#float<N>
```

### 3.5 `#addr`

`#addr` 表示非类型化地址。

LAIN-IR 的地址不携带 pointee type。

也就是说：

```text
#addr
```

不是：

```text
#ptr<T>
```

内存读写时必须显式给出读写类型。

### 3.6 `#unit`

`#unit` 表示正常完成但不携带值。

例如：

```lain-ir
#proc side_effect_only() -> #unit {
  #ret
}
```

### 3.7 `#never`

`#never` 表示不可能正常产生值。

典型来源：

```text
#unreachable
#trap
infinite loop without exit
```

## 4. No High-Level Aggregate Types

LAIN-IR 不提供高层聚合类型。

LAIN-IR 不直接支持：

```text
struct
tuple
array
enum
union
slice
closure
interface object
trait object
effect object
```

这些必须在 Middle AST / Elaboration 阶段完成 layout。

例如高层结构体：

```lain
struct Vec(comptime T: type) {
    ptr: Ptr(T),
    len: usize,
    cap: usize,
}
```

不能直接 lower 为：

```text
#struct Vec { ... }
```

而应该经过 layout pass 变成：

```text
size
align
field offset
field type
ABI rule
```

最终通过 LAIN-IR 的地址、offset、load、store 表达：

```lain-ir
#set %len_ptr = #offset %vec_addr 8
#set %len = #load #bits<64> %len_ptr none
```

LAIN-IR 的聚合访问本质上是：

```text
base address + byte offset + load/store type
```

## 5. Values and Constants

LAIN-IR value 分为：

```text
constant
register
procedure symbol
external symbol
```

### 5.1 Integer Constants

```lain-ir
0
1
42
255
```

整数常量在使用位置由期望类型决定。

必要时可以显式标注：

```lain-ir
#const #bits<32> 42
```

### 5.2 Float Constants

```lain-ir
#const #float<32> 1.0
#const #float<64> 3.1415926
```

### 5.3 Unit Constant

```lain-ir
#unit
```

### 5.4 Undef / Poison

早期版本不引入 `undef` / `poison`。

所有 value 必须有确定来源。

如果后续为了优化需要引入 poison 语义，必须单独写入 IR verifier 和 backend lowering 规则。

## 6. Procedures

LAIN-IR 的过程使用 `#proc` 定义。

```lain-ir
#proc add_i32(%x: #bits<32>, %y: #bits<32>) -> #bits<32> {
  #set %z = #add #bits<32> %x %y
  #ret %z
}
```

参数必须显式命名并标注类型。

返回类型必须显式标注。

返回 `#unit` 时可以写：

```lain-ir
#proc f() -> #unit {
  #ret
}
```

### 6.1 Physical Procedure

`#proc` 是物理 subroutine。

它不是高层 Lain 的 `fn`。

一个高层 `fn` 可能 lower 为：

```text
zero proc
one proc
multiple specialized proc
closure invoke proc
foreign wrapper proc
trampoline proc
```

例如：

```lain
fn identity(comptime T: type, x: T) -> T {
    x
}
```

可能 lower 为：

```lain-ir
#proc identity_i32(%x: #bits<32>) -> #bits<32> {
  #ret %x
}

#proc identity_f64(%x: #float<64>) -> #float<64> {
  #ret %x
}
```

也可能完全 inline，不产生任何 `#proc`。

### 6.2 External Procedure

外部过程可以用 `#extern_proc` 声明。

```lain-ir
#extern_proc puts(%s: #addr) -> #bits<32> link_name="puts"
```

`#extern_proc` 是 ABI 层声明，不是高层 `foreign fn`。

高层 `foreign fn` 应该先在 Middle AST 中解析、检查，再 lower 成 `#extern_proc` 或 wrapper `#proc`。

### 6.3 Calling Convention

过程可以携带 ABI metadata：

```lain-ir
#proc main() -> #bits<32> abi="c" {
  #ret 0
}
```

```lain-ir
#extern_proc puts(%s: #addr) -> #bits<32> abi="c" link_name="puts"
```

早期实现可以只支持：

```text
abi="lain"
abi="c"
```

## 7. Calls

普通调用：

```lain-ir
#set %r = #call add_i32 %a %b
```

无返回负载调用：

```lain-ir
#call print_i32 %x
```

尾调用：

```lain-ir
#tail_call next %arg0 %arg1
```

`#tail_call` 表示语义上要求尾调用。

如果后端无法保证尾调用，必须报错或在 verifier 阶段拒绝，而不是静默退化为普通调用。

调用目标必须在 lowering 后确定。

LAIN-IR 不表达 overload resolution。

错误示例：

```lain-ir
#call add %x %y
```

如果 `add` 是 overload set，这不允许出现在 LAIN-IR 中。

正确做法是 elaboration 后生成确定目标：

```lain-ir
#call add_i32 %x %y
```

## 8. Local Binding

局部绑定在规范文本中带有结果类型：

```lain-ir
#let %name: i32 = instruction
```

示例：

```lain-ir
#let %x: i32 = #add(%a, %b)
#let %y: i32 = #mul(%x, 10)
```

过程签名的 `-> T` 是 `#return` 的类型契约；绑定或参数被返回、传给调用时，verifier 必须能解析出它的类型。旧的无注解绑定（例如 `#let x = 42` 或 `%x = 42`）只作为输入兼容：验证器从可推导表达式补类型，printer 一律输出带注解的规范形式。

`#let` 不是变量赋值。

它只是给 SSA value 命名。

下面的形式非法：

```lain-ir
#let %x: i32 = #add(%a, %b)
#let %x: i32 = #sub(%x, 1)
```

应该写成：

```lain-ir
#let %x0: i32 = #add(%a, %b)
#let %x1: i32 = #sub(%x0, 1)
```

## 9. Arithmetic and Bit Operations

整数算术：

```text
#add T %a %b
#sub T %a %b
#umul T %a %b
#smul T %a %b
#udiv T %a %b
#sdiv T %a %b
#urem T %a %b
#srem T %a %b
```

示例：

```lain-ir
#set %z = #add #bits<32> %x %y
#set %q = #sdiv #bits<32> %a %b
```

位操作：

```text
#and T %a %b
#or T %a %b
#xor T %a %b
#not T %x
#shl T %x %amount
#lshr T %x %amount
#ashr T %x %amount
#rotl T %x %amount
#rotr T %x %amount
#popcount T %x
#clz T %x
#ctz T %x
```

比较：

```text
#ucmp op T %a %b
#scmp op T %a %b
```

`op` 取值：

```text
eq
ne
lt
le
gt
ge
```

比较结果类型为：

```text
#bits<1>
```

示例：

```lain-ir
#set %c0 = #ucmp lt #bits<32> %i %n
#set %c1 = #scmp ge #bits<64> %x 0
```

## 10. Floating Point Operations

浮点算术：

```text
#fadd T %a %b
#fsub T %a %b
#fmul T %a %b
#fdiv T %a %b
#fma T %a %b %c
```

浮点比较：

```text
#fcmp op T %a %b
```

`op` 取值：

```text
oeq
one
olt
ole
ogt
oge
ueq
une
ult
ule
ugt
uge
ord
uno
```

示例：

```lain-ir
#set %z = #fadd #float<64> %x %y
#set %c = #fcmp olt #float<64> %x %y
```

比较结果类型为：

```text
#bits<1>
```

## 11. Conversion Operations

整数截断与扩展：

```text
#trunc FromT ToT %x
#zext FromT ToT %x
#sext FromT ToT %x
```

浮点转换：

```text
#fptrunc FromT ToT %x
#fpext FromT ToT %x
```

整数与浮点转换：

```text
#uitofp FromT ToT %x
#sitofp FromT ToT %x
#fptoui FromT ToT %x
#fptosi FromT ToT %x
```

地址与整数转换：

```text
#addr_to_int ToT %addr
#int_to_addr FromT %x
```

示例：

```lain-ir
#set %x64 = #zext #bits<32> #bits<64> %x32
#set %f = #sitofp #bits<32> #float<64> %i
```

## 12. Memory

LAIN-IR 使用非类型化地址。

```text
#addr
```

内存访问必须显式携带访问类型。

### 12.1 Allocation

栈分配：

```text
#alloca bytes=N align=A -> #addr
```

示例：

```lain-ir
#set %buf = #alloca bytes=16 align=8
```

`#alloca` 的结果是当前 `#proc` 栈帧中的地址。

生命周期至少覆盖当前 `#proc` 的执行。

早期版本不引入显式 lifetime marker。

后续可以扩展：

```text
#lifetime_start %addr bytes=N
#lifetime_end %addr bytes=N
```

### 12.2 Address Offset

地址偏移：

```text
#offset %base byte_offset -> #addr
```

示例：

```lain-ir
#set %field_ptr = #offset %obj 8
```

`byte_offset` 是字节偏移。

它可以是常量，也可以是 integer value。

动态偏移示例：

```lain-ir
#set %byte_index = #umul #bits<64> %i 4
#set %elem_ptr = #offset %base %byte_index
```

### 12.3 Load

读取内存：

```text
#load T %addr ordering -> T
```

示例：

```lain-ir
#set %x = #load #bits<32> %ptr none
```

### 12.4 Store

写入内存：

```text
#store T %addr %value ordering -> #unit
```

示例：

```lain-ir
#store #bits<32> %ptr 10 none
```

### 12.5 Memory Ordering

`ordering` 取值：

```text
none
relaxed
acquire
release
acqrel
seqcst
```

`none` 表示普通非原子内存访问。

原子语义使用：

```text
relaxed
acquire
release
acqrel
seqcst
```

约束：

```text
#load 不能使用 release。
#store 不能使用 acquire。
普通 load/store 应使用 none。
atomic load/store 必须使用非 none ordering。
```

示例：

```lain-ir
#set %x = #load #bits<32> %ptr acquire
#store #bits<32> %ptr %x release
```

### 12.6 Memory Example

```lain-ir
#proc memory_demo() -> #bits<32> {
  #set %buf = #alloca bytes=8 align=4

  #set %ptr0 = #offset %buf 0
  #set %ptr1 = #offset %buf 4

  #store #bits<32> %ptr0 10 none
  #store #bits<32> %ptr1 20 none

  #set %a = #load #bits<32> %ptr0 none
  #set %b = #load #bits<32> %ptr1 none

  #set %sum = #add #bits<32> %a %b
  #ret %sum
}
```

## 13. Atomic Operations

基础 atomic read/write 可以由 `#load` / `#store` 携带 ordering 表示。

复合 atomic 操作使用专门指令：

```text
#atomic_rmw op T %addr %value ordering -> T
#cmpxchg T %addr %expected %desired success_ordering failure_ordering -> #bits<1>
```

`#atomic_rmw op` 的 `op` 取值：

```text
add
sub
and
or
xor
xchg
umin
umax
smin
smax
```

示例：

```lain-ir
#set %old = #atomic_rmw add #bits<32> %counter 1 acqrel
```

早期版本可以只实现 `#load` / `#store`，把 `#atomic_rmw` 和 `#cmpxchg` 留作扩展。

## 14. Control Flow

LAIN-IR 使用结构化控制流。

核心节点：

```text
#block
#loop
#break
#continue
#if
#switch
#ret
#unreachable
```

LAIN-IR 的公开控制流是结构化 region，不是 CFG。普通 region 没有
basic-block label，也不能作为任意跳转目标。LLVM backend 或 custom
backend 可以在 lowering 时为这些 region 构造私有 CFG、basic block 和
phi/block parameter。

## 15. Block

`#block` 表示一个可被 `#break` 退出的结构化区域。

```lain-ir
#set %r = #block name (%arg0: T0, %arg1: T1, ...) -> RetT {
  ...
  #break name %value
}
```

如果 block 不返回负载，返回类型为 `#unit`。

示例：

```lain-ir
#set %x = #block choose () -> #bits<32> {
  #set %c = #ucmp lt #bits<32> %a %b
  #if %c {
    #break choose %a
  } else {
    #break choose %b
  }
}
```

早期实现也可以省略显式 `#block` 结果绑定，只允许 statement-form block。

但规范目标上，`#block` 可以产生值。

## 16. Loop

`#loop` 表示带参数的结构化循环。

```lain-ir
#set %r = #loop name (%param0: T0 = init0, %param1: T1 = init1, ...) -> RetT {
  ...
  #continue name %next0 %next1
  #break name %result
}
```

循环参数相当于 SSA phi。

示例：

```lain-ir
#proc sum_to_n(%n: #bits<32>) -> #bits<32> {
  #set %result = #loop sum_loop (
    %i: #bits<32> = 0,
    %sum: #bits<32> = 0
  ) -> #bits<32> {
    #set %done = #ucmp gt #bits<32> %i %n
    #if %done {
      #break sum_loop %sum
    } else {
      #set %next_sum = #add #bits<32> %sum %i
      #set %next_i = #add #bits<32> %i 1
      #continue sum_loop %next_i %next_sum
    }
  }

  #ret %result
}
```

## 17. Conditional Region

条件控制流使用结构化 `#if`：

```lain-ir
#set %result = #if %cond {
  #yield %then_value
} else {
  #yield %else_value
}
```

`%cond` 类型必须是：

```text
#bits<1>
```

不产生值时，可以省略结果绑定和 `#yield`：

```lain-ir
#set %c = #ucmp lt #bits<32> %x %y
#if %c {
  #ret %x
} else {
  #ret %y
}
```

`#if` 的分支是嵌套 region，不拥有 CFG label。产生值的各分支必须以
`#yield` 返回与声明结果类型一致的值。backend 在需要时把它 lower 为
then/else/merge basic blocks。

## 18. Switch

多路分支：

```lain-ir
#switch %target {
  case 0 { ... }
  case 1 { ... }
  default { ... }
}
```

示例：

```lain-ir
#switch %tag {
  case 0 { #ret 0 }
  case 1 { #ret 1 }
  default { #ret 255 }
}
```

## 19. Return and Unreachable

返回：

```text
#ret %value
#ret
```

`#ret` 只能用于返回类型为 `#unit` 的过程。

不可达：

```text
#unreachable
```

陷入运行时错误：

```text
#trap
```

`#unreachable` 表示控制流在语义上不可达。

`#trap` 表示运行时主动终止。

## 20. Compile-Time Execution

LAIN-IR 可以作为编译期执行的目标。

编译期执行由 Host evaluator 执行 LAIN-IR。

### 20.1 Pure Eval

纯编译期求值：

```lain-ir
#set %x = #eval #block const_calc () -> #bits<32> {
  #set %a = #call fib_i32 10
  #break const_calc %a
}
```

`#eval` 要求内部计算在编译期完成，并把结果作为 IR 常量或 comptime value 返回。

### 20.2 Comptime Call

编译期调用：

```lain-ir
#comptime_call generate_bindings
```

示例：

```lain-ir
#proc generate_bindings() -> #unit {
  #call host_read_file
  #call host_write_file
  #ret
}

#proc main() -> #bits<32> {
  #comptime_call generate_bindings
  #ret 0
}
```

`#comptime_call` 表示该过程在编译期间执行，而不是运行时执行。

### 20.3 Boundary

Meta 层不直接执行任意 I/O。

需要 I/O 的编译期行为必须 lower 成受约束的 LAIN-IR，并交给 Host evaluator。

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

LAIN-IR 本身只表达执行目标，不负责高层 effect checking。

## 21. Low-Level Concurrency Primitives

LAIN-IR 可以提供绿色线程、协程和用户态调度所需的最低层 primitive。

这些 primitive 是物理层能力，不是高层 async / effect 语义。

### 21.1 Context Initialization

```text
#init_context %stack_addr %proc_addr -> #addr
```

含义：

```text
根据 stack address 和 procedure address 初始化一个可切换的执行上下文。
```

### 21.2 Context Switch

```text
#swap_context %current %next -> #unit
```

含义：

```text
保存 current context，恢复 next context，并切换执行位置。
```

`#swap_context` 是强优化屏障。

后端必须认为：

```text
register state may escape
stack state may escape
control flow is non-local
```

### 21.3 Boundary

LAIN-IR 的 context primitive 不表示高层 async 函数。

高层 async / coroutine / effect handler 必须先在 Middle AST 或 Elaboration 阶段 lower。

最终可以 lower 到：

```text
state machine
heap frame
stackful context
scheduler call
#swap_context
```

LAIN-IR 只提供最底层物理操作。

## 22. Diagnostics Metadata

LAIN-IR instruction 可以携带调试与报错 metadata。

例如：

```lain-ir
#set %x = #load #bits<32> %ptr none @span("main.lain", 10, 5)
```

metadata 不影响执行语义。

常见 metadata：

```text
@span(file, line, column)
@origin(ast-node-id)
@name(source-name)
@inline_hint
@no_inline
```

Backend 可以使用 metadata 生成 debug info。

Verifier 不应依赖 metadata 判断程序语义。

## 23. Verification Rules

LAIN-IR 在进入 Backend 前必须通过 verifier。

基础规则：

```text
所有 %register 必须先定义再使用。
同一作用域内 %register 不能重复定义。
所有 instruction 的 operand type 必须匹配。
所有 #ret 的 value type 必须匹配当前 #proc 返回类型。
#ret 只能出现在 #proc 内。
#break 的目标必须是可见的 #block 或 #loop。
#continue 的目标必须是可见的 #loop。
#break 参数必须匹配目标 region 返回类型。
#continue 参数必须匹配目标 loop 参数类型。
#if 的条件必须是 #bits<1>。
#if / #switch 的分支必须是嵌套 region，不能引用任意 CFG label。
#switch 的 case value 必须匹配 target 类型。
#load / #store 必须显式携带访问类型。
#load 不能使用 release ordering。
#store 不能使用 acquire ordering。
#tail_call 必须处于 tail position。
#tail_call 的返回类型必须匹配当前 #proc 返回类型。
#unreachable 之后不能有可达普通 instruction。
```

## 24. Example: Add

```lain-ir
#proc add_i32(%x: #bits<32>, %y: #bits<32>) -> #bits<32> {
  #set %z = #add #bits<32> %x %y
  #ret %z
}
```

## 25. Example: If

```lain-ir
#proc min_u32(%a: #bits<32>, %b: #bits<32>) -> #bits<32> {
  #set %c = #ucmp lt #bits<32> %a %b
  #set %result = #if %c {
    #yield %a
  } else {
    #yield %b
  }
  #ret %result
}
```

## 26. Example: Loop

```lain-ir
#proc sum_to_n(%n: #bits<32>) -> #bits<32> {
  #set %result = #loop sum_loop (
    %i: #bits<32> = 0,
    %sum: #bits<32> = 0
  ) -> #bits<32> {
    #set %done = #ucmp gt #bits<32> %i %n
    #if %done {
      #break sum_loop %sum
    } else {
      #set %next_sum = #add #bits<32> %sum %i
      #set %next_i = #add #bits<32> %i 1
      #continue sum_loop %next_i %next_sum
    }
  }

  #ret %result
}
```

## 27. Example: Struct Field Access After Layout

高层代码：

```lain
struct Pair {
    a: i32,
    b: i32,
}

fn second(p: Ptr(Pair)) -> i32 {
    p.b
}
```

假设 layout pass 计算出：

```text
Pair.a offset = 0
Pair.b offset = 4
Pair size = 8
Pair align = 4
```

LAIN-IR：

```lain-ir
#proc pair_second(%p: #addr) -> #bits<32> {
  #set %b_ptr = #offset %p 4
  #set %b = #load #bits<32> %b_ptr none
  #ret %b
}
```

注意：

```text
LAIN-IR 中没有 Pair 类型。
LAIN-IR 中没有 field access。
只有 address、offset、load。
```

## 28. Example: Foreign Call

```lain-ir
#extern_proc puts(%s: #addr) -> #bits<32> abi="c" link_name="puts"

#proc main() -> #bits<32> abi="c" {
  #set %msg = #global_addr hello_string
  #set %r = #call puts %msg
  #ret 0
}

#global hello_string bytes="hello, world\00" align=1
```

## 29. Globals

全局数据使用 `#global` 定义。

```lain-ir
#global name bytes="..." align=N
```

获取全局地址：

```lain-ir
#global_addr name -> #addr
```

示例：

```lain-ir
#global hello_string bytes="hello, world\00" align=1

#proc get_msg() -> #addr {
  #set %p = #global_addr hello_string
  #ret %p
}
```

早期版本可以只支持 immutable global bytes。

后续可以扩展 mutable global：

```lain-ir
#global_mut counter size=4 align=4 init=0
```

## 30. Backend Mapping

LAIN-IR 可以 lower 到：

```text
C
LLVM IR
native code
interpreter bytecode
```

Backend 不应该重新解释高层 Lain 语义。

Backend 只处理 LAIN-IR：

```text
#bits
#float
#vec
#addr
#alloca
#offset
#load
#store
#add
#if
#switch
#loop
#proc
#call
```

例如：

```lain-ir
#set %b_ptr = #offset %p 4
#set %b = #load #bits<32> %b_ptr none
```

可以 lower 到 C：

```c
uint32_t b = *(uint32_t*)((uint8_t*)p + 4);
```

也可以 lower 到 LLVM IR：

```llvm
%ptr = getelementptr i8, ptr %p, i64 4
%b = load i32, ptr %ptr
```

## 31. Boundary Summary

LAIN-IR 的边界总结：

```text
LAIN-IR 是物理执行层。
LAIN-IR 不是 source language AST。
LAIN-IR 不包含 parser-level syntax。
LAIN-IR 不包含 unresolved semantic form。
LAIN-IR 不包含 generic。
LAIN-IR 不包含 macro。
LAIN-IR 不包含 high-level struct。
LAIN-IR 不包含 high-level effect。
LAIN-IR 不包含 overload。
LAIN-IR 不包含 source-level fn。
```

核心对应关系：

```text
source fn      -> Meta form -> maybe one or more #proc
source struct  -> layout metadata -> #addr + #offset + #load/#store
source generic -> comptime specialization -> concrete #proc / code
source effect  -> checked semantic annotation -> lowered control/runtime form
source module  -> symbol namespace / linkage -> concrete proc/global names
```

最重要的原则：

```text
Middle AST owns language semantics.
LAIN-IR owns physical execution.
Backend owns target emission.
```
