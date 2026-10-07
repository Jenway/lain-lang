# LAINIR

> [!QUOTE]
> C 有许多角色，其中重要的一个是结构化汇编与事实上的统一 ABI。
>
> 大部分的 C 语言的 “现代竞争者” 认为 C 语言缺少了太多东西，于是增加了更多的 feature 并称之为 better C。
>
> Lain-IR 的观点是，在这种角色下，C 的特性恰恰是太多了以至于不能准确的对应到现代硬件上来。

## 值与类型

| 类型  | 含义 |
| --- | --- |
| `#bits<N>` | 整数|
| `#floats<N>` | 浮点数|
| `#vec<N>` | SIMD 向量表示|
| `#addr` | 物理地址|

load/store 的位串和浮点宽度须为 8、16、32、64；地址另按地址表示处理。

值由定义它的操作确定类型。普通算术显式写类型实参；字面量按所在位置解释：

| 位置 | 类型来源 |
| --- | --- |
| 普通算子 | 算子的类型实参 |
| 过程 return | 过程结果签名 |
| 区域 yield、循环携带值和结果 | 对应区域声明 |
| 直接调用实参 | 被调过程参数签名 |

## 模块、过程与外部声明

模块包含数据对象和过程，符号按模块解析。

```lainir
data message ro { 104 105 0 }

#proc write(%bytes: #addr, %length: #bits<64>) #extern "write"

#proc add(%a: #bits<32>, %b: #bits<32>) -> #bits<32> {
  %sum = #add[#bits<32>](%a, %b)
  #return %sum
}
```

数据对象用 `ro` 或 `rw` 声明只读或可写。数据初值是字节序列。
LAINIR 的数据对象只记录物理字节与访问属性；源语言的字符串、常量和记录语义须先由 Meta 转成布局与字节。
extern 的 link_name 指定宿主符号。解释器通过能力表解析，后端通过相应链接接口调用。
物理参数和结果由 IR 签名规定；寄存器、栈传参和目标调用约定由后端实现。

## 绑定与操作数

绑定为单赋值：`%name = #op(...)`。同一区域不能重复定义名字，使用前须已有定义。
值本身没有可取的存储地址；需要地址时使用数据对象或 alloca。

```lainir
%sum = #add[#bits<64>](%x, 1)
%slot = #alloca[#bits<64>](1)
#store[#bits<64>](%sum, %slot)
```

以上是过程体片段。操作数按位置传递。文本 reader 接受嵌套操作作为输入简写，
解析后将其展开为独立指令，artifact 与 canonical 文本中的操作数只保留值或常量。
控制流结构只能出现在指令位置。

## 算子

| 类别 | 名称 |
| --- | --- |
| 整数算术 | `add sub mul sdiv udiv srem urem` |
| 位运算 | `and or xor shl lshr ashr` |
| 整数比较 | `eq ne slt sle sgt sge ult ule ugt uge` |
| 浮点算术 | `fadd fsub fmul fdiv` |
| 浮点有序比较 | `foeq fone folt fole fogt foge` |
| 浮点无序比较 | `fueq fune fult fule fugt fuge` |
| 转换 | `zext sext trunc bitcast fpext fptrunc fptosi fptoui sitofp uitofp` |
| 地址转换 | `int2ptr ptr2int` |
| 内存与取址 | `lea load store alloca data_addr proc_addr` |
| 调用 | `call call_indirect` |
| 控制流 | `if loop switch yield break continue return` |

比较结果为 `#bits<1>`。位模式自身不带有符号性，`sdiv`/`udiv`、`slt`/`ult` 明确选择解释方式。
没有 `div`、`lt`、`field` 等省略解释信息的形式。

向量算子和 `xchg`、`cmpxchg`、`rmw_*` 原子算子已有名称，执行实现仍有缺口。
车道后缀与 checked 多结果算子尚不能作为已实现指令使用。
引入或落实这些形式时，须同时确定类型规则、VM 与目标后端行为。

除零、移位量超出宽度、有符号除法溢出等域外行为应得到确定性拒绝。
IR 不提供异常捕获或 Trap 值构造；执行失败通过 VM/编译器协议报告。
各算子的实际实现与拒绝路径仍需专项验收，不能由名称表推断支持程度。

## 结构化控制流

区域可以产生结果。`yield` 交付区域结果，`return` 交付过程结果。
有结果的区域每条路径都需要满足结果要求；终结子后不得继续放指令。

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

循环头分别声明携带状态和结果。continue 更新携带状态，break 给出循环结果。
循环体自然结束表示离开循环，只有显式 continue 才产生回边。

```lainir
#proc sum(%n: #bits<64>) -> #bits<64> {
  %r = #loop count(%i: #bits<64> = 0, %s: #bits<64> = 0) -> (#bits<64>) {
    %done = #uge[#bits<64>](%i, %n)
    #if %done { #break count(%s) }
    %next = #add[#bits<64>](%i, 1)
    %total = #add[#bits<64>](%s, %i)
    #continue count(%next, %total)
  }
  #return %r
}
```

switch 选择子要显式给出物理类型，case 常量不能重复，必须有 default。
分支不贯穿到下一 case。

```lainir
#proc classify(%k: #bits<32>) -> #bits<32> {
  %r = #switch[#bits<32>] %k -> (#bits<32>) {
    case 0 { #yield 10 }
    case 1 { #yield 20 }
    default { #yield 0 }
  }
  #return %r
}
```

`break` 和 `continue` 的参数带括号，`yield` 和 `return` 不带括号。

## 调用与内存

直接调用按目标签名验证实参及结果。间接调用显式声明结果类型，
其参数检查能力有限；目标身份与 CALL 权限由执行环境检查。

```lainir
%r = #call add(%a, %b)
%v = #call_indirect[#bits<64>](%f, %x)
```

alloca 的计数是**与元素类型无关**的无符号常量，取值 `1..2^64-1`（验证器拒 0，码 **2024**），
分配相应元素数量的连续存储，结果为地址：字节数 = **计数 × 元素类型字节数**。
计数**不**按元素类型的宽度解释——`#alloca[#bits<8>](256)` 申请 256 字节、
`#alloca[#bits<64>](256)` 申请 2048 字节；元素类型只决定每个元素占多少字节。
`计数 × 元素字节数` 溢出拒 **1035**，超出租约容量拒 **1007**；被拒时水位不变。
load/store 显式指定访问类型，store 的操作数顺序为值、目的地址。
lea 计算 `base + index * scale + offset`，按地址宽度回绕。构造地址时不检查区域；
实际访问检查当前 VSpace 的范围和权限。尾后地址可以构造，访问时应拒绝。

地址有效性、区域权限与生命周期由执行环境管理。过程 activation 结束时，
对应栈活窗口收回；旧地址在该区域未重新授权时访问被拒。裸 `#addr` 不带对象身份，
同址重新授权后的对象级时间安全属于 Meta 的高层引用规则，见 [VM](vm.md#生命周期与预算要求)。
内存序和 volatile 写在类型实参列表中；在原子与后端语义验收完成前，不应把文本可解析当作同步保证。
