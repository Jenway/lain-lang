# bootstrap 方言

自举的 stage0 是手写 LAINIR 文本（bootstrap/ 下按 bootstrap/SOURCE_ORDER 拼成 28 个文件、
12809 行的编译单元；驱动报 unit files=28 bytes=552938）。要让 Lain 编译器能编译自己，
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

F1 已实现并端到端验收：`bootstrap/std/lainir.l1` 提供 `lainir_reset()`、
`lainir_text()`、`lainir_length()`、`lainir_proc_begin(name, name_length)`、
`lainir_return(value)`、`lainir_proc_end()`。当前过程骨架发射无参、返回 `#bits<32>`
的过程；`lainir_return` 接受非负十进制立即数。样例
`bootstrap/lain/examples/lainir_f1.l1` 生成 `#proc probe() -> #bits<32> {`、
`#return 0` 和独立闭合行，产物经 `build\check_unit.exe` 解析与验证通过；驱动
报告 `host_status=0`、`trap none`。Canonical 等价性将在 F5 按解析后的结构比较。

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
let NAME = <表达式>;      // 标注可省：类型由值推（调用取被调方返回类型）
return <值>;
if <条件> { … }            // 可带 else；条件是任意表达式（括号可省），分支必须以 return 收尾
let NAME: TYPE = if <条件> { <值> } else { <值> };   // 带值 if：两个分支各 yield 一个值
let total: u32 = for i in 0..n acc = 0 { acc = acc + i; };
let t: i32 = loop outer(i: i32 = n, acc: i32 = 0) while i > 0 { acc = acc + i; i = i - 1; };
break LABEL(<值>);        // 早退：把值交给循环，就是值型循环的出口
continue LABEL(<实参…>);  // 早退：进入下一轮，实参按参数顺序给
let b: u8 = load(p);      // 读内存：宽度由期望类型定（也可写 return load(p);）
store(p, v);              // 写内存：宽度取自 v 的类型
let q: addr = lea(p, i);  // 地址算术：降成 #lea(%p, %i, 1, 0)
let s: addr = "hi";      // 静态字节块（只允许顶层 let）：产物是 data s_storage ro { 104 105 0 } + #proc s() -> #addrextern NAME(a: T) -> U = link_name;   // 外部过程：只声明不定义，调用与 func 同路，产物是 #proc NAME(…) -> U #extern "link_name"
```

- 赋值 `NAME = 值;` **只在循环体内合法**：能赋值的合法目标是循环自己声明的参数
  （`for` 只有语言定的 `acc` 一个）。
- `loop` 是 `for` 的通用形态：标签、任意个参数（各带类型与初值）、任意条件、多语句体。
  条件与 `if` 走同一个表达式入口，体里每格是 `参数 = 值;`，循环回边按声明顺序带走参数；
  参数里没有 `acc` 就没有值可取，报 4。
- 体里还可以写 `break LABEL(<值>);` 与 `continue LABEL(<实参…>);`，两者都结束这一轮
  之后的语句（区域里终止指令必须是最后一条）。`break` 直接把值交给循环；`continue`
  按参数顺序换掉下一轮的参数，循环的 `#break` 出口仍然照发 —— 值型循环的区域里必须有
  一个 `#break`（验证器 2006），所以落尾的 `continue` 也走收尾那条 `#continue`。
- `load` / `store` / `lea` 是**内建**（不是用户函数，名字不查作用域表）：
  `load(p)` 只出现在能提供期望类型的位置（`let b: u8 = …`、`return …`），宽度由那里定；
  `store(p, v)` 的宽度取 `v` 的类型；`lea(p, i)` 发 `#lea(%p, %i, 1, 0)`。没有期望类型
  （无标注 `let b = load(p);`、`load(p);` 单独成句）报 4，不猜。地址加整数是语言自己的一条规则：
  `p + i` 降成 `#lea(基址, 下标, 1, 0)`（只认 `+`，且左边必须是地址；`i + p` 报 4）；
  `p + i * K` 还没接（没有优先级，见《计划》C）。
- 字节串字面量（`"…"`）**只允许出现在顶层** `let NAME: addr = "…";`：发的是一份静态字节块
  `data NAME_storage ro { <字节…> 0 }` 加一个取址过程 `#proc NAME() -> #addr`（同一个名字，
  `%base = #data_addr NAME_storage`）。字节按源码原样搬（reader 认转义对但不解码），末尾补一个 0。
  别处（函数体内、作为实参）报 4：`data` 只能落顶层，而发射是流式的。
- `extern` 是**声明**不是定义：形如 `extern NAME(a: T) -> U = link_name;`，链接名原样搬进
  产物（`#proc NAME(%a: T) -> U #extern "link_name"`）。声明在作用域里的身份与 `func` 一样是 6，
  所以调用点、返回类型推导、参数表检查共用同一条路；它不登记任何宿主能力（那头是驱动
  `lainmeta_host_register` 的事）。样例 `bootstrap/lain/examples/extern.lain`。
- 顶层形式：`let` / `struct` / `enum` / `func` / `extern` / `import(…)`。关键字经 Meta 语法
  注册表取 handler：1=my_if 2=func 3=if 4=struct 5=enum 7=顶层 let 8=return
  9=局部 let 10=for 11=my_block 13=loop 14=extern（12 是宏 `my_require`）。**6（原 scalar 声明）
  已取消，号位退休不复用。**
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
- `loop` 的**早退形态**：**已可用** —— `break LABEL(<值>);` 与 `continue LABEL(<实参…>);`
  由 `meta_lower_loop_jump` 降级，样例 `bootstrap/lain/examples/jumps.lain`（产物含
  `#break L<n>(…)` 与 `#continue L<n>(…)`，过 `lainir_parse` + `lainir_verify`）。
  尚缺**有条件**的早退：循环体今天还不接受 `if`，所以跳出只能是无条件的（体里第一件事
  就是跳出）；补法见 D 的剩项。

```text
let n: u64 = loop scan(i: u64 = 0, acc: u64 = 0) while i < len {
  break scan(acc);
  continue scan(i + 1, acc + 1);
};
```

  `break`/`continue` 的实参按 `loop` 声明参数的顺序给。
- `addr` 上的内存操作，宽度由类型定：`load<T>(p)`、`store<T>(p, v)`、`p + i`、`p + i * K`
  （后两者降成 `#lea`，scale 分别是 1 与 K）。
- 字节串字面量（降成模块里的 `#data`），给出静态字节块的地址值。
- `extern NAME(a: T) -> U = link_name;` —— 对应 `#extern`。
- `as` —— 定宽转换（`zext`/`trunc`）。有符号加宽表达不出来，允许继续缺席。

### 期望类型

- 期望类型**唯一决定宽度**：`let b: u8 = load(p)` 是 8 位，`store(p, x)` 的宽度取自 `x` 的类型。
- 没有期望类型、值也推不出来时（裸表达式位置）**报错，不许猜**；无标注绑定
  `let y = <值>` 由值推：调用取被调方声明的返回类型、参数名取参数表、括号往里一层，
  都不是才退回本函数的返回类型当**上下文**（两个裸字面量的算子靠它查表）。
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

`loop`（已实现）的降级形状，取自样例 bootstrap/lain/examples/loop.lain 的产物：

```lain
func countdown(n: i32) -> i32 {
  let total: i32 = loop outer(i: i32 = n, acc: i32 = 0) while i > 0 {
    acc = acc + i;
    i = i - 1;
  };
  return total;
}
```

```text
%total = #loop L1(%i: #bits<32> = %n, %acc: #bits<32> = 0) -> (#bits<32>) {
  %r2 = #sgt[#bits<32>](%i, 0)
  #if %r2 {
    %r3 = #add[#bits<32>](%acc, %i)
    %r4 = #sub[#bits<32>](%i, 1)
    #continue L1(%r4, %r3)
  }
  #break L1(%acc)
}
```

初值发在 `#loop` 之前（LAINIR 的循环参数初值在父区域求值）；条件、体、回边与出口都在
循环区域内，出口只有一个，就是 `#if` 之后落下的那条 `#break`。

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
| 绑定与返回 | `let n: i32 = …;`、`let y = inc(n);`、`return n;` | 标注可省：类型由值推（调用取被调方返回类型、参数名取参数表） |
| 条件 | `if a < b {…}`、`if (a < a) {…}`、`+ else`、`my_if((…))` | 条件是任意表达式：裸条件与括号条件走同一个入口，块就是表达式停下来的那一格；分支必须以 `return` 收尾，所以 `if` 是语句不是值 |
| 带值 if | `let x: i32 = if c { a } else { b };` | 降级为 `%x = #if %c -> (#bits<32>) { … #yield %a } else { … #yield %b }`：条件是任意表达式、类型取绑定的标注（缺标注由值推）；**只在绑定位置**可用（`return if …` 报 4），**必须带 else**（缺 else 报 4），两个分支都必须以一个值收尾。样例 `bootstrap/lain/examples/choose.lain` |
| 循环 | `let t: i32 = for i in 0..n acc = 0 { acc = acc + i; };`、`let t: i32 = loop outer(i: i32 = n, acc: i32 = 0) while i > 0 { acc = acc + i; i = i - 1; };` | `for` 的边界与累加器宽度跟循环变量的类型走：i32/u32/u64/i64/usize 都能用（`u64` 曾报 5，A 的第四刀补齐 64 位算子后通了，实测 `#ult[#bits<64>]`），体只有一条赋值；`loop` 是通用形态：标签 + 任意个参数（各带类型与初值）+ 任意条件 + 多语句体，结果类型取 `acc` 参数的类型（标注可省）；`break LABEL(值);` / `continue LABEL(实参…);` 可早退（落尾的 `continue` 等价于「换掉下一轮参数再绕一圈」）。样例 `bootstrap/lain/examples/loop.lain`、`bootstrap/lain/examples/jumps.lain` |
| 算子 | i32 → `+ - * / % < > <= >= == !=`（add/sub/mul/sdiv/srem + slt/sgt/sle/sge/eq/ne）；u32 → 同（`/`、`%`、`>`、`<=`、`>=` 走 udiv/urem/ugt/ule/uge） | i32/u32/u64/i64/usize/bool 的算术、比较与位运算（`and`/`or`/`<<`/`>>` → and/or/shl/lshr）都已绑定；只有 `i8`/`u8` 没有算子（它们只作 load/store 宽度） |
| 声明 | `struct`、`enum`、`import(…)`、顶层 `let x = T {…};` | 会发出布局与静态存储 |
| 聚合类型名 | `struct Point { x: i32 y: i32 }`、`enum Shape { Circle(i32) Empty }`、`func f(p: Point) -> i32`、`let q: Shape = s;` | 名字可进参数类型与带标注的 `let`：聚合值在 IR 里是 `#addr`，所以参数类型发 `#addr`、标注绑定发一次地址复制（`#lea(%p, 0, 1, 0)`）；声明体里字段/变体**空格分隔，不能写逗号**（写了会当成字段名，报 5）。样例 `bootstrap/lain/examples/types.lain` |
| 宏 | `my_if`、`my_block`、`my_require` | 编译期展开，过同一道信任门 |
| 转换 | `x as T`（定宽整数之间） | 变宽发 `#zext`、变窄发 `#trunc`、等宽补一条加零复制；目标类型是**类型名**不是值；源宽度取操作数自己的类型（参数名），取不到才用上下文宽度。样例 `bootstrap/lain/examples/convert.lain` |
| 十六进制字面量 | `0x10`、`0Xff` | 读成数值后按**十进制**规范化写进产物（`0xff00` → `65280`）；整段必须是合法数字：`0x` 后面没有数字报 4；超过绑定宽度仍报 23。样例 `bootstrap/lain/examples/hex.lain` |
| 内存 | `let b: u8 = load(p);`、`return load(p);`、`store(p, v);`、`let q: addr = lea(p, i);`、`load(p + i)` | 三个内建名字：`load` 的宽度由期望类型定（`#load[#bits<8>](%p, 0)`），`store` 的宽度取值的类型（`#store[#bits<8>](%v, %p)`），`lea` 发 `#lea(%p, %i, 1, 0)`。地址加整数（`p + i`、`p + 1`）降成 `#lea(基址, 下标, 1, 0)`。没有期望类型报 4（`let b = load(p);`、`load(p);`）；`i + p` 报 4、`p + i * K` 报 6。样例 `bootstrap/lain/examples/memory.lain` |
| 字节串字面量 | `let s: addr = "hi";`（只允许顶层） | 发 `data s_storage ro { 104 105 0 }` 与 `#proc s() -> #addr { %base = #data_addr s_storage; #return %base }`；字节原样搬、末尾补 0。样例 `bootstrap/lain/examples/bytes.lain`。函数体内写字节串报 4 |
| 外部过程与链接 | `extern NAME(a: T) -> U = link_name;` | 只声明不定义：名字进作用域（身份与 `func` 一样）、调用走同一条表达式路径，产物是 `#proc NAME(%a: T) -> U #extern "link_name"`；链接名原样搬运，宿主那头登记与否由驱动决定。样例 `bootstrap/lain/examples/extern.lain` |
| 负数字面量 | `-5`（负号紧跟数字） | LAINIR 的字面量是无符号十进制文本，没有负号，所以降级成 `#sub[repr](0, 5)`：宽度与符号都取上下文类型。只认字面量，一元负号作用于变量仍报 4。样例 `bootstrap/lain/examples/neg.lain` |

实测被拒（列出来是因为它们看着都该能用）：`i + p` 报 4（只认左边的地址）；
`p + i * K` 报 6（降级是左到右折叠，没有乘法优先级 —— 要支持得先有表达式树）。
（`-`、`*`、`%` 曾报 6，比较全集与 `and`、`or`、`<<`、`>>` 曾报 4，已在 A 里补上绑定；
裸条件 `if a < b` 曾报 4、无标注 `let y = …` 曾报 21，已在 B 的第一半修好；`as` 与负数字面量
曾报 4，已在 B 的后半修好；十六进制 `0x10` 曾被 `bs_read_uint` 静默读成 `0`（更糟：`host_status=0` 而产物是错的），
已在同一次修好；带值 `if` 曾报 4，已在 D 的第一半修好；字节串字面量曾报 4，已在 C 的第二半修好
（只支持顶层绑定）；`load`/`store`/`lea` 曾报 6
（收尾的加零复制走源码算子表，`u8` 没有绑定），已在 C 的第一半修好；函数体内的聚合构造
曾报 4，已在 E 的第三刀修好 —— 降级到栈上 `#alloca`（见《二、半成品》）。）

### 二、半成品：`struct` / `enum` 的值层

它们**不是**档位问题 —— handler 4/5 早已注册，声明会发出布局（`<T>__<f>_offset`、
`<T>__size`、变体的 tag 与载荷偏移），顶层 `let p = Pair { a: 1 b: 2 };` 会发出
`data p_storage rw { … }` 与 `#proc p() -> #addr`。**名字已经能用**：struct / enum 的名字可以写进参数类型与带标注的 `let`（见《一、已能用》的
「聚合类型名」行）。

**值层已定案：栈（`#alloca`）。** 函数体里的 `let q: T = T { … };` 与
`return T { … };` 已经降级成
`%r<n> = #alloca[#bits<8>](<size>)` + 每字段一条 `#store`（基址 `#lea(%r<n>, 0, 0, <off>)`），
绑定接 `%NAME = #lea(%r<n>, 0, 1, 0)`。选栈而不是「每个构造点一份静态 storage」，是因为后者
要先有带外顶层区（F 的一部分）、且递归会共享同一份存储（重入互相踩）；栈版本的代价是**值
逃不出帧** —— `#alloca` 从栈租约里 bump（`engine.c` 的 `op_alloca`：元素大小 × 常量 count，
16 对齐，越界报 `LAINVM_TRAP_STACK_EXHAUSTED`），帧弹出时水位退回 `stack_mark`
（`engine.c:926`），所以区域退出后地址失效，同 C 返回局部地址。今天**不检查**这种逃逸：
`return p;` 照样发出，返回的是已失效的栈地址。

值层剩下的是（每条都实测过，拒绝码一律 4，源在 build/probes/ 下）：

| 写法 | 实测 | 探针 |
|---|---|---|
| `return p.x;` | 4 | `fieldval.lain` |
| `let y: i32 = p.x;` | 4 | `fieldbind.lain` |
| `return p.x + 1;` | 4 | `fieldexpr.lain` |
| `let v: i32 = Shape.Circle(s);`（投影） | 4 | `proj.lain` |
| 函数体内 `let s: Shape = Shape.Circle { k };` | 4 | `varctorlocal.lain` |
| 顶层 `let c = Shape.Circle { 7 };`（对照，已能用） | 0 | `sumtoplevel.lain` |

字段访问与投影走同一条现成的形态判断：形状读给出 form 6（字段访问）时，
`bootstrap/std/funcs.l1:918` 把它记下来、`:938` 直接 `lain_meta_fail(4)`（函数的早退分支只接
form 5 的调用与 form 10 的构造），所以 `p.x` 无论出现在返回、绑定还是更大的表达式里都是 4。
还没有的是：投影（要读判別字段再选载荷，不能只看形状）、函数体内的变体构造（变体仍只走
顶层静态块）、字段值只能是「一个字面量」或「一个名字」（`a: p + 1` 只取 `p`）、以及上面
那条逃逸语义。顶层构造里写变量名会发出解析不到的
`%NAME`（顶层绑定是过程 `#call NAME()`，不是值）—— 由装载器拒。这些都不影响自举：手写
Meta 里 `#struct`/`#enum` 的命中数是 0。

### 三、待做（按阻塞顺序）

**A. 算子表** —— 不是语法，是 bootstrap/std/scalars.l1 里那段 `data` 字节表。
- 表里有 9 个标量名：`i32 u32 u64 i64 i8 u8 bool usize addr`。除 `i8`/`u8`（它们的用途是
  load/store 的宽度，不是算术）外，其余都绑定了算子（`bootstrap/lain/examples/ops.lain`、
  `bits.lain`、`widths.lain`）：
  - 32 位：`+ - * / % == != > <= >= < and or << >>`（i32 走 sdiv/srem/slt/sgt/sle/sge，
    u32 走 udiv/urem/ult/ugt/ule/uge）；
  - 64 位：同上一整列（`u64`/`usize` 走无符号那组，`i64` 走有符号那组）——`#bits` 4114 次、
    `#add` 471 次里的大宗就是它们；
  - `bool`：`== != and or`。
- 词算子 `and`/`or` 由词法层整词识别（`an` 仍是 4）；`<<`/`>>` 走双字节标点。
- IR 侧零改动：`#sub #mul #udiv #urem #eq #ne #ult #uge #and #or #shl #lshr` 都已实现。

**B. 宽度推导与转换** —— 「算子产生自己的结果类型」。`if x < 2` 不加括号、`let` 省标注这
一半**已完成**：条件是任意表达式（块 = 表达式停下来的那一格），无标注绑定的类型由值推
（`bootstrap/std/funcs.l1` 的 `meta_value_type_window` / `meta_decl_ret_window`，样例
`bootstrap/lain/examples/flow.lain`）。`as` 也**已完成**：变宽 `#zext`、变窄 `#trunc`、
等宽加零复制（`meta_emit_op_lit` 按 #data 地址发名字，样例 `convert.lain`）。负数字面量也
**已完成**：`-5` 发成 `#sub[repr](0, 5)`（样例 `neg.lain`）。十六进制也**已完成**：`0x`/`0X` 前缀由 `bs_read_uint` 按十六进制读，
整段必须是合法字面量（`0x` 报 4），宽度检查同样生效（样例 `hex.lain`）。**B 至此没有剩余项**。

**C. 内存与地址** —— Meta 的实际工作方式。
- `addr` 类型（已有）＋ `load` / `store` / `lea`：**前半完成** —— 三个内建名字可用
  （`meta_emit_load_temp` / `meta_lower_store` / `meta_emit_lea_temp`，宽度由期望类型定；
  样例 `bootstrap/lain/examples/memory.lain`，产物过 `lainir_parse` + `lainir_verify`）。
- `p + i`、`p + 1`：**已完成** —— 表达式路径按「符号是 `+` 且左操作数的类型是 addr」发
  `#lea(基址, 下标, 1, 0)`（`is_lea` 分支）。剩下 `p + i * K` 的 scale（降级是左到右折叠，
  没有优先级，要支持得先有表达式树）；`i + p` 报 4（只认左边的地址，否则会发出 `#add`
  拿地址当整数）。
- 字节串字面量 → `#data`（`#data_addr` 232）：**完成** —— 顶层 `let NAME: addr = "…";`
  发出静态块与取址过程（`meta_lower_static_bytes`，样例 `bootstrap/lain/examples/bytes.lain`，
  产物过 `lainir_parse` + `lainir_verify`）。函数体内的字节串报 4（`data` 只能落顶层）。
- 宿主授予**整块窗口**的能力（`region_grant`）：**完成** ——
  `lain_meta_region_grant(kind, index, dest)` 把 {地址, 容量, 权利} 三个 64 位字写进调用方给的
  24 字节位置（`bootstrap/std/regions.l1`；记录落在这个模块自己的 rw 数据块里，不占暂存区的
  固定格 —— 那些格是各遍的诊断/暂存区）。暂存区的基址与容量都从记录读回
  （`meta_region_scratch_base` / `meta_region_scratch_capacity`，分配器改用它），一次授予之后
  仍是普通 `#load`/`#store` —— 没有逐字节回调。今天只用 kind 3（暂存区）；0/1/2
  （Source / AstIn / AstOut）已定义、未使用。驱动摘要里的 `region grants=` 就是授予次数
  （每个样例 2 次：建表与重建各一次）。

**D. 控制流补齐**
- 带值 `if`（`#if` 779 + `#yield` 719）：**前半完成** —— 绑定位置的带值 if 已可用
  （`meta_lower_if_value`，样例 `bootstrap/lain/examples/choose.lain`，产物过 `lainir_parse` +
  `lainir_verify`）；尚缺：作为操作数或返回值出现（`return if …`、`1 + if …`），以及缺 `else` 时
  的默认分支语义。
- `loop` 带标签 + 循环参数（`#loop` 97 / `#break` 185 / `#continue` 104）：**完成** ——
  标签、任意个参数、任意条件、多语句体（`meta_lower_loop`）与带实参的
  `break` / `continue`（`meta_lower_loop_jump`）都已可用，样例
  `bootstrap/lain/examples/loop.lain`、`bootstrap/lain/examples/jumps.lain`，产物都过
  `lainir_parse` + `lainir_verify`。D 剩下的只有带值 `if` 的后半（当操作数/返回值、缺
  `else` 的默认分支）与**循环体里的 `if`**：有条件早退要先让循环体接受嵌套语句。

**E. 声明与链接**
- `extern NAME(a: T) -> U = link_name;`：**完成** —— 14 号 handler（`meta_register_extern` /
  `meta_declare_extern` / `meta_lower_decl_extern`，头由抽出来的 `meta_emit_proc_header` 发），
  声明身份与 `func` 一样是 6，调用点与返回类型推导完全共用一条路（样例
  `bootstrap/lain/examples/extern.lain`：4 条 extern + 3 个函数，产物过 `lainir_parse` +
  `lainir_verify`）。手写 Meta 的 46 个 `#extern` 里取 14 条最复杂的签名实测逐字符等价
  （build/probes/cap12.lain，matched=14 of 14）。**注意**：一次 lowering 请求的源码规模有既存上限 ——
  今天实测约 40 条合成 extern（或 13 条 9 参长签名）就开始报 4，多个源会**累积**，
  与 extern / func 无关，见《待补规则》。
- struct / enum 名字可写进参数类型与标注：**完成** —— 聚合类型在 IR 里就是 `#addr`
  （构造发 `#proc NAME() -> #addr`），所以 `p: Point` 的参数类型发成 `#addr`、
  `let q: Point = p;` 是一次地址复制（`#lea(%p, 0, 1, 0)`）；实现是
  `bootstrap/std/registry.l1` 的 `meta_tid_repr` 对 kind 2/3 直接报 addr
  （聚合记录里 size/align 占了 kind/width 那两个字）。样例 `bootstrap/lain/examples/types.lain`。
- 函数体内构造的 storage（见二）：**完成** —— 值层定案为**栈上 `#alloca`**：每个构造点发
  `%r<n> = #alloca[#bits<8>](<布局大小>)` 加每字段一条 `#store`，绑定接 `%NAME = #lea(%r<n>, 0, 1, 0)`、
  `return T { … }` 直接 `#return %r<n>`。实现是 `bootstrap/std/records.l1` 的
  `meta_lower_construct_local` 与拆开的 `meta_emit_store_head`/`_tail`/`_name`，
  入口在 `bootstrap/std/funcs.l1` 的 `meta_lower_stmt`（构造单独成句的早退分支）。样例
  `bootstrap/lain/examples/locals.lain`。

**F. 层 0：`lainir_*` 发射库** —— 见上文《层 0：发射层》。它是「用 Lain 写 Meta」真正缺的
那一层，也是 bootstrap/std/wire.l1 那套手写拼串的正式化。

用量证据（口径：bootstrap/SOURCE_ORDER 的 28 个文件，不数注释与示例）：`#bits` 4114、
`#call` 2994、`#if` 925、`#yield` 860、`#lea` 790、`#eq` 630、`#return` 628、`#add` 471、
`#data_addr` 343、`#break` 220、`#and` 175、`#ne` 173、`#continue` 120、`#ult` 112、
`#loop` 112、`#sub` 87、`#or` 84、`#mul` 67、`#proc` 407（定义数，= 验证器报的 subroutine 数）。

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

### 五、删除与瘦身（实测口径）

动手做「待做」之前，先看能拿掉什么。口径是静态调用图：根 = 三个 ABI 入口
（`lain_std_lower` / `lain_std_abi_version` / `lain_std_initialize`）加全部 46 个能力的
`#extern`；边 = `#call NAME`。全仓库 `#proc_addr` 与 `#call_indirect` 命中为 0，所以没有
静态图看不见的间接引用。

今天的规模（`build/verify_probe.exe` + 按 `SOURCE_ORDER` 逐文件统计）：28 个文件 / 12809 行 /
411 个 `#proc` 定义（= 验证器的 subroutine 数）/ 148 个 `data` 块；注释 1936 行。

**死 proc：46 个、1306 行（约 10%）。**

| 文件 | 个数 | 行数 | 是什么 |
|---|---|---|---|
| bootstrap/std/wire.l1 | 16 | 750 | 类型、值、过程引用的 wire 编解码与校验，env 值绑定 |
| bootstrap/std/eval.l1 | 6 | 195 | bytes 实参请求、emitted 请求、声明闭包与过程引用求值 |
| bootstrap/std/scope.l1 | 9 | 189 | 依赖闭包一套、`meta_scope_set_payload`、`ast_index_of` |
| bootstrap/std/parse.l1 | 7 | 93 | `meta_sem_put` / `meta_sem_annotate` / `meta_sem_binding` / `meta_sem_expansion` 一系 |
| bootstrap/std/registry.l1 | 6 | 45 | `meta_tid_owner` / `meta_tid_namespace` / `meta_tid_field_*` |
| bootstrap/std/modules.l1 | 2 | 34 | `meta_cstr_len`、`meta_slice_equals` |

最大的单条：`meta_record_type_from_field_list`@bootstrap/std/wire.l1:571（166 行）、
`meta_wire_validate`@bootstrap/std/wire.l1:171（119）、
`meta_eval_apply_decl_closure`@bootstrap/std/eval.l1:76（112）、
`meta_wire_validate_field_list`@bootstrap/std/wire.l1:84（87）、
`meta_scope_dependency_closure`@bootstrap/std/scope.l1:674（77）。

**别删**：bootstrap/std/funcs.l1 的 `meta_lower_stmt` / `meta_lower_if` / `meta_lower_for` /
`meta_lower_body` 是活的（`meta_lower_decl_func` → `meta_lower_body`）。

bootstrap/std/wire.l1 那一层只被死代码调用，但它是《层 0》的现存对应物（本文 :36、:239、
:278 三处如此描述），所以删它要同时改本文 —— 它是「第二步的存货」，不是垃圾。
bootstrap/std/eval.l1 的 bytes 与 emitted 请求同理：功能没接线，不是写错了。

**未被引用的 `data` 块 10 个**：`meta_kw_arrow`@bootstrap/std/lex.l1:107、
`meta_o1`@bootstrap/std/emit.l1:81、`meta_o5`@bootstrap/std/emit.l1:85、
`meta_s2`@bootstrap/std/emit.l1:226、`meta_s3a`@bootstrap/std/emit.l1:227、
`meta_s3b`@bootstrap/std/emit.l1:228、`meta_s4a`@bootstrap/std/emit.l1:229、
`meta_s4b`@bootstrap/std/emit.l1:230、`meta_reg_entry_off`@bootstrap/std/registry.l1:30、
`meta_reg_entry_size`@bootstrap/std/registry.l1:32。

**从未被调用的能力包装 7 个**（bootstrap/std/emit.l1 的 `#extern` 加 seed/src/meta/host.c
的登记，删要两侧一起，能力表 46 → 39）：`lain_meta_emit_data`@bootstrap/std/emit.l1:19、
`lain_meta_emit_length`@bootstrap/std/emit.l1:20、
`lain_meta_apply_emitted_bytes_request`@bootstrap/std/emit.l1:35、
`lain_meta_ast_root`@bootstrap/std/emit.l1:48、`lain_meta_ast_tx_release`@bootstrap/std/emit.l1:55、
`lain_meta_diagnostic_field`@bootstrap/std/emit.l1:284、
`lain_meta_apply_diagnostic_field`@bootstrap/std/emit.l1:285。

**其他杠杆**：注释 1904 行，最重的几个是 bootstrap/std/emit.l1 84/302（28%）、
bootstrap/std/parse.l1 192/764（25%）、bootstrap/std/types.l1 22/101（22%）、
bootstrap/std/recognize.l1 115/537（21%）、bootstrap/meta.l1 298/1532（19%）；
bootstrap/std/handlers/ 的 6 个文件共 320 行，可以并成一个。

**顺序**：先删未被引用的 `data` 块与未被调用的能力（无风险）；wire 那一层删还是留由设计定，
留就加一句「当前不可达」的注释。

### 六、结论

今天能跑通的最小闭环 = `func` / `return` / 调用 / `let`（标注可省）/ `if`（条件任意表达式）/ 带值 `if`（仅绑定位置）/ `for` / `loop`（标签 + 参数 + 多语句体 + 带实参的 `break`/`continue`）/
`load`/`store`/`lea`（宽度由期望类型定）/ `p + i`（→ `#lea`）/ 顶层字节串（→ `#data` + 取址过程）/
函数体与顶层的聚合构造（→ 栈上 `#alloca` + 每字段 `#store`，或顶层 `data`）/
宿主整块窗口授予（`region_grant`，暂存区的基址与容量从记录读回）/ `extern` 声明（`= link_name`，调用与 `func` 同路）/
算子表覆盖全部标量名（`i8`/`u8` 除外，见 A）/ 聚合类型名（struct、enum）进参数类型与标注。够做「一小段 Lain 端到端」，不够写编译器。到「能用 Lain 重写 Meta」还差
**D 的剩项（带值 `if` 当操作数/返回值、循环体里的 `if`）+ E + F**；A、B、C 已完成。

## 待补规则

- `vref` 要成为不透明标量（像 `addr`：不可算术、不可 `as`）。
- `load`/`store`/`lea` 的宽度只来自**期望类型**：无标注 `let b = load(p);` 与 `load(p);`
  单独成句都报 4，不猜（曾经拿本函数的返回类型顶上，发出过宽度错的 `#load`）。
- 收尾的「加零复制」不再走源码的算子表：地址用 `#lea(x, 0, 1, 0)`、其余定宽整数用
  `#add[repr](x, 0)`。`u8`/`i8`/`addr` 没有算子绑定，走算子表查 `+` 会报 6。
- `p + i` 只认 `+` 且**左操作数**是地址：`i + p` 报 4（换过去是 `#lea(1, %p, 1, 0)`，
  而按左边的整数类型发 `#add` 会拿地址当整数 —— 验证器 2005）。
- `p + i * K` 的 scale 还是 1：降级是左到右折叠，`p + i * K` 会先算成 `(p + i) * K`，
  在 addr 上报 6。要支持得先有表达式树。
- 字节串字面量只在**顶层** `let NAME: addr = "…";` 能用：产物是 `data NAME_storage ro { … }`
  加取址过程 `NAME()`。函数体里没有地方放 `data`，所以写在那里报 4；把整块的地址拿进函数
  要等 F（层 0 的带外顶层区）。底层用 `#data_addr` 引用静态块的值，**不能**在表达式里按裸名字
  引用（那会发成 `%NAME`）—— 顶层 let 绑定按值引用还是既存缺口。
- 期望类型的传递路径（见上）。
- `addr` 上的 `==` 给不给（层 0 有 `lainir_eq`）。
- 带值 `if` 只在**绑定位置**可用：`let x: T = if …` 走 `meta_lower_if_value`，`return if …`
  与 `1 + if …` 都报 4（它们要求值-if 也能当操作数/返回值）。缺 `else` 也报 4 ——
  区域的 `results` 要求每个出口都 `#yield`，没有隐式默认值。
- `loop` 的参数里**必须有一个叫 `acc`**，它提供循环结果的类型；没有报 4。
- `loop` 的标签是必需的（今天只记下来备用）：缺标签报 4。
- `loop` 的循环体只能给**自己的参数**赋值，别的目标报 4；体是多语句的（`for` 只有一条）。
- `loop` 的早退：`break LABEL(<值>);` 用循环结果类型当上下文，`continue LABEL(<实参…>);`
  用各参数的声明类型当上下文；两者都必须带标签，标签名不符报 4，实参个数不符报 4
  （`break` 恰好 1 个、`continue` 恰好等于参数个数），循环外写报 4，循环体里 `break` 之后
  的语句不再降级。值型循环的区域里必须留下一个 `#break`：只写落尾的 `continue` 也照样由
  收尾补上它，但如果循环根本不可能产出值，产物过不了自己的验证器（2006）。
- 循环体还不接受 `if`：跳出只能是无条件的。有条件的早退要等循环体支持嵌套语句，
  那是 D 的剩项（`if b == 0 { break scan(acc); }` 今天报 4）。
- 一元负号只认**字面量**（`-5`）：它占两个节点（负号 + 数字），操作数游标要跳两格；
  作用于变量（`-x`）报 4 —— 那需要一条真正的取负规则，不是记法。
- 负数字面量不做「装得下」检查（`meta_literal_fits` 只看源码里的裸数字）：`let b: u32 = -1;`
  发成 `#sub[#bits<32>](0, 1)`，按宽度取模得全 1，与机器上的补码一致。
- `as` 的目标类型**不回填**给后面的算子或下一次转换：链式 `x as i64 as i32` 的第二次
  只能按**上下文宽度**判方向（绑定的标注宽度），所以它可能发成加零复制而不是 `#trunc`；
  值仍然对（指令自己按宽度截/扩），但产物不是最直白的那条。
- `region_grant` 的记录落在**本模块的 rw 数据块**里（`data meta_region_scratch_record rw { … }`）：
  它不能放暂存区的固定格 —— 那些格被各遍当诊断/暂存区用（例如 `meta_scope_scan_root` 写
  1540 + src*32）。授予以**宿主状态**为准（能力返回 0，失败写在宿主状态里），所以 `scope_setup`
  授完先读一次 `lain_meta_status`。
- `extern` 只做声明：链接名原样搬进 `#extern "…"`，**不等于**宿主登记了对应能力 ——
  能力表是驱动调 `lainmeta_host_register` 建的，两者对不上要到调用时才炸。
- 聚合类型名（struct / enum）进参数类型与标注：聚合值在 IR 里报 `#addr`（构造发
  `#proc NAME() -> #addr`），所以 `p: Point` 的参数类型是 `#addr`，`let q: Point = p;`
  是一次地址复制（`#lea(%p, 0, 1, 0)`）。翻译点在 `meta_tid_repr`：聚合记录里
  word16/word24 是 size/align，本来会被当成 kind/width 解包出 `#bits<4>` 这种假类型。
- 声明体里字段/变体**空格分隔**（`x: i32` 然后换行 `y: i32`）：写成逗号分隔
  （`x: i32, y: i32`）会让字段走查把逗号当字段名起步，`meta_type_resolve` 拿不到类型报 5。
- 函数体内的聚合构造降级到**栈**：每个构造点发 `%r<n> = #alloca[#bits<8>](<布局字节数>)`
  （`#alloca` 的 count 是元素个数，元素类型是 `#bits<8>`，所以按字节；count 必须是常量，
  布局大小为 0 报 5，验证器另对常量 0 报 2024），每字段一条
  `#store[repr](<值>, #lea(%r<n>, 0, 0, <字段偏移>))`；绑定接 `%NAME = #lea(%r<n>, 0, 1, 0)`，
  `return T { … }` 直接 `#return %r<n>`。
- 同一 proc 里**每个构造点必须用自己的 `%r<n>`**：装载器 `region_lookup`（seed/src/vm/image.c）
  按名字取**第一个**匹配定义，重名会静默指向前一个（不报错、只算错）。临时编号来自暂存区
  +144 的计数器（`meta_next_temp`），所以顶层字面名 `%base` 与它们不冲突。
- 构造的字段值只认两种形态：字段值的首字节是数字 → 当字面量（`bs_read_uint`）；否则整段
  当**一个变量的名字**（发 `%NAME`）。所以 `a: p + 1` 只取 `p`、丢掉 `+ 1`；变体构造的载荷
  同理。要支持表达式得先有表达式树。
- 栈上构造的值**逃不出帧**：`#alloca` 从栈租约 bump（16 对齐，越界报
  `LAINVM_TRAP_STACK_EXHAUSTED`），帧弹出时水位退回 `stack_mark`（seed/src/vm/engine.c:926），
  区域退出后地址失效。`return T { … }` 与把参数地址返回都**照样发出**，今天不做逃逸检查。
- 顶层构造里写变量名会发出解析不到的 `%NAME`（顶层绑定是过程 `#call NAME()`，不是值）——
  由装载器拒。变体（enum）构造仍只走顶层静态块，函数体内的变体构造还未降级。
- 类型不符的绑定**不做检查**：`func bad(p: Point) -> i32 { let k: i32 = p; return k; }`
  会发出 `%k = #add[#bits<32>](%p, 0)`（`host_status=0`），但产物过不了 `lainir_verify`
  （2005 BAD_OPERAND_TYPE）。也就是说错误由**下游信任门**拒收、不会变成错误代码，
  但报错位置在产物而不是源码。原因：`meta_emit_close` 的复制只按上下文 kind 选 `#lea`/`#add`，
  不看被复制的值自己的类型（只有参数名能查到类型）。
- 一次 lowering 请求能吃的源码规模有既存上限（不是 extern 引入）：今天实测 —— 单个源里
  39 条合成 extern（2369 B）过、40 条（2430 B）报 4（诊断无位置）；12 条 9 参长签名（2270 B）
  过、13 条（2460 B）报 4；**多个源会累积**（4 个源各 10 条、合计 2480 B 也报 4；8 个源各 5 条
  同样）。两种形状的条数差很多但边界一致，所以按**声明展开的节点规模**算，约 550–650 个节点；
  具体机制（AstIn/AstOut arena、scratch 或 visited 预算）未定位，超出本轮范围。旧记录
  「一个文件里 20 条左右、22 条 extern 报 4、22 条 func 报 9401」是在 C 组改动之前测的，
  现在 45 条 func 也过，已不成立。

## 与现状的差距

- 标量已经是表：bootstrap/std/scalars.l1 里一段 `data` 字节（名字、kind、宽度、算符对），
  handler 6 与 bootstrap/std/handlers/scalar.l1 已删除，bootstrap/lain/std/prelude.lain
  也随之删除（语言里不再有标量声明，也就没有「先 import 一份 prelude」这一步）。
- 层 0 尚不存在；今天的对应物是 bootstrap/std/wire.l1 的手写拼串。
- 表面语言里的内存操作今天有 `load` / `store` / `lea` / `p + i` 与顶层字节串（→ `#data`）；
  宿主授予整块窗口的能力（`region_grant`）已接线，但只在 Meta 自己的 bootstrap 代码里用
  （bootstrap/std/regions.l1），表面语言还没有对应写法。
- 端到端验收源是 bootstrap/lain/examples/arith.lain：`host_status=0`、`output bytes=74`，
  产物是 `%r1 = #add[#bits<32>](1, 2)`（见 docs/development.md 的驱动一节）。
