# LAINIR

**状态**：草案 v0，未实现。
**范围**：LAINIR 的物理形态。不含语言语义，不含内存管理，不含 ABI。

规则一律以「机器上怎么做」为准。

---

## 1. 定义

LAINIR 是 Lain 的物理结构化 IR，描述两样东西：

- **机器真正有的动作**：值、运算、内存访问、控制转移。
- **为了不用 goto 所必需的最少结构**：区域、终结子、过程。

**生产者**：Meta。**消费者**：backend 与 VM。
LAINIR 里没有语言语义——函数、类型、模块、泛型、effect 全在 Meta 层。

## 2. 非目标

- 不是优化 IR。
- 不是源码表示（不做名字解析，不承载 hygiene，除源位置外不携带源信息）。
- 不含 ABI：不出现寄存器名、参数位置、栈布局、返回地址。
- 不含内存管理：地址的有效性、生命周期、所属空间由 VM 负责。
- 不含失败语义：没有 trap，没有异常，没有错误传播。
- 不含 UB。

## 3. 判据

任何进入 LAINIR 的规则必须满足三条之一：

1. 对应机器上的一个动作。
2. 能消灭一个分析。
3. 接管机器上那份要人肉维护的约定。

## 4. 类型

```text
ty ::= #bits(N)     N > 0。整数解释
     | #f(F)        IEEE 格式 F：16 / 32 / 64 / 80 / 128 …
     | #vec(N)      N > 0。打包车道解释；车道分解在算子上
     | #addr        地址
```

规则：

- 类型只描述**存储与搬运**：搬多少字节、这些位怎么解释。不含符号，不含语义。
- **准入判据：类型记录「解释类别」，也就是「哪些算子合法」。**
  类型说能吃什么，算子说怎么解释。

  | 东西 | 改变合法算子集合？ | 进类型？ |
  | --- | --- | --- |
  | 整数 / 浮点 / 打包车道 | 是（三套算子互不相交） | **进** |
  | 字节数 | 是（决定 load/store 的宽度与落位） | **进** |
  | 符号 | 否（`sdiv` 与 `udiv` 都要 `#bits`） | 不进 |
  | 车道分解 | 否（`vadd.i32x4` 与 `vadd.f32x4` 都要 `#vec<128>`） | 不进 |
  | 具体寄存器文件 | 否 | 不进 |

- **符号在算子上**：`#sdiv`/`#udiv`、`#slt`/`#ult`、`#zext`/`#sext`/`#trunc`。
  类型系统不区分 `u32` 与 `i32`；语言层的 `i32`/`u32` 是 Meta 的事。
- **浮点用格式，不用宽度**：`binary32` 与 `binary64` 是两种格式，
  80 位扩展与 double-double 差得更多。
- **车道分解在算子上**：车道数、车道宽度、元素解释都不改变字节数，
  和符号同理——它们只在合法算子之间做选择。
- **具体寄存器文件不进类型**：同一份位串可以落在不同寄存器文件里，由算子和 lowering 决定。
  ABI class 由解释类别推出：`#vec<128>` → SSE class，`#bits<128>` → INTEGER class，
  `#f64` → SSE class。不需要额外声明。
- **没有任何复合类型。** 数组、结构体、元组的布局完全是 Meta 的事，IR 只看见字节和偏移。
- **值的类型由定义它的操作决定**，不是独立标注。文本里写了必须一致。
- **搬运必须写出类型**：load、store、参数、返回值、`#alloca`。
- **宽度上限是一个机器字：64 位。** 位串和浮点超过 64 位需要**多字表示**，
  而那要求 VM 的值模型（现在是一个 64 位字）、引擎的 ALU、后端三处一起改。
  在改之前，规范里就不允许——验证器报 2026，后端也留了一道拒绝。
  **宽的整数是 Meta 的事**（用多个 64 位值拼），不是物理底座的事。
- **内存访问的宽度必须是字节可寻址的**（8/16/32/64）。所以「搬几个字节」
  永远在 8 以内，不需要多字搬运。
- **不允许上下文类型**：一个操作的类型不得由它的消费者决定。
- 没有 `unit`，没有 `never`：「没有结果」= 结果列表为空；「不返回」= 终结子的形式。

### 4.1 宽度写在哪

机器上宽度是**指令**写的：`add rax, 1` 里 `rax` 定了 64 位，立即数没有自己的类型，
只是被符号扩展塞进那个宽度。所以：

- **每条指令用类型实参写明它操作的类型**（方括号里）；**字面量不带类型**。
- **字面量的宽度取自它所在位置声明的类型**。有四类位置的操作数带字面量却没有
  类型实参，各自的来源是：

  | 位置 | 例子 | 宽度取自 |
  | --- | --- | --- |
  | 普通算子 | `#add[#bits<64>](%x, 1)` | 指令的类型实参 |
  | 区域终结子 | `#yield 1` | 区域声明的结果类型 `-> (#bits<64>)` |
  | 过程终结子 | `#return 1` | 签名声明的返回类型 |
  | 取址 | `#lea(%p, %i, 1, 0)` | 地址宽度（scale / offset） |
  | 循环参数初值 | `%i: #bits<64> = 0` | 参数声明的类型 |

  不这么定，装载器就没法给字面量造出一个有类型的值，后端也没法决定立即数编码多宽。
- **字面量只需要在那个宽度里表示得出来。**
- 过程参数与返回值在签名里写类型；区域的入口参数与出口结果在区域头上写类型。

```lainir
%i = #add[#bits<128>](%x, %y)            // 整数解释
%v = #vadd.i32x4[#vec<128>](%a, %b)      // 打包车道解释

// #add 只吃 #bits；向量算子只吃 #vec。跨类是类型错误。
```

## 5. 值

- 具名，**单赋值**。同一区域内重复定义是错误。
- 作用域是所在区域。跨区域不可见；值只能经区域入口参数和出口结果流动。
- 值不占存储。只有 `#alloca` 的结果指向内存。
- 名字不是物理资源：不暗示寄存器，不暗示栈槽，没有溢出问题。

分水岭是**有没有地址**：

```lainir
#proc f(%x: #bits<64>) -> #bits<64> {
  %t = #add[#bits<64>](%x, 1)      // 只是个值：不占存储，可以进任意寄存器或直接消掉
  %u = #mul[#bits<64>](%t, %t)
  #return %u
}

#proc slot_ref() -> #addr {
  %slot = #alloca[#bits<64>](1)    // 有地址：必须真的占一块稳定内存
  #store[#bits<64>](42, %slot)
  #return %slot
}
```

## 6. 指令

```text
%name = #op[TYPE](operands)              // 产出值
%a, %b = #op[TYPE](operands)             // 产出多个值
#op[TYPE](operands)                      // 不产出值
```

- **执行顺序 = 书写顺序。** 没有隐含顺序，没有未指定顺序。
- **操作数一律位置式**，没有具名操作数（机器上是 `add rax, 1`）。
- **类型实参写在方括号里**，含义按算子类别确定；属性（内存序、volatile）跟在类型后面，
  同一个方括号里。
- 操作数必须是值（名字或常量），**不得是嵌套操作**。
- 每个操作的语义自足：宽度、符号、溢出行为都在算子上。
- **允许一个算子产出多个结果**：机器上一条指令可以同时给出值和标志位。

嵌套只是文本糖，解析期展平：

```lainir
// 手写可以这样：
%s2 = #add[#bits<64>](%s, #zext[#bits<64>](#load[#bits<8>](#lea(%p, %i, 1, 0))))

// 解析后是四行，artifact 永远是这个形状：
%a  = #lea(%p, %i, 1, 0)
%b  = #load[#bits<8>](%a)
%w  = #zext[#bits<64>](%b)
%s2 = #add[#bits<64>](%s, %w)
```

糖只在 reader 层。**artifact 永远平坦**，canonical 文本平坦，固定点的逐字节比较稳定。

### 6.1 指令集合

| 类别 | opcode |
| --- | --- |
| 整数算术 | `add` `sub` `mul` `sdiv` `udiv` `srem` `urem` |
| 位运算 | `and` `or` `xor` `shl` `lshr` `ashr` |
| 浮点算术 | `fadd` `fsub` `fmul` `fdiv` |
| 整数比较 | `eq` `ne` `slt` `sle` `sgt` `sge` `ult` `ule` `ugt` `uge` |
| 浮点比较（有序） | `foeq` `fone` `folt` `fole` `fogt` `foge` |
| 浮点比较（无序） | `fueq` `fune` `fult` `fule` `fugt` `fuge` |
| 整数转换 | `zext` `sext` `trunc` `bitcast` |
| 浮点转换 | `fpext` `fptrunc` `fptosi` `fptoui` `sitofp` `uitofp` |
| 地址转换 | `int2ptr` `ptr2int` |
| 向量 | `vadd` `vsub` `vmul` `vdiv` `vand` `vor` `vxor` `vshl` `vshuffle` `vbroadcast` `vextract` `vinsert` `vcmpeq` `vcmpne` `vcmplt` `vcmpgt` |
| 原子 | `xchg` `cmpxchg` `rmw_add` `rmw_sub` `rmw_and` `rmw_or` `rmw_xor` |
| 取址 | `lea`（base + idx×scale + offset） |
| 内存 | `load` `store` `alloca` |
| 引用 | `data_addr` `proc_addr` |
| 调用 | `call` `call_indirect` |
| 阶段 | `eval`（标注在 `call` 上，见 §10） |
| 结构 | `if` `loop` `switch` |
| 终结子 | `yield` `break` `continue` `return` |

### 6.2 模块级声明

- **字面量**不是指令：常量直接作为操作数出现。
- **`data <sym>`**：模块级数据对象（符号 + 字节初值 + 只读/可写标记）。
  对应机器上的 `.rodata` / `.data` / `.bss`；加载时就在，有符号。
- **`data_addr <sym>`**：取该对象的地址，加载时解析。
- **`proc_addr <sub>`**：取子过程的地址。产出的是**普通 `#addr`**，和 `data_addr`
  同形——地址没有类型，能不能调用由它所在区段的权限说话。
- 和 `#alloca` 的区别：`data` 是静态的、有符号、加载时就在；`#alloca` 是运行时在栈上要的。

### 6.3 语义规则

- 结果类型由算子决定。比较的结果是 `#bits<1>`。
- **向量算子的车道分解写在算子名上**：`#vadd.i32x4`、`#vmul.f32x8`，
  照机器助记符的路子（`paddd`、`vaddps` 也是把车道拼在名字里）。
  类型实参只给总位数。具体取哪些算子取决于目标指令集的交集。
- **浮点比较分有序和无序两族**，因为机器上「无序」是可区分的状态
  （x86 `ucomiss` 用 PF 表示）。
- **checked 算术用多结果算子**：`%v, %o = #sadd_overflow[#bits<64>](%a, %b)`。
  机器上 `add` 顺手设 OF，值和标志出自同一条指令；也可以由 Meta 用比较拼。
- `#alloca[#bits<64>](N)` 分配 N 个元素的连续对象，N 是常量。
- **域外行为由 VM 确定性拒绝**，不是 UB。已知的域外情形：
  除数为 0；移位量 ≥ 宽度；越权地址；预算耗尽。

## 7. 结构

- **控制流只出现在指令位置**，不得出现在操作数位置。
- 区域形式：`#if`（then / else）、`#loop`、`#switch`。
- **区域可以产出值**，通过终结子。
- **循环头部一次声明两套列表**：入口与回边携带的状态，以及出口的结果。
  `#continue` 交回携带列表，`#break` 交出结果列表。
- `#switch` 无 fallthrough，case 值必须是常量；default 语义必须显式。
- 终结子之后不得再有指令。

**区域末尾落下 = 顺序离开这个区域。** 机器上一切都会往下走，回边是你手写的跳转：

- `#if` 的区域落下 → 到 `#if` 之后。
- `#loop` 的体落下 → **离开循环**（不是回边）。回边必须显式 `#continue`。
- `#switch` 的 case 落下 → 离开 switch。
- **产出值的区域，每条路径都必须显式交值**（`#yield` / `#break`）。
  机器上不存在「到不了」这回事，所以不为「不返回的调用」开洞；
  哪个调用不返回是 Meta 知道的事，由它去排。

区域产出值——join 就是这条指令的出口，所以不需要 phi，也不需要 dominance：

```lainir
#proc max(%a: #bits<64>, %b: #bits<64>) -> #bits<64> {
  %c = #sgt[#bits<64>](%a, %b)
  %r = #if %c -> (#bits<64>) {
    #yield %a
  } else {
    #yield %b
  }
  #return %r
}
```

循环——跨迭代状态声明在头部一处，出口值走 `#break`。
机器上「携带状态」就是你一直用着的那几个寄存器，这里只是把它写出来；
循环体里没有对 `%i`/`%s` 的赋值，所以不存在「这一点上的值是哪个赋值产生的」：

```lainir
#proc sum_bytes(%p: #addr, %n: #bits<64>) -> #bits<64> {
  %sum = #loop bytes(%i: #bits<64> = 0, %s: #bits<64> = 0) -> (#bits<64>) {
    %c  = #uge[#bits<64>](%i, %n)
    #if %c { #break bytes(%s) }
    %a  = #lea(%p, %i, 1, 0)
    %b  = #load[#bits<8>](%a)
    %w  = #zext[#bits<64>](%b)
    %s2 = #add[#bits<64>](%s, %w)
    %i2 = #add[#bits<64>](%i, 1)
    #continue bytes(%i2, %s2)
  }
  #return %sum
}
```

多路派发——每个 case 是一个区域，没有 fallthrough；「这是多路派发」这个信息
让后端能按 case 集合的形状在跳表和比较链之间选：

```lainir
#proc classify(%k: #bits<32>) -> #bits<32> {
  %r = #switch %k -> (#bits<32>) {
    case 0  { #yield 10 }
    case 1  { #yield 20 }
    default { #yield 0  }
  }
  #return %r
}
```

终结子集合：`#yield`、`#break`、`#continue`、`#return`。

## 8. 过程与调用

```text
#proc NAME(%p: ty, ...) -> ty { ... }
```

命名：语法关键字是 `#proc`；**代码里这个类型叫 `Subroutine`**（历史原因），不叫
`Procedure`/`Proc`。文档正文说的「过程」就是它。

- 过程 = 签名 + 一个区域。
- **调用约定由 IR 定义。IR 里不出现寄存器、参数位置、栈布局、返回地址。**
  同一份 IR 走不同目标。
- 帧、对齐、栈大小、被调用者保存：由 lowering 从过程体算出来。
- **尾调用不是一种形式**：机器上就是把 `call`+`ret` 换成一次 `jmp`，由 lowering 决定。
- **间接调用不做参数类型检查**：机器上 `call rax` 也不做。类型安全是 Meta 的事。
  但它自己**必须声明结果类型**（`#call_indirect[#bits<64>](%f, ...)`）——
  否则这个值的类型无从确定，验证器和后端都无从下手。参数类型仍然不查。
- **「让出执行」不是一种形式**：机器上就是一条陷入指令，剩下的全归外面。
  就是一次普通调用，帧留不留由 VM 决定。
- 「不返回」是被调用者的行为，不是 IR 的构造。

```lainir
#proc f(%x: #bits<64>) -> #bits<64> {
  %y = #call g(%x)                 // 不出现返回地址、帧、参数寄存器
  %r = #add[#bits<64>](%y, %x)     // %x 跨调用可用，不需要谁保存它
  #return %r
}
```

## 9. 内存

- load/store **必须写出类型**。
- **序写在操作上。** 不写 = 该目标最弱。集合：`relaxed` `acquire` `release`
  `acq_rel` `seq_cst`。
- **`volatile` 是访问的一个属性**（面向 MMIO：这条访问不能被动），和序并列写在方括号里。
- **合法访问的语义由 IR 定义；域外由 VM 确定性拒绝。** 这不是 UB。
- 地址是显式物理对象：内存操作只接受 `#addr`。
- 内存管理（有效性、生命周期、所属空间）完全由 VM 负责，IR 不参与。

区段按权限分堆，和链接器一样：`.rodata`（READ）、`.data`（READ | WRITE）、
`.text`（READ | CALL）。**`CALL` 就是代码段的可执行权限**，对应硬件上的执行位；
ASM 的指令里没有它，MMU 里有。`#addr` 不区分代码和数据，区分靠这张权限——
这就是 `#call_indirect` 的判据。

```lainir
%a = #load[#bits<64>](%p)                       // 不写序 = 该目标最弱
%b = #load[#bits<64>, acquire](%p)
%c = #load[#bits<32>, volatile](%mmio)
#store[#bits<64>, release](%b, %q)
```

## 10. 阶段：`#eval`

- `#eval` **是调用上的阶段标注，不是操作码**。语义：这个调用的结果在编译期已知。
- 标注放在**调用点**，因为机器上代码不知道自己在哪个阶段跑——
  同一段代码，你现在执行它还是留着以后执行，它自己不知道。什么时候跑是调用者决定的。
- 它是**编译器和 VM 之间唯一的接缝**。
- **产出普通物理值**，不产出代码。
- 存在区间：Meta 发出 → folding 消掉。**backend 永远看不到它。**
- **实参必须是编译期已知的**。相位单向：编译期不得依赖运行期。
- **失败不进 IR**：编译期执行失败是编译器的诊断。
- **预算与空间由 VM 提供**；能不能碰宿主 I/O 由宿主注入给 VM 的能力决定，不是 IR 的规则。
- IR 不区分值的阶段，也不管它的地址来自哪次执行。

```lainir
#proc table_size() -> #bits<64> { ... }      // 普通过程，运行时也能跑

#proc f(%n: #bits<64>) -> #bits<64> {
  %s = #eval table_size()
  %r = #add[#bits<64>](%n, %s)
  #return %r
}
```

folding 之后：

```lainir
#proc f(%n: #bits<64>) -> #bits<64> {
  %r = #add[#bits<64>](%n, 64)
  #return %r
}
```

## 11. 验证规则

一遍前向扫描即可判定：

1. 同一区域内名字不得重复定义。
2. 使用前必须已定义。
3. 终结子之后不得有指令。
4. 每条指令必须能确定宽度：字面量所在指令必须给出类型实参。
5. 操作数的类型必须与算子要求一致（跨解释类别是错误）。
6. 产出值的区域，每条路径都必须交值。
7. `#break`/`#continue` 的实参必须与目标循环的列表一致。
8. 搬运操作必须带类型。
9. 操作数必须是值，不得是嵌套操作。
10. `#if` 的条件必须是 `#bits<1>`；`#switch` 的 case 值不得重复。
11. 直接调用的实参数量必须匹配；间接调用不检查。
12. `#eval` 的实参必须是编译期已知的。
13. 位串/浮点的宽度必须满足 `0 < width <= 64`（2026 / 2027）。
14. 内存访问的宽度必须是字节可寻址的（8/16/32/64）（2013）。

**域外行为在两条路径上的物化**：VM 记 trap 然后拒绝（除零 1001、移位越界
1002、有符号除法溢出 1003）；编译产物里没有那条通道，所以后端**发检查**、
违反就终止。谁把这些检查消去（证明它们不会发生）还没有定——见 §13。

## 12. 文本形式

**canonical 的定义就是打印器的输出**：平坦（一条指令一行）、类型实参总是写出来、
整数用十进制、数据对象排在过程前面、每个模块项之间一个空行。

固定点：`print(parse(text)) == text` 对 canonical 文本成立。

```text
module   := data* proc*
data     := 'data' SYMBOL ('ro' | 'rw') '{' BYTE* '}'
proc     := '#proc' NAME '(' params? ')' ('->' result)? ( block | extern )
extern   := '#extern' '"' SYMBOL '"'
params   := param (',' param)*
param    := '%' NAME ':' TYPE
result   := TYPE | '(' TYPE (',' TYPE)* ')'
block    := '{' inst* '}'

inst     := results? '#' OPCODE typearg? tail
results  := '%' NAME (',' '%' NAME)* '='
typearg  := '[' TYPE (',' ATTR)* ']'          // ATTR: volatile / acquire / release /
                                              //       acq_rel / seq_cst / relaxed
TYPE     := '#bits<' N '>' | '#f<' N '>' | '#vec<' N '>' | '#addr'

tail     := operands                          // 普通算子
          | NAME operands                     // #call
          | ' ' SYMBOL                        // #data_addr / #proc_addr
          | NAME                              // 同上（无括号的写法不合法）
          | cond                              // #if
          | loop                              // #loop
          | ' ' operand (',' operand)*        // #yield / #return（不带括号）
          | ' ' NAME operands?                // #break / #continue（目标是标签）

cond     := ' ' operand ('->' '(' TYPE (',' TYPE)* ')')? block ('else' block)?
loop     := ' ' NAME '(' loopparams? ')' ('->' '(' TYPE (',' TYPE)* ')')? block
loopparams := '%' NAME ':' TYPE '=' operand (',' ...)*

operands := '(' (operand (',' operand)*)? ')'
operand  := '%' NAME | INT | FLOAT | '#' OPCODE typearg? operands
```

**输入糖**：操作数位置上出现 `#op(...)` 会被展平成一条独立指令，名字用
`_tmp<N>`（保留前缀），操作数变成对它的引用。这就是 §6 说的
「嵌套只是文本糖，artifact 永远平坦」。

```lainir
// 手写可以这样：
%r = #add[#bits<64>](40, #mul[#bits<64>](2, 1))

// 展平后：
%_tmp0 = #mul[#bits<64>](2, 1)
%r = #add[#bits<64>](40, %_tmp0)
```

## 13. 未决问题

1. **循环两套列表的写法是否可接受**（§7）。
2. **向量算子跨目标取哪个交集**（§6.1 给的是候选，`.i32x4` 是候选后缀语法）。
