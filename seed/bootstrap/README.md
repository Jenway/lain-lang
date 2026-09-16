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
std/emit.l1    初代标准库：宿主 ABI 声明 + 产物输出
meta.l1        Meta 的三个入口与 v0 的语言规则
```

## v0 的语言

只有一条规则：

```lain
let NAME = INTEGER;
```

产出：

```lainir
#proc NAME() -> #bits<64> {
  #return INTEGER
}
```

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
