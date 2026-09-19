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
std/types.l1   初代标准库：字节比较、暂存区单元、repr 的字节数与对齐
std/registry.l1 初代标准库：类型注册表（身份 / interning）
std/scope.l1   初代标准库：作用域表（名字 → 绑定，按范围解析）
std/parse.l1   初代标准库：语法树（切词、按括号分组、摊成一排格子）
std/modules.l1 初代标准库：源码注册表、import 解析、命名空间成员的 mangle
std/scalars.l1 初代标准库：标量声明、类型名解析、算子解析
std/records.l1 初代标准库：积类型（布局、构造、字段访问）
std/funcs.l1   初代标准库：普通函数（参数、返回类型、函数体、带实参的调用）
std/sums.l1    初代标准库：和类型（布局、构造、投影）
meta.l1        Meta 的三个入口与 v0 的语言规则
```

## v0 的语言

```lain
let NAME [: TYPE] = INTEGER;
let NAME : TYPE = INTEGER OP INTEGER;
```

`TYPE` 缺省时是 `#bits<64>`（老形式）。写了类型就去**声明**里查它——不带点的名字
查 root 环境（逻辑路径 `std::prelude`），带点的名字（`W.w32`）查被 import 的模块：

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

## 模块与 import

输入语言现在可以有多份源码。`import("逻辑路径")` 拿到的命名空间**不是物理值**，
是编译期绑定——所以 `let M = import(...)` **不产出任何 LAINIR**：

```lain
let M = import("std::math");
let main = M.answer;
```

```lainir
#proc std__math__answer() -> #bits<64> {
  #return 42
}
#proc main() -> #bits<64> {
  #return #call std__math__answer()
}
```

三件事值得记：

- **解析规则是全逻辑路径精确匹配，不做末段回退。** `import("std::math")` 规范化成
  `std/math.lain`（`::` 对 `/`，末尾补 `.lain`），然后和宿主注册的每一份源码的路径
  **逐字节比整条**。find 不到就是 11，不会退化成「谁的 basename 像就绑谁」。
  旧塔正是后者，已证实会把 `packages::lain::lainvm::api_contract` 静默绑到
  `src/lainir/api_contract.lain`。回归里有一条专门钉这条规则：模块按
  `other/math.lain` 注册、源码 import `std::math` → 必须 11。
- **命名空间成员要 mangle 成扁平名**才落得到物理层（LAINIR 的名字是平的）：
  `std::math` → `std__math`，再拼 `__` 和成员名。`::` 是两字节分隔符，mangle 成
  **两个**下划线——只发一个的话 `std::a::b` 和 `std::a_b` 会撞成同一个扁平名。
- **不建编译期符号表**：`let M = import(...)` 不留记录，用到 `M.x` 时再回源码里
  扫这条 import 找回来，和积/和类型重算布局是同一个办法。代价是 O(n²)。

注册表就是宿主那份源码列表：每份源码有一个**逻辑路径**，`import` 拿它匹配。
驱动必须**显式授权**每一份源码的文本**和路径**（`lain_meta_source_path_data`
返回的是宿主地址，不在 Meta 的映像里），少授权一次就是 trap 1004。

v0 的缺口：导入的绑定只支持 `let NAME = INT;` 这一种形状，repr 固定 `#bits<64>`；
类型还不能 import；没有 `export` 声明（模块里所有 `let` 都是导出的）；没有循环
检测；重名 import 绑定（`let M = ...` 两次）不报错，后一条会赢。

### 取用模块的成员时会**进入那个模块的作用域**

这是「模块内部互相看不见」那个静默缺陷的修法。以前取 `S.b` 是拿模块源码的字节
硬读一个整数字面量——`let b = a;` 读到字母 `a`，给 0，不报错。现在会：

1. 把「当前源码」切到那个模块（链从 `[root, 调用方]` 变成 `[root, 那个模块]`）；
2. 扫一次它的顶层绑定（**扫一次就留着**，见下）；
3. 在**那个模块**的作用域里展开成员：数字就是字面量，标识符就是同模块的另一条
   绑定——递归发出来，再发一条 `#call`。

```lain
// std/chain.lain
let base = 7;
let step1 = base;
let step2 = step1;
```

```lainir
#proc std__chain__base() -> #bits<64> { #return 7 }
#proc std__chain__step1() -> #bits<64> { #return #call std__chain__base() }
#proc std__chain__step2() -> #bits<64> { #return #call std__chain__step1() }
```

两条规则值得记：

- **调用方那一层不在模块的链里。** 链是 `[root, 那个模块]`，所以模块内部的绑定
  看不见调用方的绑定——否则就是动态作用域。这个是用「查找只认当前源码 + root」
  两个条件表达的，不需要通用的层级表。代价是它只够表达深度 2 的链；要有块级
  作用域就得换成真正的层级表。
- **模块的作用域扫一次就留着，不退。** 一开始我按「进出一层」的写法做了，结果
  `S.left` 和 `S.right` 各扫出一批新条目、`emitted` 标记全是 0，同一条绑定被发了
  两次——产物里两个同名过程，被 verifier 拒（2022）。留着条目之后第二次取用就
  看得见「已经发过」。留着不违反可见性（别人的条目看不见），代价是表随被取用的
  模块数增长，而容量本来就是按模块算的。

v0 的限制：模块里能取用的只有**值绑定**；取标量类型或别的报 4。模块内部的引用只
支持「同模块的另一条绑定」，不支持嵌套的限定引用（`let b = Other.x;` 里的 mangle
缓冲区会被覆盖）——那种情况现在报 4，不是静默给错值。

## 作用域表

上面那两节都在讲「名字怎么找到」。以前这件事的做法是**拿名字的字节回源码里搜一段
长得像声明的字节**，搜到就用。那里面没有「名字是在哪个范围里被绑定的」，所以：

- 搜的是整份文件，没有内层 / 外层之分；
- 没有一份「这个范围里有哪些名字」的清单，重名无从检测；
- 每个使用点都重搜一遍。

`std/scope.l1` 补的是**作用域结构**。一张表，条目记「哪个范围的哪个名字是什么」：

```text
ScopeEntry: source  name_off  name_len  kind  payload  aux  emitted
kind 1 = 模块引用（payload = 被 import 的源码下标，aux = 逻辑路径位置）
kind 2 = 标量类型（payload = 类型 id）
kind 3 = 值绑定  kind 4 = struct  kind 5 = enum
```

**查找从表尾往前**：表是按范围逐层追加的，所以「倒着找、第一个命中」天然就是词法
作用域的语义（最内层优先，再往外）。**重名只在本层里比**——同一个名字可以在内层
重新绑定（遮蔽），但不许在同一层绑两次，报 16。

### 条目记的是**声明节点下标**，不是「等用到再去源码里找」的位置

这是这张表被叫成符号表的原因，也是它真正省下来的东西：

```text
kind 2 标量    payload = 类型 id（0 = 还没 intern）  aux = repr 的节点下标
kind 4 积类型  payload = 声明的节点下标              aux = 0
kind 5 和类型  payload = 声明的节点下标              aux = 0
kind 1 模块引用 payload = 被 import 的源码下标        aux = 逻辑路径的位置
kind 3 值绑定  payload = 初始化式的节点下标           aux = 0
```

拿字段访问说：以前每写一次 `Pair.left(p)`，都要**回源码里把 `struct Pair` 那条声明
搜一遍**（`meta_find_rec_body`），再从头累加字段偏移。现在条目里就指着那条声明的
节点，读一下就有——`meta_scope_body` 是 O(1)。标量同理：`i32` 的条目指着它的 repr
节点，`meta_scope_type_id` 不用再搜。

**搜索式的名字查找全删了**：`bs_next_form`、`meta_find_keyword`、`bs_next_let`、
`bs_match_let`、`meta_import_at`、`meta_find_binding_in`、还有过渡形态
`meta_find_decl` / `meta_find_scalar` / `meta_find_rec_body` / `meta_find_enum_body`。
留在 `std/parse.l1` 的只有「把源码读成树」和「节点怎么读」——**找名字不是那一层的
事**。

步骤数（三轮累积）：

| 用例 | 走字节 | 走树 | 走符号表 |
| --- | --- | --- | --- |
| `struct: Mixed 大小 24` | 80144 | 58333 | **53860** |
| `sum: Shape 大小 16` | 79786 | 57480 | **54822** |
| `sum: Circle { 7 } 构造 + 投影` | 102398 | 84027 | **82158** |
| `product: Mixed.b(m)` | 127339 | 110661 | **102380** |

**注意还没到 O(1)**：作用域表自己的查找还是**线性扫一遍条目**。条目数等于声明数
（比源码字节数小得多），所以那部分不刺眼，但严格说还是 O(声明数 × 查找次数)。
真正 O(1) 要哈希或者排序索引——现在不值得，等有真实规模的输入再说。


层次：外层是 root 环境（`std::prelude` 那份源码），内层是被降级的源码。被 import
的模块取用时再压一层、用完退掉（`meta_scope_mark` / `meta_scope_release`）——那一
步还没接，所以现在带点的名字（`T.i32`）仍然回模块源码里搜声明，两条路以后会合成
一条。

表建在暂存区里，**容量按模块算**：声明数上界按最少字节数的声明估（`let a=1;` 是
8 字节，`scalar a = addr { }` 是 18 字节）。装不下报 17，不截断——截断会让「表里
没有」和「真的没有」分不开。

### 步骤数没有降，反而涨了

这是诚实的结果，得记下来：

| 用例 | 之前 | 现在 |
| --- | --- | --- |
| `sum: Shape 大小 16` | 47060 | **79786** |
| `sum: Circle { 7 } 构造 + 投影` | 152937 | **102398** |

预扫是 O(文件) 一遍，而它省下的每次查找本来是 O(文件)。所以**查找次数多才划算**
（构造+投影那个降了三分之一），一两次查找的用例纯粹是倒贴。

这一轮的收获不是速度，是**语义**：名字现在能按范围解析了，重名能报出来了。速度要
等模块压栈、查找变频繁、预扫被摊薄之后才回得来。

### 顺带：输入语言有注释了

做这件事时撞出来的：prelude 自己的注释里写着 `scalar` 这个词，而**输入语言当时没有
注释语法**，于是注释文字被预扫当成声明扫了进去——两条名字长度为 0 的假声明，直接
报重名。

所以 `bs_skip_space` 现在同时跳空白和注释（`#` 到行末）。这不是装饰：没有它，一份
源码里出现的 `scalar` 字样都会变成声明。

## 普通函数

`std/funcs.l1`。这是「Meta 用 Lain 写」的第一块地基：解析器的每个组件都是一个
**带参数的过程**，没有这一层，语言里连一个解析器都放不下。

```lain
func add(a: i32, b: i32) -> i32 { a + b }
let main: i32 = add(3, 4);
```

```lainir
#proc add(%a: #bits<32>, %b: #bits<32>) -> #bits<32> {
  %r = #add[#bits<32>](%a, %b)
  #return %r
}
#proc main() -> #bits<32> {
  #return #call add(3, 4)
}
```

跑出来 7。

**参数名直接当 LAINIR 的参数名用。** LAINIR 的过程参数是有名字的（`%a: #bits<32>`），
所以源码里的 `a` 到产物里还是 `a`，中间不需要槽位表。这是这一版最省事的地方。

函数的二元运算（`a + b`）复用 `let` 那条路的算子解析：**物理算子按返回类型查 op 表**。
`a + b` 返回 `i32` 时这是对的；真正的规则该看操作数类型，等有类型检查再说。

v0 的限制（都写在 `std/funcs.l1` 的文件头，别猜）：

- 函数体是**一个表达式**：`值` 或 `值 算子 值`。没有局部变量、没有语句序列、
  没有 `if` / `loop`。
- 参数类型只支持**一个词**（`i32`），不支持限定名（`T.i32`）。
- 函数体里的标识符**只按这张参数表解析**，不走作用域表——所以参数遮蔽不了外面的
  同名绑定，而且单表达式函数体里本来也没有「外面」。遮蔽要等块级作用域。
- 调用实参**原样搬运**：`f(3, 4)` 把 `3, 4` 抄进 `#call`。对字面量是对的；换成别
  的东西会产出引用未定义值的文本，被验证器拒。

最后一条有回归钉着：

```text
PASS  reject: 函数体引用未定义的参数 -> 验证器拒   （verify: 2002 undefined value `b`）
```

**注意这不是 Meta 报的错，是验证器报的**——产物是「合法文本但引用了未定义的值」，
正是「验证 ⇒ 可执行」那条线在起作用。

## 语法树

`std/parse.l1`。在这之前，「读源码」是拿字符找模式：跳空白、读标识符、匹配关键字
字节、猜一条形式到哪结束。它不知道程序是有结构的，所以每个使用点都要重搜一遍，
而且「`letter` 里的 let」「注释里的 scalar」这类东西会被当成声明——都真的出过。

现在源码先被读成**一棵树**：

```text
struct Pair { left: i32 right: i32 }
```

```text
#9  组   [ 0..37] 孩子: #0 #1 #8          ← 根：整份文件
  #0  词   "struct"
  #1  词   "Pair"
  #8  组   [12..36] 孩子: #2 #3 #4 #5 #6 #7   ← 一对花括号
    #2  词   "left"
    #3  词   ":"
    #4  词   "i32"
    #5  词   "right"
    #6  词   ":"
    #7  词   "i32"
```

树只有两种节点：**词**（一段连续的字母数字，或者一个标点，或者一个引号串）和
**组**（一对括号连同里面装的东西）。**它不认识 `let` / `struct` / `enum`**——那些
是 Meta 的事。正因如此这一层是跟语言无关的。

每个节点摊平成一格，40 字节、五个数：`种类 / 起点 / 长度 / 第一个孩子 / 下一个兄弟`。
「第一个孩子 + 下一个兄弟」两句话就能表达任意形状的树，不需要指针——正好合上
「Meta 手里只有整数、地址、和一块可写内存」这个约束。建法是**后序**的：孩子先追加，
父节点最后追加并记下第一个孩子的下标。

驱动有个 `tree` 模式把树打出来（`build/tmp-probe/run_all.ps1` 里有三条检查核对形状）。

### 形式发现和分派已经走树

这一步改了行为：**顶层形式从哪到哪、它是哪一种，都由树给出**，不再逐字节找关键字。

以前 `meta_lower_source` 和 `meta_scope_scan` 都是「从某个位置起，逐字节找下一处
长得像 `let` / `struct` / `enum` / `scalar` 的地方」。现在问的是「这个节点是不是一个
内容为 `struct` 的词」，而且一条形式从哪到哪由节点给出。

**注意顶层树的孩子是词，不是形式**——一条形式跨好几个孩子（`struct`、`Pair`、
`{...}`）。所以循环里跳过一条形式内部那些非关键字的词是正常的；但**整份文件的
第一个孩子必须是能被认出的形式**，否则报 4。这是「按形状认」和「按字节搜」的差别
落到实处的地方。

两条钉子钉着它：

- **注释里藏的 `let` 不算声明**。这是确认过的真 bug：`bs_next_form` 逐字节扫关键字、
  不看注释，而 prelude 的注释里写着 `scalar` 这个词——两条名字长度为 0 的假声明，
  直接报重名 16。现在注释连词都不是。
- **字段名叫 `letter`**（里面有 `let` 三个字母）照样只是一个词。这一条以前也过，
  但靠的是「关键字后面必须是空白」那个约束，而它不在分派那一侧——现在不靠它了。

还没换的：**一条形式内部**的扫描（字段列表、初始化列表、算子的 op 表）还是走字节。
它们现在拿到的是树给出的准确跨度，所以那类「注释/关键字误判」不会再发生在形式上，
但形式内部仍然按字节读。

### 按名字找声明也走树了

`meta_find_scalar` / `meta_find_rec_body` / `meta_find_enum_body` 以前都是「从头
逐字节搜关键字，名字不对就接着搜下一处」，每个使用点搜一遍。现在三个都走同一个
`meta_find_decl`：问「这个节点是不是内容为 `scalar` 的词」，名字是不是它的下一个
节点。地址先转成整数再比（`#ptr2int`），所以查找函数不用让调用方把源码下标传下来。

**搜索式的关键字匹配全删了**：`bs_next_form`、`meta_find_keyword`、`bs_next_let`、
`bs_match_let`、`meta_import_at`、`meta_find_binding_in`。净 264 行删掉、198 行加上。

留着的 `bs_match_keyword` 只剩两处，都是「在**已知位置**检查这几个字节是不是
`import`」——定点检查，不是搜索。出事的从来是搜索那部分。

代价与收益：

| 用例 | 之前 | 现在 |
| --- | --- | --- |
| `struct: Mixed 大小 24` | 80144 | **58333** |
| `sum: Shape 大小 16` | 79786 | **57480** |
| `sum: Circle { 7 } 构造 + 投影` | 102398 | **84027** |
| `product: Mixed.b(m)` | 127339 | **110661** |

降了 7%–28%。没有更多是因为「按名字找声明」本身还是**线性扫一遍形式**——只是现在
扫的是节点、不是字节。真正的 O(n²) 要等编译期符号表（名字 → 声明）建起来才算还清。

### 工作区大小由编译单元决定

两个踩过的坑，都留在代码注释里：

- **递归会覆盖出参单元**。`meta_parse_seq` 用固定单元传出「这一层的孩子起点」，
  而递归调用会写同一个单元——父节点一度被接到了自己孩子的链子里，组节点的孩子
  列表里出现了它自己。修法是递归前把本层的累加器存起来、读完结果再放回去。
- **组要吃掉自己的闭括号**。内层停在 `}` 上，外层得从它后面接着走，跨度也要把
  闭括号算进去。不然根节点会把 `}` 当成一个普通的词收进孩子里。

### 工作区大小由编译单元决定

宿主那块可写内存以前写死 64 KiB。加了树之后不够了——树是每个 token 一个节点，
节点数和源码字节数同一个量级。写死大小的后果不是「慢」，是编译一份稍大的源码就
报「装不下」，而那是宿主的资源配得不对，不该让用户看见。

现在按已登记源码的总字节数给（每字节 128 字节，保底 64 KiB），**按需分配**——要等
源码都登记完再拿。这只是资源配额，不是语言规则：Meta 仍然自己算它要多少，装不下
照样报 17。

## 类型身份：注册表

在这之前，「类型」是一次**回源码重算**的结果：给出名字，扫一遍源码，算出一个 repr。
两个 repr 之间没有「是不是同一个类型」可言——同一个名字问两次，是两个互不相干的
重新计算。签名、抽象、模块实例全都建立在「类型有身份」之上，所以这一层得先有。

`std/registry.l1` 补的就是它。类型是一条被 **interning** 的记录：

```text
TypeValue: id  kind  repr_kind  repr_width  owner  namespace  arg_start  arg_count
key      = (kind, owner, namespace)          // v0 没有参数化类型，实参区恒空
```

`owner = ((源码下标 + 1) << 32) | (声明位置 + 1)`，`owner = 0` 留给合成类型
（没写类型标注时的默认 `#bits<64>`）。id 从 1 开始。这套形状是照正式设计抄的
（`std/meta.lain` 的 `TypeValue` / `intern_type`：那边的 key 是
`(kind, nominal_owner, namespace, arguments)`，注册表是 values/arguments/modules
三个向量；这里 v0 只留 values，实参区先空着）。

**repr 不进 key，这是关键。** `i32` 和 `u32` 的 repr 都是 bits/32，它们却是两个
类型（op 表不同）。让它们不同的不是 repr，是 owner——owner 指的是**哪一条声明**，
不是哪一份源码。所以身份是**名义的**，来自声明，不是结构的。

回归里那两条断言钉着这件事：

```text
PASS  tid: i32 用两次 -> 1 条                    （同一条声明 → 同一个 id）
PASS  tid: i32 + u32 -> 2 条，repr 相同          （repr=0/32，owner 不同）
```

想看见类型 id，得看**编译期状态**——产出的 LAINIR 里没有它。驱动把注册表读出来
打印（`types: N registered`，模式 `types` 时列全表）。这是 harness 和 Meta 之间
唯一的内部耦合，就三个偏移量，在 `meta_boot.c` 顶上写着。

**和正式设计的一处偏离**：正式设计里 `TypeValue` **不带物理形状**（注释写的是
"their physical ABI shape is intentionally absent here; lowering chooses that
later"），这里为了 v0 把 repr 放在同一条记录里——因为现在唯一的类型种类就是标量，
而标量的定义就是它的 repr。等有了结构类型，形状就该挪出去，由 lowering 决定。

## 标量：从字节表到声明

标量**曾经**是一段手打的静态字节表（`data meta_types`）——加一个类型要改字节，
错一个字节就产出错的 repr。现在它是**声明**，住在源码里：

```lain
scalar i32 = bits<32> { "/" = sdiv "+" = add }
scalar u32 = bits<32> { "/" = udiv "+" = add }
scalar addr = addr { }
```

repr 的四种写法对应 LAINIR 的四个类型构造子：
`bits<N>`=0、`f<N>`=1、`vec<N>`=2、`addr`=3。op 表是「源码里的符号 → 要发的物理
算子」——`i32` 和 `u32` 的 repr 一样，差别全在 `"/"` 那一行，Meta 里没有一行写着
「如果是 i32」。

**root 环境就是一份源码**：逻辑路径 `std/prelude.lain`（仓库里是
`seed/lain/std/prelude.lain`）。不带点的类型名去那里查。旧塔的正式语言里 `i64`
也不带点，所以保留这个形状：

```lain
let a: i32 = 12 / 3;              // i32 → prelude
let W = import("std::widths");
let b: W.w32 = 12 / 3;            // W.w32 → 被 import 的那份源码
```

解析和 struct/enum 一样是**回源码里扫声明**，不建编译期类型表。被降级的那份
源码自己写 `scalar` 会被拒（14）——只有 prelude 和被 import 的模块参与解析，
这样「写了个不会被看到的声明」不会静默生效。

v0 不认注释、不认识换行以外的排版差异（空白 = 字节 ≤ 32）、没有 `expand`
阶段（还没有宏）。这些是**缺口，不是设计**。

## 宿主状态是诊断通道

深层的辅助过程（算布局、查算子）只能回一个打包值，没法把状态带上来。它们用
`lain_meta_fail` 把码记在**宿主状态**里，驱动必须看。不看会出事——实测过一次：
类型名没解析出来，布局照算，`__size` 静静变成 62，一路跑到产物里才发现不对。
现在驱动的这条检查在回归里由「没注册 prelude → 5」钉着。

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
| `lain_meta_source_path_data` | index | 逻辑路径地址（NUL 结尾，import 的注册表） |
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
# 参数：<src> <入口> <期望值> [断言文本] [模式] [逻辑路径=文件]...
build/tmp-probe/meta_boot.exe seed/tests/meta_import2.lain main 42 std__math__answer - \
  std/math.lain=seed/tests/modules/std/math.lain
```

模块从第 7 个参数起，写成 `逻辑路径=文件`。逻辑路径必须是**规范化之后**的样子
（`std/math.lain`），因为解析是全路径逐字节匹配。真驱动会从一个模块根递归收集
`.lain` 自动算相对路径；这里显式给，免掉目录递归，也让注册表的内容在命令行上
看得见。

`build/tmp-probe/run_all.ps1` 把两条都纳入回归。

## 下一步该长什么

按依赖顺序：

1. **函数体长大**：局部变量、语句序列、`if`、`loop`。没有这些，函数体只能是一个
   表达式，写不出解析器那样的过程。
2. **`expand` 阶段**：现在还只是恒等，没有宏/attribute。
3. **值也能带类型过模块**：现在 import 只导出 `let NAME = INT;`（repr 固定
   `#bits<64>`）。
4. **`p.left` 这种写法**：要一张「值名 → 类型名」的表——作用域表已经能装，缺的是
   把值绑定的类型也记进去。
5. **AST 面**：已经做了一半（`std/parse.l1` 把源码读成树，形式发现和分派都走树）。
   还没换的是**形式内部**的扫描：字段列表、初始化列表、算子的 op 表仍然按字节读。

硬编码清单（还没还的债）：顶层形式 `let`/`struct`/`enum`/`scalar`/`func` 是**按形状
认**了（不再是字节搜索），但形状本身写在 `meta_lower_form` 的 if 链里；`struct`/`enum`
的分隔符是手打的 `__`；root 环境的名字 `std::prelude` 是写死的一条逻辑路径。


顺手记一条**已经还掉的**：`i32` 曾经是编译器里写死的名字（一段手打字节），
现在它只是 prelude 模块的一条 `scalar` 声明——没注册 prelude 就没有 `i32`，
回归里那条「没注册 prelude → 5」钉着这个事实。
