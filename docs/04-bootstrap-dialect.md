# bootstrap 方言

自举的 stage0 是手写 LAINIR 文本（bootstrap/ 下 29 个 .l1/.lain，共 11055 行，按
bootstrap/SOURCE_ORDER 拼成一个 471884 字节的编译单元）。要让 Lain 编译器能编译自己，
先得有一份**用 Lain 写的 Meta**；写它需要一门表面语言。

这份文档定的是这门方言的特性集合、语法，以及每条语法降成什么 LAINIR。它不是 lainlang
的完整规范，只是自举所需的最小子集。判据写在前面，因为后面每一条取舍都由它决定。

## 判据

- **语言语义不能等于 LAINIR 语义。** 源码里不出现 `#` 形态。否则 IR 改一个算子、
  改一条区域的 results 规则，就是破坏性的**语言**变更；LAINIR 只是 Meta 与宿主之间的私有 ABI。
- **验证器不能面对用户输入。** `infer`（宽度唯一来源）与 `verify`（VM 与后端假定的契约）
  成立的代价是「源码只能经 handler 降级到达 IR」。用户能直接写 IR，语言契约就等于整个 IR。
- **但低层操作必须可达。** 禁的是**透传**，不是低层：`load<u64>(p)` 是语言规定了类型、宽度、
  对齐、失败码，**然后**由降级决定它变成 `#load[#bits<64>](%p)`；`#load[#bits<64>](%p, 0)`
  是语言什么都没说。资格线是「这条低层算子，语言有没有替它说话」。
- **唯一的信任门是** `lainir_parse` + `lainir_verify`。源码里没有 `#` 不是安全边界 ——
  用户宏用同一套发射能力可以生成任意 LAINIR 文本（bootstrap/std/macros/ 今天就在这么干）。

## 层 0：发射层

层 0 是一门**用 Lain 写的库**，每个 LAINIR 形态一个函数。它没有类型：宽度是**数字**
（位数，不是字节），值是**句柄**。

- 它面向「正在生成的产物」，不是「我自己」。`lainir_load(v, 8)` = 往产物里追加一条
  `#load[#bits<8>]`；`load(p)`（层 1）= 读我自己地址空间里 `p` 处的字节。前缀是语义消歧，
  不是命名习惯 —— Meta 两者都要。
- **vref** 是发射出的值引用（产物里的 `%name`），不是机器值。它是不透明标量：只由
  `lainir_*` 产生、只被 `lainir_*` 消费，不能算术、不能 `as`。
- 层 0 的正式化对象就是今天 bootstrap/std/wire.l1（780 行）加 `meta_emit_ref2` 那套手写拼串。
- 阶段纪律：任何 Lain 代码都能调用层 0（用户宏也能发射任意 IR）；闸门始终是
  `lainir_parse` + `lainir_verify`。发射能力只登记进编译期那张表。

### 名单

命名规则：`lainir_` + `INST_` 后缀小写（`INST_ADD` → `lainir_add`）。下表中与 IR 有
出入的形状才逐条写。

- 整数算术 `#add #sub #mul #sdiv #udiv #srem #urem` → `lainir_add(a, b, width)` …
- 位运算 `#and #or #xor #shl #lshr #ashr` → `lainir_and(a, b, width)` …
- 比较 `#eq #ne #slt #sle #sgt #sge #ult #ule #ugt #uge` → `lainir_eq(a, b, width)` …
  （结果恒为 1 位）
- 转换 `#zext #sext #trunc #bitcast` → `lainir_zext(v, width)` …；`#ptr2int` →
  `lainir_ptr2int(v, width)`；`#int2ptr` → `lainir_int2ptr(v, width)`；
  `#fpext #fptrunc #fptosi #fptoui #sitofp #uitofp` → 同形
- 浮点算术与比较 `#fadd … #fueq …` → 同形
- 向量 `#vadd … #vcmpgt` → 同形
- 原子 `#xchg #cmpxchg #rmw_add #rmw_sub #rmw_and #rmw_or #rmw_xor` → 同形
- 内存
  - `#lea` → `lainir_lea(base, index, scale, disp)`
  - `#load` → `lainir_load(ptr, width)`
  - `#store` → `lainir_store(ptr, value, width)`
  - `#alloca` → `lainir_alloca(width, count)`
- 地址 `#data_addr` → `lainir_data_addr(name)`；`#proc_addr` → `lainir_proc_addr(name)`
- 调用 `#call` → `lainir_call(name, args…)`；`#call_indirect` → `lainir_call_indirect(target, args…)`
- 控制
  - `#if` → `lainir_if(cond, results) { … }` / `lainir_else() { … }`
  - `#loop` → `lainir_loop(label, params, results) { … }`
  - `#yield` → `lainir_yield(v)`；`#break` → `lainir_break(label, args…)`；
    `#continue` → `lainir_continue(label, args…)`
  - `#switch` → `lainir_switch(value, cases) { … }`；`#return` → `lainir_return(v)`
- 单元 `#proc` → `lainir_proc_begin(name, params, results)` / `lainir_proc_end()`；
  `#extern` → `lainir_extern(name, link_name, params, results)`；
  `#data` → `lainir_data(name, bytes)`
- 产物 `lainir_reset()`、`lainir_text()` —— 取回 canonical 文本，交给
  `lainir_parse` + `lainir_verify`

层 0 是「推迟的表面语法」的兜底：`switch`、`alloca`、`proc_addr`、间接调用没有表面语法，
仍可用 `lainir_*` 发射。推迟只说明用户代码不好写，不说明编译器做不到。

## 层 1：语言层

层 1 由 handler 实现，handler 用层 0 写：`let b: u8 = load(p)` 在 handler 里就是一次
`lainir_load(p, 8)`。

### 已实现（v0 基线）

```text
func add(a: i32, b: i32) -> i32 { return a + b; }
let main: i32 = add(3, 4);

let NAME: TYPE = <表达式>;
return <值>;
if <条件> { … }            // 可带 else
let total: u32 = for i in 0..n acc = 0 { acc = acc + i; };
```

- 赋值 `NAME = 值;` **只在 for 体内合法**：循环体唯一能赋值的合法目标是语言定的 `acc`。
- 顶层形式：`let` / `struct` / `enum` / `func` / `scalar` / `import(…)`。关键字经 Meta 语法
  注册表取 handler：1=my_if 2=func 3=if 4=struct 5=enum 6=scalar 7=顶层 let 8=return
  9=局部 let 10=for 11=my_block。
- `for` 是一条**表达式**，值就是累加变量的最终值；`#loop` 的变量直接就是 `i` 与 `acc`，
  不需要「源码名 → LAINIR 名」的映射表。v0 限制：不能嵌套循环；下界与上界是值（字面量、
  参数或已声明的局部）而不是任意表达式；循环体只有一条 `acc = 值 算子 值;`。
- 注释是必须支持的（注释里的 `scalar` 字样曾被当成声明扫进来）。

### 目标新增

- `scalar` 声明去掉尖括号，算子表用括号与逗号：

```text
scalar i32 = bits(32) { "/" = sdiv, "+" = add, "<" = slt };
scalar addr = raw {};
```

  算符字符串到 LAINIR 算子名的绑定表住在 Meta 侧；`addr` 的 repr 不能与类型同名，改用 `raw`。
- 带标签的循环（替代 `for` 的通用形态）：

```text
let n: u64 = loop scan(i: u64 = 0) -> u64 {
  …
  break scan(i);
  continue scan(i + 1);
};
```

  `break`/`continue` 的实参按 `loop` 声明变量的顺序给。
- `addr` 上的内存操作，宽度由类型定：`load<T>(p)`、`store<T>(p, v)`、`p + i`、`p + i * K`
  （后两者降成 `#lea`，scale 分别是 1 与 K）。
- 字节串字面量（降成模块里的 `#data`），给出静态字节块的地址值。
- `extern NAME(a: T) -> U = link_name;` —— 对应 `#extern`。
- `as` —— 定宽转换（`zext`/`trunc`）。有符号加宽表达不出来，允许继续缺席。

### 期望类型

- 期望类型**唯一决定宽度**：`let b: u8 = load(p)` 是 8 位，`store(p, x)` 的宽度取自 `x` 的类型。
- 没有期望类型（裸表达式位置、无标注绑定）**报错，不许猜**。
- 代价：IR 刻意不存结果类型（`infer` 是宽度的唯一来源，单独一层），所以 `let`/`return`/`as`/
  实参位置必须把期望类型交给 `load` 的 handler。这是要往 Meta 里加的一条贯通信息。

## 对照例子

`for`（v0 已有）的降级形状，取自 bootstrap/std/funcs.l1:501-509：

```text
%total = #loop L1(%i: #bits<32> = 0, %acc: #bits<32> = 0) -> (#bits<32>) {
  %c2 = #slt[#bits<32>](%i, %n)
  #if %c2 {
    %r3 = #add[#bits<32>](%acc, %i)
    %r4 = #add[#bits<32>](%i, 1)
    #continue L1(%r4, %r3)
  }
  #break L1(%acc)
}
```

`loop` + `addr` 的降级形状，与 bootstrap/std/modules.l1:31-43 的 meta_cstr_len 同形：

```text
func cstr_len(p: addr) -> u64 {
  let n: u64 = loop scan(i: u64 = 0) -> u64 {
    let b: u8 = load(p + i);
    if b == 0 { break scan(i); }
    continue scan(i + 1);
  };
  return n;
}
```

```text
#proc cstr_len(%p: #addr) -> (#bits<64>) {
  %r = #loop scan(%i: #bits<64> = 0) -> (#bits<64>) {
    %q = #lea(%p, %i, 1, 0)
    %b = #load[#bits<8>](%q)
    %end = #eq[#bits<8>](%b, 0)
    #if %end {
      #break scan(%i)
    }
    %i2 = #add[#bits<64>](%i, 1)
    #continue scan(%i2)
  }
  #return %r
}
```

## 档位

**M0（自举必需）**：定宽整数与 `scalar` 声明 / `func` + `return` / 调用 / 带值 `if` /
`loop` 与循环参数、`break`、`continue` / 比较 / 整数算术与 `trunc`、`zext` / `addr` 类型与
`load`、`store`、`lea` / 字节串字面量 / `extern` 与 `link_name`。

**推迟**：`struct`、`enum`、`switch`、`alloca`、`proc_addr`、间接调用。这些在 IR、验证器、
引擎里都已实现，缺的只是表面语法与降级；推迟的代价是「用 Lain 写别的程序不够用」，
不是「自举做不下去」。（手写 Meta 里 `#struct` 与 `#enum` 的命中数是 0：它把数据摊平进
arena 与字节偏移。）

**排除**：任何 `#` 形态、`ptr2int`/`int2ptr`、浮点、向量、原子、并发、泛型、闭包、
用户可见的 eval/apply、字符串类型、`?{}` 宏输入。

**非语言前提**：先有一小层用 Lain 写的 stdlib；宿主提供「授予一整块窗口」的能力
（`region_grant`）、`emit_uint`、`scratch_alloc`。逐字节回调不可行 —— 词法器与解析器是
逐字节跑的，11055 行 Meta 跑出 359053 步。

## 待补规则

- `bits(32)` 要改 bootstrap/std/scalars.l1 的 `meta_repr_at`（它今天读 `<N>`）。
- `vref` 要成为不透明标量（像 `addr`：不可算术、不可 `as`）。
- 期望类型的传递路径（见上）。
- `addr` 上的 `==` 给不给（层 0 有 `lainir_eq`）。
- `loop` 的 `continue` 可否省略实参。

## 与现状的差距

- `meta_repr_at` 仍读 `<N>`；bootstrap/lain/std/prelude.lain 仍写 `bits<32> { … }`。
- scalar 的降级还是占位：bootstrap/std/handlers/scalar.l1:18-21 直接
  `#call lain_meta_fail(14)`。prelude 第一行就是 scalar 声明，所以驱动今天跑到
  `steps=359053 host_status=14` 就停在这里（见 docs/development.md 的驱动一节）。
- 层 0 尚不存在；今天的对应物是 bootstrap/std/wire.l1 的手写拼串。
- 表面语言里没有内存操作：Meta 今天只能靠手写 LAINIR 或宿主交出的窗口读写字节。
