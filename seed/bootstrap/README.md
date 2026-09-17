# seed/bootstrap —— 初代 Meta 与标准库（手写 LAINIR）

这一层存在的唯一理由是**打破鸡生蛋**：

> Meta 是编译器里「知道哪些词是什么意思」的那部分，它必须能用 Lain 自己写、
> 必须能被整份替换掉。但要用 Lain 写它，先得有一个 Lain 编译器。

所以第一代 Meta **不是用 Lain 写的**，而是用手写 LAINIR 直接写出来 —— 物理 IR
是唯一的底座，它不需要编译器就能被 seed 执行。这一层把语言规则跑起来，等它
能把 Lain 源码编成 LAINIR，就可以用它把 Meta 换成 Lain 版，然后丢掉它。

## 与归档里的旧塔无关

`seed/archive/` 里那份第一代实现（`lainir/compiler.l1` + `bootstrap/compiler/*.l1`）
是**旧方言**：语句 + 表达式树、`#let` 可变绑定、`#unit`、51 个 `bootstrap.ir-*`
IR 内省能力。这一层是新方言、从零重写的，两者不共享任何代码，也不需要兼容。

对照（实测的迁移面，供参考）：

| 形状 | 旧塔 | 新方言 |
| --- | --- | --- |
| 绑定 | `#let %x: T = v`（2571 处） | `%x = #op[...](...)`，单赋值 |
| 重新赋值 | `%x: T = v`（846 处，98.6% 跨构造写） | 循环携带参数 / 区域 `#yield` |
| 类型 | 6 种（含 `#unit`/`#never`/`#simd`） | 4 种：`#bits<N>` `#f<N>` `#vec<N>` `#addr` |
| 算子 | 9 种语句 + 42 种表达式树 | 89 种扁平算子，类型实参**必写** |
| 表达式 | 嵌套 | 无表达式；输入糖会自动摊平成 `_tmp<N>` |
| `#extern` | `#extern #proc name(...)` | `#proc name(...) #extern "link_name"` |
| `#break`/`#continue` | 带括号 | 带括号（`#yield`/`#return` 不带） |

## 目录

```text
SOURCE_ORDER   链接顺序；驱动按它把文件拼成一份文本再解析（确定性来源）
std/lex.l1     初代标准库：字节与词法（空白、标识符、十进制数、关键字）
std/emit.l1    初代标准库：宿主 ABI 声明 + 产物输出 + repr 的文本
std/types.l1   初代标准库：类型表 + 查表
meta.l1        Meta 的三个入口与 v0 的语言规则
```

## v0 的语言

```lain
let NAME [: TYPE] = INTEGER;
let NAME : TYPE = INTEGER OP INTEGER;
```

`TYPE` 缺省时是 `#bits<64>`（老形式）。写了类型就去**类型表**里查它：

```lainir
#proc NAME() -> <TYPE 的 repr> {
  #return INTEGER
}
```

右值写成 `整数 OP 整数` 时，`OP` 是拿去**该类型的 op 表**里查的：

```lainir
#proc NAME() -> <repr> {
  %r = #<查到的物理算子>[<repr>](LHS, RHS)
  #return %r
}
```

于是

```lain
let a: i32 = 12 / 3;
let b: u32 = 12 / 3;
```

产出两条**不同的**指令：

```lainir
%r = #sdiv[#bits<32>](12, 3)     // i32
%r = #udiv[#bits<32>](12, 3)     // u32
```

而 Meta 里没有一行写着「如果类型是 i32」——它只是查了表。**这就是这张表存在
的理由**：语言规则住在数据里，不住在编译器的分支里。

v0 的 i32/u32 各带两个 op（`/` 和 `+`），别的类型一个都没有。写一个表里没有的
算子（比如 `i32` 上的 `%`）会得到状态码 6，不是静默用别的算子顶上。

## 积类型

```lain
struct Mixed { a: i8 b: i64 c: i8 }
```

Meta **算**出每个字段的字节偏移和整个结构的大小，并给每个字段发一个过程：

```lainir
#proc Mixed__a_offset() -> #bits<64> {
  #return 0
}
#proc Mixed__b_offset() -> #bits<64> {
  #return 8
}
#proc Mixed__c_offset() -> #bits<64> {
  #return 16
}
#proc Mixed__size() -> #bits<64> {
  #return 24
}
```

那三个数字是**加出来的**：`a` 在 0；`b` 是 `i64` 要 8 对齐，所以填到 8；`c` 在 16；
总大小按最大对齐（8）向上取到 24。**这不是查表查来的**——积类型和标量的区别
就在这里：标量的 repr 是表里写的，积类型的布局是算的。

规则现在是「自然对齐」：大小 = 位数/8（不足一字节当一字节），对齐 = 大小，
`#addr` 是 8/8。真正的 ABI 对齐规则比这复杂，v0 先这样。

**构造和字段访问**（v0 记法：记录名和值名都写出来）：

```lain
struct Pair { left: i32 right: i32 }
let p = Pair { left: 3, right: 4 };
let s: i32 = Pair.left(p);
```

```lainir
data p_storage rw { 0 0 0 0 0 0 0 0 }

#proc p() -> #addr {
  %base = #data_addr p_storage
  #store[#bits<32>](3, #lea(%base, 0, 0, 0))
  #store[#bits<32>](4, #lea(%base, 0, 0, 4))
  #return %base
}

#proc s() -> #bits<32> {
  %b = #call p()
  %b2 = #lea(%b, 0, 0, 0)
  %v = #load[#bits<32>](%b2)
  #return %v
}
```

三件事值得记：

- **构造不是一条指令**，是「一块静态存储 + 每个字段一条 `#store`」。这和标量的
  「一个算子一条指令」是两种形状——这正是为什么 operator 条目不能只是
  `(符号, 指令)`。
- **字段访问是偏移**：`Pair.left` 那个 `0` 是声明时累加出来的，`#load` 的宽度
  也来自字段类型。
- **`p.left` 这种写法现在不支持**。它需要一张「值名 → 类型名」的表；v0 里
  记录名是写出来的（`Pair.left(p)`）。这是缺口，不是设计。

还有一个权宜之计：降级字段访问要**编译期**知道偏移和宽度，而声明时算出来的
只有那几个 offset/size **过程**（运行时才叫得动）。所以 `std/records.l1` 是
**回源码里再走一遍那条 struct 声明**来重算的——慢，但不需要建记录表。真正的
做法是一张编译期符号表。

顶层形式现在可以有多条：`std/lex.l1` 的 `bs_next_form` 扫关键字找下一条。
它不看上下文，所以字段名叫 `letter` 之类的会被误判——v0 的输入里没这种名字，
真做法是让每条降级返回「这条形式到哪结束」。

## 和类型

`std/sums.l1`。规则（v0）：tag 固定 `#bits<8>` 在偏移 0，载荷区从
`align_up(1, 最大对齐)` 开始，所有变体共用，总大小按最大对齐取整。
每个变体**最多一个**载荷字段。

```lain
enum Shape { Empty Circle(i32) Square(i64) }
```

```lainir
#proc Shape__Empty_tag() -> #bits<64> { #return 0 }     // 无载荷 → 不发 _off
#proc Shape__Circle_tag() -> #bits<64> { #return 1 }
#proc Shape__Circle_off() -> #bits<64> { #return 8 }
#proc Shape__Square_tag() -> #bits<64> { #return 2 }
#proc Shape__Square_off() -> #bits<64> { #return 8 }
#proc Shape__size() -> #bits<64> { #return 16 }
```

载荷起点是 8（最大对齐来自 `i64`），`Circle` 的 `i32` 放得下，所以两个变体
偏移相同、总大小 16。上面缩写了；实际产出每条过程都占 3 行。

和积类型同源——布局都是 `align_up` 累加出来的——多出来的只有**判别字段**。
所以 `std/sums.l1` 也沿用「回源码再走一遍」的做法，不建表。

**构造和投影**：

```lain
enum Shape { Empty Circle(i32) Square(i64) }
let c = Shape.Circle { 7 };
let v: i32 = Shape.Circle(c);
```

```lainir
data c_storage rw { 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 }

#proc c() -> #addr {
  %base = #data_addr c_storage
  #store[#bits<8>](1, #lea(%base, 0, 0, 0))
  #store[#bits<32>](7, #lea(%base, 0, 0, 8))
  #return %base
}

#proc v() -> #bits<32> {
  %b = #call c()
  %t = #load[#bits<8>](%b)
  %m = #eq[#bits<8>](%t, 1)
  %r = #if %m -> (#bits<32>) {
    %p = #lea(%b, 0, 0, 8)
    %v = #load[#bits<32>](%p)
    #yield %v
  } else {
    #yield 0
  }
  #return %r
}
```

构造和积类型同形（一块静态存储、一条 tag 存、一条载荷存），投影没有对应物：
积类型的字段访问是**一条 `#load`**（偏移编译期就知道，字段一定在），和类型的投影
必须**先看判别字段**——别的变体共用同一块载荷区。v0 把不命中折叠成 0（全函数）。

`A.b` 里的 `A` 是 struct 还是 enum，靠 `meta_is_enum` 按名字回源码找 `enum` 声明
来分——这决定走「字段访问」还是「变体构造／投影」，两者形状完全不同。

两条错误路径是有状态码的，不是静默的 0：投影无载荷的变体 → 10，投一个声明里
没有的变体 → 9。

**还没接的**：`p.left` 这种写法（要先有一张「值名 → 类型名」的表），以及和类型的
`#switch`——投影现在是「比一次 tag」，变体多了应该换成分派。

## 类型表

在 `std/types.l1`，是一段**静态字节**：不需要 init 过程，也没有手算的偏移
——条目自描述，Meta 走一遍就查到了。

```text
类型:  [1]名字长度 [L]名字 [1]repr kind [1]repr width [1]op 数 [op 条目 × op 数]
op:    [1]符号长度 [S]符号 [1]物理名长度 [P]物理名
```

repr kind 就是 LAINIR 的四个类型构造子：`0=#bits<N>`、`1=#f<N>`、
`2=#vec<N>`、`3=#addr`。表以「名字长度 0」终止。

v0 表里有 `i32 u32 i64 i8 u8 bool usize addr` 八项。**表少一个字节就会产出错的
repr**（少 ops 数字节时，下一项的长度会被当成 ops 数，走表直接跳飞），所以
驱动带了一条「产物里必须出现某段文本」的断言守着它。

v0 不认注释、不认识换行以外的排版差异（空白 = 字节 ≤ 32）、没有 `expand`
阶段（还没有宏）。这些是**缺口，不是设计**。

## 机制：谁提供什么

| 谁 | 提供什么 |
| --- | --- |
| 驱动（`seed/tests/meta_boot.c`） | 把 bootstrap 源拼成文本、解析/验证/装载、登记能力、**显式授权源码地址**、把产出的文本再执行一遍 |
| seed 底座 | LAINIR 的 parse/verify/load/engine，能力表机制，`data` 段与 VSpace |
| 宿主服务（`seed/src/meta/host.c`） | 源码读入、产物写出、失败上报 —— **不含任何语言知识** |
| Meta（本目录） | `let` 是什么、`NAME` 是名字、`INTEGER` 是数值、这些怎么变成 LAINIR |

### 能力面（9 个）

| 名字 | 参数（第一个永远是 host 地址） | 结果 |
| --- | --- | --- |
| `lain_meta_source_count` | — | 源码份数 |
| `lain_meta_source_data` | index | 源码字节地址 |
| `lain_meta_source_length` | index | 字节数 |
| `lain_meta_source_path_data` | index | 路径地址 |
| `lain_meta_emit_reset` | — | 0 |
| `lain_meta_emit_write` | addr, length | 0 |
| `lain_meta_emit_data` | — | 产物文本地址 |
| `lain_meta_emit_length` | — | 产物字节数 |
| `lain_meta_fail` | code | 0 |

名字同时是 link_name，所以**必须是合法 C 标识符**（SYMBOL 策略下后端按它发
外部符号引用）。旧 seed 用 `bootstrap.source-count` 这种短横线名字，在新设计下
后端发不出来。

### 没有全局状态

宿主 ABI 不带 `user_data`（带了编译产物就得从一个全局去读它）。所以
**host 的地址是一个显式输入**：驱动把它作为 `#addr` 传给 `lain_std_lower`，
Meta 原样作为每次能力调用的第一个参数传回来。这就是文档里
`lain_std_initialize(context)` 那个 context。

### 地址必须显式授权

Meta 的 TCB 不自带地址空间，源码字节也不是它自己映像的一部分。驱动必须先把
源码范围登记进 VSpace，否则 Meta 第一次 `#load` 就被确定性拒绝（trap 1004）。
这是设计要的行为：**没有地址授权就别假设临时 TCB 有可用的编译期地址。**

## 怎么跑

```text
clang -std=c11 -Iseed/include -o build/tmp-probe/meta_boot.exe seed/tests/meta_boot.c \
  seed/src/core/*.c seed/src/vm/*.c seed/src/meta/host.c
build/tmp-probe/meta_boot.exe                                   # let main = 42
build/tmp-probe/meta_boot.exe seed/tests/meta_source2.lain answer 1234
```

`build/tmp-probe/run_all.ps1` 把两条都纳入回归。

## 下一步该长什么

按依赖顺序：

1. **`expand` 阶段**：现在还只是恒等，没有宏/attribute。
2. **注释与真正的词法**：v0 只跳空白。
3. **多源码与模块**：`source_count` 已经在能力面里，Meta 还没用。
4. **表达式与类型**：v0 只会 `let NAME = INT;`。
5. **AST 面**：Meta 现在直接扫源码字节。真正的设计里，Parser 出无语义的
   RawAst，Meta 用语言中立的 AstApi（拓扑/位置/上下文）操作它 —— 那需要给
   底座加一层 AST 对象模型，是**新的一大片**，不在 v0 里。
