# LAINIR

本规范描述当前物理 IR 的形式。实现支持范围和待补验收见 [实现进度](../implementation/README.md)。
源语言中的类型、函数构造、模块、泛型和 effect 由 Meta 处理。

## 1. 值与类型

| 类型 | 含义 |
| --- | --- |
| `#bits<N>` | N 位整数位模式，有符号解释由算子决定 |
| `#f<N>` | 浮点表示，具体支持的格式由实现限制 |
| `#vec<N>` | 总宽度为 N 位的向量表示；向量执行与车道语法尚需完善 |
| `#addr` | 物理地址，不带 pointee 类型 |

没有复合类型、`#unit` 或 `#never`。无结果用空结果列表表示。
记录和数组的布局由 Meta 转换为字节数、偏移和访问指令。
当前位串与浮点验证限制为 `0 < N <= 64`；更宽的值不能仅靠文本声明获得支持。
位宽通过不表示该浮点格式或向量操作已可执行，需查对应后端的支持情况。
load/store 的位串和浮点宽度须为 8、16、32、64；地址另按地址表示处理。

值由定义它的操作确定类型。普通算术显式写类型实参；字面量按所在位置解释：

| 位置 | 类型来源 |
| --- | --- |
| 普通算子 | 算子的类型实参 |
| 过程 return | 过程结果签名 |
| 区域 yield、循环携带值和结果 | 对应区域声明 |
| 直接调用实参 | 被调过程参数签名 |

这是物理字面量规则。Lain 源码如何选择 i32/i64 属于 [Meta 规则](meta-v0.md)。

## 2. 模块、过程与外部声明

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
extern 的 link_name 指定宿主符号。解释器通过能力表解析，后端通过相应链接接口调用。
物理参数和结果由 IR 签名规定；寄存器、栈传参和目标调用约定由后端实现。

## 3. 绑定与操作数

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

## 4. 算子

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

## 5. 结构化控制流

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

## 6. 调用与内存

直接调用按目标签名验证实参及结果。间接调用显式声明结果类型，
其参数检查能力有限；目标身份与 CALL 权限由执行环境检查。

```lainir
%r = #call add(%a, %b)
%v = #call_indirect[#bits<64>](%f, %x)
```

alloca 的计数必须是正的常量，分配相应元素数量的连续存储，结果为地址。
load/store 显式指定访问类型，store 的操作数顺序为值、目的地址。
lea 计算 `base + index * scale + offset`。是否在构造阶段检查区域、怎样处理回绕，
见 [VM 待决策项](vm.md#尚待确认的决策)，本次整理不改变现有语义。

地址有效性、区域权限与生命周期由执行环境管理。返回后的地址失效检查尚未完整实现。
内存序和 volatile 写在类型实参列表中；在原子与后端语义验收完成前，不应把文本可解析当作同步保证。

## 7. 编译期调用

`#eval` 标记调用点的编译期求值阶段。实参必须在编译期已知。
折叠阶段执行调用并将可固化结果替换到产物中；后端输入不能残留待执行的 eval。

```text
#proc table_size() -> #bits<64> {
  #return 64
}

#proc size_plus(%n: #bits<64>) -> #bits<64> {
  %s = #eval table_size()
  %r = #add[#bits<64>](%n, %s)
  #return %r
}
```

上段是阶段语义示意，当前 parser 不接受其中的 `#eval` 写法（实测拒绝码 3002）。
seed 的数据模型以 `is_eval` 标记调用，fold 测试直接构造这个字段。
文本语法与打印往返尚需接通，不能把该示意作为当前可编译模块使用。

执行失败产生编译诊断。编译期地址不能直接固化成最终程序可用的宿主地址。
eval 折叠与运行 Meta procedure 是两种用途，不把 eval 称为编译器访问 VM 的唯一接口。
当前 Meta 驱动是否接入折叠，与 seed 是否存在 fold 实现分别验收。

## 8. 验证与规范文本

验证器检查定义与引用、类型、操作数和结果数量、控制流交值、终结子位置及 eval 实参。
验证成功只说明物理结构符合约束，不证明 Meta 已正确实现源语言语义。

canonical 是打印器输出的规范文本：操作平坦、类型实参明确、整数用十进制。
规范文本应满足 `print(parse(text)) == text`。该等式是文本固定点，不代表编译器已经自举。

完整模块示例应经过 parse/verify；执行示例还需断言结果。
