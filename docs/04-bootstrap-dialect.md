# bootstrap 方言

自举的 stage0 是手写 LAINIR 文本（bootstrap/ 下按 bootstrap/SOURCE_ORDER 拼成 27 个文件、
10692 行的编译单元；驱动报 unit files=27 bytes=456914）。要让 Lain 编译器能编译自己，
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
- **不新增声明语法去描述 IR。** 语言的**知识**（有哪些标量、多宽、算符接到哪个 LAINIR 算子）
  住在 Meta 库的表里，不住在源码里。语法只用来写**有体**的东西（`func` / `struct` / `enum` /
  `let` / `import`）；没有体的声明只是换个地方抄库表。用户要自定义，走编译期调用库的注册
  接口，而不是新语法。
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
if <条件> { … }            // 可带 else；条件只吃单 token 或 (表达式)，分支必须以 return 收尾
let total: u32 = for i in 0..n acc = 0 { acc = acc + i; };
```

- 赋值 `NAME = 值;` **只在 for 体内合法**：循环体唯一能赋值的合法目标是语言定的 `acc`。
- 顶层形式：`let` / `struct` / `enum` / `func` / `import(…)`。关键字经 Meta 语法
  注册表取 handler：1=my_if 2=func 3=if 4=struct 5=enum 7=顶层 let 8=return
  9=局部 let 10=for 11=my_block。**6（原 scalar 声明）已取消，号位退休不复用。**
- `for` 是一条**表达式**，值就是累加变量的最终值；`#loop` 的变量直接就是 `i` 与 `acc`，
  不需要「源码名 → LAINIR 名」的映射表。v0 限制：不能嵌套循环；下界与上界是值（字面量、
  参数或已声明的局部）而不是任意表达式；循环体只有一条 `acc = 值 算子 值;`。
- 注释是必须支持的：声明发现按词走，不认注释的话注释里的词会被当成形式。
- 这一节写的是**设计**，实际可用的形状与失败码（实测口径）在《计划》一节。

### 目标新增

- **没有 scalar 声明语法。** 标量集、repr（kind + 宽度）与算符绑定（`"/"` → `sdiv` 一类）
  是 **Meta 库的表数据**：今天就是 bootstrap/std/scalars.l1 里的一段 `data` 字节表，
  将来是用 Lain 写的库模块。表之外没有第二份出处：`i32` 这类名字由名字解析直接命中内建表，
  不进作用域表；带点的 `T.i32` 没有解析（标量是内建名字，模块不导出标量），一律按
  「没有这个名字」报 5。用户自定义标量走编译期调用库的注册接口，不是新语法。
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

## 计划（实测口径）

分档只看两样东西：**手写 Meta 的真实 IR 用量**、**驱动实测的失败码**。用量取自 bootstrap/
下全部 .l1/.lain；失败码取自 `build/lain-meta.exe` 对最小样例的 host_status。既没有用量、
又测不通的，进「推迟」。

### 一、已能用（实测 host_status=0）

| 特性 | 表面写法 | 实测口径 |
|---|---|---|
| 函数、参数类型、调用 | `func f(a: i32) -> i32 { … }`、`f(2)` | 多语句体、递归都通 |
| 绑定与返回 | `let n: i32 = …;`、`return n;` | `let` 必须写标注（否则 21） |
| 条件 | `if b {…}`、`if (a < a) {…}`、`+ else`、`my_if((…))` | 条件只吃单 token 或 `(表达式)`；分支必须以 `return` 收尾，所以 `if` 是语句不是值（裸条件 `if a < b` 报 4） |
| 循环 | `let t: i32 = for i in 0..n acc = 0 { acc = acc + i; };` | 仅 i32/u32（u64 报 5） |
| 算子 | i32 → `+ / <`（add/sdiv/slt）；u32 → `+ / <`（add/udiv/ult） | 只有这 6 个绑定，见 A |
| 声明 | `struct`、`enum`、`import(…)`、顶层 `let x = T {…};` | 会发出布局与静态存储 |
| 宏 | `my_if`、`my_block`、`my_require` | 编译期展开，过同一道信任门 |

实测被拒（列出来是因为它们看着都该能用）：`-`、`*`、`%` 报 6（`/` 能用，这几个没有绑定）；
`>`、`==`、`!=`、`<=`、`>=`、`and`、`or`、`<<`、`>>` 报 4；`as`、负数字面量、十六进制、
字节串字面量报 4；带值 `if` 报 4。

### 二、半成品：`struct` / `enum` 的值层

它们**不是**档位问题 —— handler 4/5 早已注册，声明会发出布局（`<T>__<f>_offset`、
`<T>__size`、变体的 tag 与载荷偏移），顶层 `let p = Pair { a: 1 b: 2 };` 也会发出
`data p_storage rw { … }` 与 `#proc p() -> #addr`。缺的是**值层**：字段访问与变体投影当值用
还没降级（bootstrap/std/funcs.l1:895-898 干净地报 4），struct/enum 名字也还不能写进参数类型
或带标注的 `let`。函数体里的构造还差一份 storage —— 顶层那份是静态 `data`，函数体内要么
给每个构造点一份静态 storage，要么先有 `#alloca`。

### 三、待做（按阻塞顺序）

**A. 算子表** —— 不是语法，是 bootstrap/std/scalars.l1 里那段 `data` 字节表。
- i32/u32 补 `-`、`*`、`%`、比较全集 `== != > <= >=`、位运算 `and or shl lshr`。
- **u64 / usize 今天零个算子**：`#bits` 3607 次、`#add` 661 次 —— Meta 的长度与偏移全是
  u64，这是最刺眼的一条。`bool` 的逻辑算子同理。
- IR 侧零改动：`#sub #mul #udiv #urem #eq #ne #ult #uge #and #or #shl #lshr` 都已实现。

**B. 宽度推导与转换** —— 「算子产生自己的结果类型」。它同时是 `if x < 2` 能不加括号、
`let` 能省标注的前置（funcs.l1:934 注释里记的就是这条）。连同 `as`（`#zext` 16 / `#trunc` 23）、
负数字面量与十六进制。

**C. 内存与地址** —— Meta 的实际工作方式。
- `addr` 类型（已有）＋ `load` / `store` / `lea`、`p + i`（`#lea` 610 是最大宗内存操作，
  `#load` 42、`#store` 16）。
- 字节串字面量 → `#data`（`#data_addr` 232）。
- 宿主授予**整块窗口**的能力（`region_grant`）；逐字节回调不可行。

**D. 控制流补齐**
- 带值 `if`（`#if` 779 + `#yield` 719）。
- `loop` 带标签 + 循环参数 + `break` / `continue` 带实参（`#loop` 97 / `#break` 185 /
  `#continue` 104）；今天只有 `for … acc` 一种形状。

**E. 声明与链接**
- `extern NAME(a: T) -> U = link_name;`（45 个能力 1:1；手写 Meta 的 `#extern` 正好 45 次）。
- struct / enum 名字可写进参数类型与标注；函数体内构造的 storage（见二）。

**F. 层 0：`lainir_*` 发射库** —— 见上文《层 0：发射层》。它是「用 Lain 写 Meta」真正缺的
那一层，也是 bootstrap/std/wire.l1 那套手写拼串的正式化。

用量证据（bootstrap 全部 .l1/.lain）：`#bits` 3607、`#call` 2407、`#if` 779、`#yield` 719、
`#add` 661、`#lea` 610、`#return` 565、`#eq` 534、`#proc` 396、`#data_addr` 232、
`#break` 185、`#and` 142、`#ne` 140、`#continue` 104、`#ult` 102、`#loop` 97、`#sub` 74、
`#or` 65、`#mul` 55。

### 四、推迟与排除

**推迟**：`switch`、`alloca`、`proc_addr`、间接调用。它们在 IR、验证器、引擎里都已实现，
缺的只是表面语法与降级；层 0 是它们的兜底，所以推迟的代价是「用 Lain 写别的程序不够用」，
不是「自举做不下去」。（手写 Meta 把数据摊平进 arena 与字节偏移，`#struct`/`#enum` 的命中数
是 0 —— 这只说明**写编译器本身**不需要复合值，不说明语言该不该有。）

**排除**：任何 `#` 形态、`ptr2int`/`int2ptr`、浮点、向量、原子、并发、泛型、闭包、
用户可见的 eval/apply、字符串类型、`?{}` 宏输入。

**非语言前提**：先有一小层用 Lain 写的 stdlib；宿主提供「授予一整块窗口」的能力
（`region_grant`）、`emit_uint`、`scratch_alloc`。逐字节回调不可行 —— 词法器与解析器是
逐字节跑的。

### 五、结论

今天能跑通的最小闭环 = `func` / `return` / 调用 / `let`（带标注）/ `if`（括号）/ `for` /
三个算子。够做「一小段 Lain 端到端」，不够写编译器。到「能用 Lain 重写 Meta」还差
**A + C + D + E**，其中 A 最快见效、C 最不可替代。

## 待补规则

- `vref` 要成为不透明标量（像 `addr`：不可算术、不可 `as`）。
- 期望类型的传递路径（见上）。
- `addr` 上的 `==` 给不给（层 0 有 `lainir_eq`）。
- `loop` 的 `continue` 可否省略实参。

## 与现状的差距

- 标量已经是表：bootstrap/std/scalars.l1 里一段 `data` 字节（名字、kind、宽度、算符对），
  handler 6 与 bootstrap/std/handlers/scalar.l1 已删除，bootstrap/lain/std/prelude.lain
  也随之删除（语言里不再有标量声明，也就没有「先 import 一份 prelude」这一步）。
- 层 0 尚不存在；今天的对应物是 bootstrap/std/wire.l1 的手写拼串。
- 表面语言里没有内存操作：Meta 今天只能靠手写 LAINIR 或宿主交出的窗口读写字节。
- 端到端验收源是 bootstrap/lain/examples/arith.lain：`host_status=0`、`output bytes=74`，
  产物是 `%r1 = #add[#bits<32>](1, 2)`（见 docs/development.md 的驱动一节）。
