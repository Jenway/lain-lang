> 历史记录。原路径：`docs/01-lain-ir.md`。归档日期：2026-09-21。
> 本文保留整理前的内容；其中的状态、命令、语法和结论不作为现行依据。
> 当前文档从 [文档索引](../../../README.md) 阅读。

# LAIN-IR：物理执行层

状态：当前 bootstrap 实现的语言规范。

LAIN-IR 是 Lain 编译流程中的物理执行语言。它描述已经确定尺寸、调用约定和内存访问方式的程序。语法分析、名称解析、类型构造、泛型、effect 和宏都由 Lain/Meta 层完成，不作为 LAIN-IR 指令存在。

当前 C seed 由同一套 C 数据结构支撑：`lainir-print` 负责解析、验证和规范化输出；
`lainir-seed run` 是 LAINVM 的参考执行实现；自举编译器把同一份 IR 降低为 C。

## 1. 物理类型

```lain-ir
#bits<N>
#float<N>
#addr
#unit
#never
```

`#bits<N>` 是宽度为 `N` 的位模式，本身没有有符号性。有符号或无符号解释由具体操作决定。`#addr` 是没有 pointee 类型的物理地址。

LAIN-IR 不接受 `i32`、`i64`、`addr`、`f32` 等别名。文本中必须使用上面的规范类型。内存模型中预留的 SIMD 类型尚无文本语法，因此不属于当前语言。

## 2. 模块项

一个模块由只读数据对象、过程定义和外部过程声明组成。名称在三者之间不能重复。

```lain-ir
#data greeting(6, 1, "hello");

#extern #proc write(#addr %bytes, #bits<64> %length) -> #unit;

#proc main() -> #bits<32> {
  #call write(#data_addr(greeting), 5)
  #return 0
}
```

`#data name(size, alignment, "initializer")` 声明静态只读物理字节：

- `size` 必须大于零；
- `alignment` 必须是 2 的幂；
- initializer 不能长于 `size`，不足的部分补零；
- `#data_addr(name)` 返回对象首字节的 `#addr`；
- `#lea` 和 typed `#load` 可以读取其中的数据；
- 写入该地址在参考解释器中会 trap；生成的 C 使用 `static const` 对象；
- 当前没有 relocation，也没有可写全局数据段。

`#data` 只表达物理字节和对齐，不携带源语言常量、字符串、对象或模块语义。

## 3. 过程和局部值

```lain-ir
#proc add(#bits<32> %left, #bits<32> %right) -> #bits<32> {
  #let %sum: #bits<32> = #add(%left, %right)
  #return %sum
}
```

`#let` 建立不可变局部绑定。结构化循环暂时使用 `%index: #bits<64> = #add(%index, 1)` 更新已有绑定。

外部过程由解释器 capability table 或本机链接环境提供。它只是物理调用边界。

## 4. 整数和浮点操作

位模式算术包括 `#add`、`#sub`、`#mul`、`#eq` 和 `#ne`。除法和大小比较必须明确选择解释方式：

```lain-ir
#sdiv(left, right)  #udiv(left, right)
#slt(left, right)   #ult(left, right)
#sle(left, right)   #ule(left, right)
#sgt(left, right)   #ugt(left, right)
#sge(left, right)   #uge(left, right)
```

`#div`、`#lt`、`#le`、`#gt`、`#ge` 不属于语言。比较结果为 `#bits<1>`。

整数宽度变化也必须写明：

```lain-ir
#zext[#bits<64>](%byte)
#sext[#bits<64>](%signed_byte)
#trunc[#bits<8>](%word)
```

当前浮点操作为 `#fadd`、`#fsub`、`#fmul`、`#fdiv`、`#feq` 和 `#flt`，适用于 `#float<32>` 与 `#float<64>`。浮点常量文本和完整 ABI 支持仍待补齐。

## 5. 调用

```lain-ir
#call add(40, 2)
#let %target: #addr = #proc_addr(add)
#let %sum: #bits<32> =
  #call_indirect[(#bits<32>, #bits<32>) -> #bits<32>](%target, 40, 2)
```

间接调用在调用点携带完整物理签名。验证器检查目标是 `#addr`、参数匹配签名；直接使用 `#proc_addr(name)` 时还会与被引用过程的签名核对。

## 6. 内存和地址计算

```lain-ir
#let %memory: #addr = #alloca(64)
#let %element: #addr = #lea(base=%memory, idx=%index, scale=16, offset=8)
#store[#bits<64>] %value, %element
#let %loaded: #bits<64> = #load[#bits<64>](%element)
```

`#alloca` 分配的存储属于当前 procedure activation，也就是这一次过程调用。从进入过程到该次调用返回构成一个 activation；返回时释放其中全部 `#alloca`。地址不得从这个 activation 逃逸。参考解释器会拒绝直接返回本次 activation 的 `#alloca` 地址。

`#lea(base, idx, scale, offset)` 只计算 `base + idx * scale + offset`，结果恒为 `#addr`。结构字段访问由上层根据已确定的布局展开为 `#lea`，再接 typed `#load` 或 `#store`。LAIN-IR 没有 `#field`。

load/store 的物理类型必须显式给出。截断、扩展和 bitcast 也必须写成明确操作。LAIN-IR 没有可承载任意宿主行为的 `#primitive`；已有能力使用各自的明确指令或外部过程。

## 7. 结构化控制流

```lain-ir
#if #eq(%value, 0) {
  #return 1
} else {
  #return 2
}

#loop scan {
  #if #uge(%index, %length) { #break scan }
  %index: #bits<64> = #add(%index, 1)
  #continue scan
}
```

LAIN-IR 暂不公开 CFG basic-block label。后端可在降低结构化控制流时建立自己的基本块。循环体自然落下会结束循环；需要下一轮时必须执行 `#continue`。

## 8. 编译期执行

`#eval { ... }` 是一个有静态物理类型的编译期执行表达式。Meta 先把编译期计算降低为
物理 IR；LAINIR 验证该 IR；LAINVM 执行已经验证的块。正常结束时，表达式产生其已验证的
普通 LAINIR 值；失败时产生 Trap，Trap 不属于表达式值。

`#eval` 在最终 backend 前必须执行并从产物中消失。它不会向 runtime IR 残留一个需要
backend 特判的节点。TCB、VSpace、Trap 和预算是 LAINVM 的执行状态，不是 LAINIR 类型、
值或指令。

`#eval` 只规定已验证物理计算的编译期求值和产物结果。编译期地址的创建、持有、
有效性检查归 LAINVM 执行环境；Meta 对 AST 与语义对象的引用归相应对象协议。
当前 bootstrap 用 `#addr` 承载对象 handle，但这不是 Meta 的语言语义要求；
对象引用的交付由 VM 执行接口和 Meta 对象协议负责，须经过 LAINIR 可表述的
VM 边界，不属于 `#eval` 的结果改写协议。
`#eval` 块可以在执行期间使用 `#addr`，但不得把仅在编译期环境有效的地址当作
产物中的结果。允许保留到产物的具体结果类型及其表示仍须单独固化。

`#eval` 的静态类型由它所在的表达式位置确定。验证器把块视为一个返回该预期物理类型的
匿名过程：所有可达 `#return` 必须返回该类型，缺少返回值或返回不同物理类型都是验证错误。
它不再以外围 `#proc` 的返回类型猜测自己的类型。

块可读取外围过程中的自由 `%local`。lowering 按词法绑定收集这些值，作为临时根过程的
参数按值传入；块不持有调用者的 frame。捕获 `#addr` 只复制地址值，不复制其指向存储；
该地址仍受原来区域和 activation 的生命周期约束。执行 `#eval` 的 TCB 不自带
VSpace，也不隐式继承调用者地址空间；使用捕获地址还需显式授权相应的内存能力。

解释器可限制 step count、call depth、累计 `#alloca` 字节数和模块中的 eval block 数量。零表示不设该项限制。

## 9. 规范化与错误

`lainir-print` 执行 `parse → verify → canonical LAIN-IR`。规范输出只使用物理类型和明确区分符号语义的操作。解析器和验证器拒绝旧类型、模糊整数操作、`#field` 和 `#primitive`。

当前仍需继续明确的物理问题包括完整浮点常量与 ABI、所有间接地址的静态 provenance 验证、data relocation，以及带精确源码位置的全部运行时 trap。
