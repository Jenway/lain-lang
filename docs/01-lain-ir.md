# LAINIR

> [!QUOTE]
> C 有许多角色，其中重要的一个是结构化汇编与事实上的统一 ABI。
>
> 大部分的 C 语言的 “现代竞争者” 认为 C 语言缺少了太多东西，于是增加了更多的 feature 并称之为 better C。
>
> Lain-IR 的观点是，在这种角色下，C 的特性恰恰是太多了以至于不能准确的对应到现代硬件上来。

## 值与类型

LAINIR 仅保留直接对应硬件物理表示的低层类型，不提供任何复合类型或高级抽象：

| 类型 | 含义 | 物理宽度 |
| :--- | :--- | :--- |
| `#bits<N>` | 定宽位串（整数与位模式） | 8, 16, 32, 64 |
| `#floats<N>` | 浮点数 | 32, 64 |
| `#vec<N>` | SIMD 向量表示 | 硬件支持的向量宽度 |
| `#addr` | 物理地址 | 机器字长（`sizeof(void*)`） |

- **定型机制**：值由定义它的操作显式确定类型。算术算子显式标注类型实参；字面量按所在上下文推断（算子类型实参、过程结果签名、区域 yield 类型或直接调用参数签名）。
- **位模式中立性**：`#bits<N>` 自身不带有符号性，符号语义完全由算子显式赋予。

## 模块、过程与数据对象

模块包含数据对象（Data Object）与过程（Procedure），符号按模块解析。

```lainir
data message ro { 104 105 0 }

#proc write(%bytes: #addr, %length: #bits<64>) #extern "write"

#proc add(%a: #bits<32>, %b: #bits<32>) -> #bits<32> {
  %sum = #add[#bits<32>](%a, %b)
  #return %sum
}
```

- **数据对象**：使用 `ro`（只读）或 `rw`（可写）声明，初值为显式字节序列。LAINIR 数据对象仅记录物理字节与访问属性；源语言的字符串、常量及结构体必须在 Meta 层降解为布局与字节后写入。
- **外部过程**：`#extern` 的 `link_name` 指定外部符号名。解释器通过 CSpace 能力表解析，编译后端通过目标链接接口调用。

## 绑定与操作数

- **单赋值形式（SSA）**：绑定采用 `%name = #op(...)`。同一区域内名字不可重复定义，变量使用前必须已定义。
- **存储与值分离**：值本身没有内存地址；需要地址时必须显式使用数据对象或 `#alloca`。
- **展开简写**：文本前端允许嵌套操作作为输入简写，解析后统一规范化展开为扁平的独立指令；控制流结构只能出现在独立指令位置。

## 算子系统

LAINIR 拒绝带有隐式解释的形式（如笼统的 `div`、`lt` 或字段访问），所有算子必须显式指明行为：

| 类别 | 算子名称 | 语义约束 |
| :--- | :--- | :--- |
| **整数算术** | `add sub mul sdiv udiv srem urem` | 明确区分有符号（`s`）与无符号（`u`）运算。 |
| **位运算** | `and or xor shl lshr ashr` | 逻辑移位（`lshr`）与算术移位（`ashr`）严格分离。 |
| **整数比较** | `eq ne slt sle sgt sge ult ule ugt uge` | 比较结果恒为 `#bits<1>`；严格区分有符号与无符号比较。 |
| **浮点算术** | `fadd fsub fmul fdiv` | 遵循 IEEE 754 浮点语义。 |
| **浮点比较** | 有序比较：`foeq fone folt fole fogt foge`<br>无序比较：`fueq fune fult fule fugt fuge` | 显式处理 NaN 参与比较时的行为。 |
| **类型转换** | 整数扩展/截断：`zext sext trunc bitcast`<br>浮点转换：`fpext fptrunc fptosi fptoui sitofp uitofp` | 符号扩展（`sext`）与零扩展（`zext`）显式分离。 |
| **地址转换** | `int2ptr ptr2int` | 仅在地址与位模式间执行数值转换，不授予权限。 |
| **内存操作** | `lea load store alloca data_addr proc_addr` | 显式指定读写类型；构造地址与解引用严格分离。 |
| **调用** | `call call_indirect` | 直接调用按签名校验；间接调用需执行环境核验 CALL 权限。 |
| **控制流** | `if loop switch yield break continue return` | 结构化控制流，区域可产出结果。 |

- **确定性拒绝**：除零、移位量超出位宽、有符号除法溢出等域外行为由 VM 协议确定性拒绝产生 Trap，IR 不提供静默的未定义行为（UB）或内置异常捕获。

## 结构化控制流

LAINIR 废除自由跳转指令（goto/br），所有控制流均以“区域产出结果”的结构化形式组织：
- `yield` 交付局部区域结果；
- `return` 交付过程终态结果；
- 终结指令后禁止放置后续指令。

### 条件分支（#if）
分支路径必须对称满足区域结果类型要求；以跳转（`#break`、`#continue`、`#return`）收尾的分支从区域外部离开，不计入这条要求：

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

### 循环（#loop）
循环头显式声明携带状态与其初值，以及最终交付的结果类型。循环体自然执行完毕表示离开循环，仅显式 `#continue` 产生回边：

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

### 多路分支（#switch）
选择子显式指定物理类型；case 常量不可重复；必须包含 `default` 分支；分支不发生贯穿（fallthrough）：

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

*语法规则：`break` 与 `continue` 的参数携带括号；`yield` 与 `return` 不带括号。*

## 内存与取址机制

### 栈分配（#alloca）
- `#alloca` 的计数为独立的无符号常量，取值范围为 `1..2^64-1`，**表示元素个数，而非字节数**；
- 元素类型仅决定每个元素的物理宽度，实际分配字节数恒为：
  $$\text{字节数} = \text{计数} \times \text{元素类型字节数}$$
  *例如：`#alloca[#bits<8>](256)` 分配 256 字节；`#alloca[#bits<64>](256)` 分配 2048 字节。*
- 尺寸算术溢出或超出当前执行栈容量时立即拒绝分配；
- 分配生命周期绑定当前过程激活（Procedure Activation），进入或退出分支/循环不释放栈空间。

### 访存与地址计算（load, store, lea）
- **load / store**：显式指定访问类型。`store` 的操作数顺序固定为 `值, 目标地址`。
- **lea**：执行基址偏移算术：
  $$\text{地址} = \text{base} + \text{index} \times \text{scale} + \text{offset}$$
  运算按机器地址宽度回绕。
- **地址检查分离**：构造地址（`lea`）阶段不检查物理区域；实际访存（`load`/`store`）阶段由底层 VSpace 强制核验范围与 READ/WRITE 权限。允许构造合法区域的尾后地址，但实际访问必须拒绝。
