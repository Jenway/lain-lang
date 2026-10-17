# bootstrap 方言

`bootstrap/` 下是 28 个 Lain 源码模块，`lain0 --unit bootstrap/SOURCE_ORDER` 一次编成整份
Meta 的 LAINIR（`scripts/build_meta_unit.py` 产出 `build/lain0/bootstrap.l1`）。这份文档列出
这门方言的语法、每条语法的降级形状，以及当前仍缺失的形态。LAINIR 的格式见
`docs/01-lain-ir.md`，Meta 管线的现状见 `docs/selfhost-gap.md`。

## 判据

- **语言语义独立于 LAINIR 语义。** 源码里不出现 `#` 形态。否则 IR 改一个算子、
  改一条区域的 results 规则，就会变成破坏性的**语言**变更；LAINIR 只是 Meta 与宿主之间的
  私有 ABI。
- **验证器不面对用户输入。** `infer`（宽度唯一来源）与 `verify`（VM 与后端假定的契约）
  成立的代价是「源码只能经 handler 降级到达 IR」。用户能直接写 IR，语言契约就等于整个 IR。
- **低层操作必须可达。** 禁的是**透传**：`load<u64>(p)` 由语言规定类型、宽度、地址约束与
  失败码，**然后**由降级决定它变成 `#load[#bits<64>](%p)`；`#load[#bits<64>](%p, 0)`
  是语言什么都没说。资格线是「这条低层算子，语言有没有替它说话」。
- **不新增声明语法去描述 IR。** 语言的知识（有哪些标量、多宽、算符接到哪个 LAINIR 算子）
  住在 Meta 库的表里，不住在源码里。语法只用来写有体的东西（`func` / `struct` / `enum` /
  `let` / `import`）；没有体的声明只是换个地方抄库表。用户要自定义，走编译期调用库的注册
  接口，不走新语法。
- **唯一的信任门是** `lainir_parse` + `lainir_verify`。源码里没有 `#` 不构成安全边界 ——
  用户宏用同一套发射能力可以生成任意 LAINIR 文本（`bootstrap/std/macros/` 就在这么做）。

## 层 0：发射层

层 0 是一门**用 Lain 写的库**，每个 LAINIR 形态一个过程。它没有类型：宽度是**数字**
（单位是位数），值是**句柄**。

- 它面向正在生成的产物：`lainir_load(v, 8)` 往产物里追加一条 `#load[#bits<8>]`；
  `load(p)`（层 1）读当前地址空间里 `p` 处的字节。前缀用来区分两套语义，Meta 两者都要。
- **vref** 是发射出的值引用（产物里的 `%name`），是机器值。它是不透明标量：只由
  `lainir_*` 产生、只被 `lainir_*` 消费，不能算术、不能 `as`。
- 实现是 `bootstrap/std/lainir.lain`，处理管线发出的每一条指令文本都经过它。清空产物
  积存由宿主能力 `lain_meta_emit_reset` 承担（`bootstrap/meta.lain` 与
  `bootstrap/std/records.lain` 直接声明）。
- 任何 Lain 代码都能调用层 0（用户宏也能发射任意 IR）；闸门始终是
  `lainir_parse` + `lainir_verify`。

### 名单

层 0 不按算子逐个包装，而是提供行级写出过程，处理管线按指令形状组合它们。当前有
91 个过程，按用途分四类：

- **底层写出**：`lainir_write`（唯一直接追加文本的过程）、`lainir_byte`、`lainir_emit_uint`、
  `lainir_spaces`、`lainir_repr`、`lainir_buf_bytes`、`lainir_buf_byte`、`lainir_buf_uint`、
  `lainir_buf_temp`、`lainir_buf_repr`、`lainir_text_lea_field`。
- **指令与行前缀**：`lainir_op_prefix`、`lainir_op_prefix_phys`、`lainir_temp_name_emit`、
  `lainir_load_at`、`lainir_load_at_i`、`lainir_load_named`、`lainir_store`、`lainir_lea_at`、
  `lainir_lea_at_i`、`lainir_lea_named`、`lainir_alloca_at`、`lainir_binary_phys_at`、
  `lainir_binary_named_at`、`lainir_binary_op_at`、`lainir_binary_op_named`、
  `lainir_call_named`、`lainir_ret_call`、`lainir_return_ref`、`lainir_return_uint`、
  `lainir_ptr2int_at`、`lainir_int2ptr_at`。D-002 之后地址与整数统一走 `#bitcast`，
  `lainir_ptr2int_at` / `lainir_int2ptr_at` 内部发的就是 `#bitcast`。
- **单元与过程骨架**：`lainir_proc_head_full`、`lainir_proc_head_open`、`lainir_open_body`、
  `lainir_close_body`、`lainir_data_head`、`lainir_data_ro_head`、`lainir_data_rw_head`、
  `lainir_data_rw_bare_head`、`lainir_data_close`、`lainir_addr_body_write`、
  `lainir_base_assign_write`、`lainir_storage_suffix_write`、`lainir_return_base_close`、
  `lainir_zero_text`。
- **固定文本片段各自的写出器**：`lainir_indent_write`、`lainir_indent4_write`、
  `lainir_st_pct_write`、`lainir_pct_write`、`lainir_result_sep_write`、`lainir_newline_write`、
  `lainir_rparen_nl_write`、`lainir_rparen_nl_close_write`、`lainir_close_paren_write`、
  `lainir_open_paren_write`、`lainir_comma_write`、`lainir_colon_write`、`lainir_lea_head_write`、
  `lainir_call_word_write`、`lainir_proc_word_write`、`lainir_empty_params_write`、
  `lainir_zero_write`、`lainir_store_head_write`、`lainir_extern_marker_write`、
  `lainir_quote_nl_write`、`lainir_if_head_write`、`lainir_if_head2_write`、`lainir_else_write`、
  `lainir_close_line_write`、`lainir_close4_write`、`lainir_break_head_write`、
  `lainir_continue_head_write`、`lainir_result_head_write`、`lainir_loop_head_write`、
  `lainir_loop_body_open_write`、`lainir_arrow_lp_write`、`lainir_bracket_paren_write`、
  `lainir_body_return_write`、`lainir_ret_call_head_write`、`lainir_bind_head_write`、
  `lainir_vproj_load_write`、`lainir_vproj_if_write`、`lainir_vproj_lea_write`、
  `lainir_vproj_value_write`、`lainir_vproj_tail_write`、`lainir_assign_write`、
  `lainir_space_write`、`lainir_close_brace_write`、`lainir_return_write`、`lainir_yield_write`。

层 0 是「推迟的表面语法」的兜底：`switch`、`alloca`、`proc_addr`、间接调用没有表面语法，
仍可用 `lainir_*` 发射。推迟只说明用户代码不好写，不说明编译器做不到。

## 层 1：语言层

层 1 由 handler 实现，handler 用层 0 写：`let b: u8 = load(p)` 在 handler 里就是一次
`lainir_load(p, 8)`。

### 语法

```text
func add(a: i32, b: i32) -> i32 { return a + b; }
let main: i32 = add(3, 4);

let NAME: TYPE = <表达式>;
let NAME = <表达式>;      // 标注可省：类型由值推（调用取被调方返回类型）
return <值>;
if <条件> { … }            // 可带 else；条件是任意表达式（括号可省），分支必须以 return 收尾
let NAME: TYPE = if <条件> { <值> } else { <值> };   // 带值 if：省略 else 时假分支给类型零值
let total: u32 = for i in 0..n acc = 0 { acc = acc + i; };
let t: i32 = loop outer(i: i32 = n, acc: i32 = 0) while i > 0 { acc = acc + i; i = i - 1; };
break LABEL(<值>);        // 早退：把值交给循环，就是值型循环的出口
continue LABEL(<实参…>);  // 早退：进入下一轮，实参按参数顺序给
let b: u8 = load(p);      // 读内存：宽度由期望类型定（也可写 return load(p);）
store(p, v);              // 写内存：宽度取自 v 的类型
let q: addr = lea(p, i);  // 地址算术：降成 #lea(%p, %i, 1, 0)
let s: addr = "hi";      // 静态字节块（只允许顶层 let）：产物是 data s_storage ro { 104 105 } + #proc s() -> #addr
let t: [u64] = [10, 20]; // 定长同质表（只允许顶层 let）：产物是 data t_storage rw { … } + #proc t() -> #addr
extern NAME(a: T) -> U = link_name;   // 外部过程：只声明不定义，调用与 func 同路
```

- 赋值 `NAME = 值;` **只在循环体内合法**：能赋值的合法目标是循环自己声明的参数
  （`for` 只有语言定的 `acc` 一个）。
- `loop` 是 `for` 的通用形态：标签、任意个参数（各带类型与初值）、任意条件、多语句体。
  条件与 `if` 走同一个表达式入口，体里每格是 `参数 = 值;`，循环回边按声明顺序带走参数；
  参数里没有 `acc` 就没有值可取，报 4。
- 体里还可以写 `break LABEL(<值>);` 与 `continue LABEL(<实参…>);`，两者都结束这一轮
  之后的语句（区域里终止指令必须是最后一条）；体里也可以写 `if`，分支里的 `break` /
  `continue` 同样结束这一轮，分支里给参数赋的值与另一分支的值在 `if` 之后合并。`break`
  直接把值交给循环；`continue` 按参数顺序换掉下一轮的参数，循环的 `#break` 出口仍然照发
  —— 值型循环的区域里必须有一个 `#break`（验证器 2006），所以落尾的 `continue` 也走收尾
  那条 `#continue`。
- `load` / `store` / `lea` 是**内建**（不查作用域表）：`load(p)` 只出现在能提供期望类型的
  位置（`let b: u8 = …`、`return …`），宽度由那里定；`store(p, v)` 的宽度取 `v` 的类型；
  `lea(p, i)` 发 `#lea(%p, %i, 1, 0)`。没有期望类型（无标注 `let b = load(p);`、
  `load(p);` 单独成句）报 4，不猜。地址加整数是语言自己的一条规则：`p + i` 降成
  `#lea(基址, 下标, 1, 0)`（只认 `+`，且左边必须是地址；`i + p` 报 4）；`p + i * K` 的
  `* K` 折进 `#lea` 的 scale（K 是无符号十进制字面量，后面不能再跟算子）。
- 字节串字面量（`"…"`）**只允许出现在顶层** `let NAME: addr = "…";`：发的是一份静态
  字节块 `data NAME_storage ro { <字节…> }` 加一个取址过程 `#proc NAME() -> #addr`（同一个
  名字，`%base = #data_addr NAME_storage`）。字节按源码原样搬（reader 认转义对但不解码），
  **不补末尾 0**（要结束符就写 `let NAME: ro [u8] = [ … , 0 ];`）。别处（函数体内、作为
  实参）报 4：`data` 只能落顶层，发射是流式的。
- 定长同质表（`[T]`）**只允许出现在顶层** `let NAME: [T] = [1, 2, 3];`：元素宽度取 `T`
  （缺标注按 `u64`），元素必须是无符号整数字面量，装不下该宽度报 23；产物是
  `data NAME_storage rw { <每元素按小端拆字节…> }` 加同样的取址过程，`ro [T]` 出 `ro` 块。
  任意值位置都能写 `NAME[下标]`（发取址加 `#lea(基址, 下标, 元素字节数, 0)`；字面量下标
  越界报 4），`len(NAME)` 发元素个数（字节串绑定发字节数）。样例
  `bootstrap/lain/examples/table.lain`。
- `extern` 是**声明**：形如 `extern NAME(a: T) -> U = link_name;`，链接名原样搬进产物
  （`#proc NAME(%a: T) -> U #extern "link_name"`）。声明在作用域里的身份与 `func` 一样
  是 6，调用点、返回类型推导、参数表检查共用同一条路；它不登记任何宿主能力（登记是驱动
  调 `lainmeta_host_register` 的事）。样例 `bootstrap/lain/examples/extern.lain`。
- 顶层形式：`let` / `struct` / `enum` / `func` / `extern` / `import(…)`。关键字经 Meta
  语法注册表取 handler：1=my_if 2=func 3=if 4=struct 5=enum 7=顶层 let 8=return
  9=局部 let 10=for 11=my_block 13=loop 14=extern（12 是宏 `my_require`）。**6（原
  scalar 声明）已取消，号位退休不复用。**
- `for` 是一条**表达式**，值就是累加变量的最终值；`#loop` 的变量直接就是 `i` 与 `acc`，
  不需要「源码名 → LAINIR 名」的映射表。v0 限制：不能嵌套循环；下界与上界是值（字面量、
  参数或已声明的局部），不能写任意表达式；循环体只有一条 `acc = 值 算子 值;`。
- 注释是必须支持的：声明发现按词走，注释里的词不能被当成形式。

### 类型与宽度

- 期望类型**唯一决定宽度**：`let b: u8 = load(p)` 是 8 位，`store(p, x)` 的宽度取自 `x`
  的类型。
- 没有期望类型、值也推不出来时（裸表达式位置）**报错，不猜**；无标注绑定
  `let y = <值>` 由值推：调用取被调方声明的返回类型、参数名取参数表、括号往里一层，
  都不是才退回本函数的返回类型当**上下文**（两个裸字面量的算子靠它查表）。
- IR 刻意不存结果类型（`infer` 是宽度的唯一来源，单独一层），所以 `let` / `return` / `as` /
  实参位置必须把期望类型交给 `load` 的 handler。
- 标量集、repr（kind + 宽度）与算符绑定是 **Meta 库的表数据**：`bootstrap/std/scalars.lain`
  里的一段 `data` 字节表。表之外没有第二份出处：`i32` 这类名字由名字解析直接命中内建表，
  不进作用域表；带点的 `T.i32` 没有解析（标量是内建名字，模块不导出标量），按
  「没有这个名字」报 5。用户自定义标量走编译期调用库的注册接口。
- 聚合类型名（struct / enum）可以写进参数类型与带标注的 `let`：聚合值在 IR 里是 `#addr`，
  所以参数类型发 `#addr`，标注绑定发一次地址复制（`#lea(%p, 0, 1, 0)`）。声明体里字段与
  变体**空格分隔**，写逗号会让字段走查把逗号当成字段名，报 5。

## 对照例子

`for` 的降级形状：`bootstrap/std/funcs.lain` 的循环降级。

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

`loop` 的降级形状，取自样例 `bootstrap/lain/examples/loop.lain` 的产物：

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

`loop` 加地址算术的降级形状：

```lain
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

## 模块：成员位置与符号命名

模块键 = 被导入文件相对**源根**的路径去掉 `.lain`、把 `/` 换成 `_`
（`bootstrap/std/lex.lain` → `std_lex`，`bootstrap/lain/examples/modules_lib.lain` →
`modules_lib`）。源根在 `lain0 --unit <清单>` 时是清单文件所在目录，在
`lain0 <file.lain>` 时是入口文件所在目录。每个模块的顶层声明一律发 `<键>__<名字>`；
清单里标为**根名**的少数声明保持平坦（`bootstrap/SOURCE_ORDER` 只把 `meta` 的三个入口
`lain_std_abi_version` / `lain_std_initialize` / `lain_std_lower` 标成根名），宿主
`extern` 的 `link_name` 同样保持平坦。

| 位置 | 写法 | 发出的东西 |
| --- | --- | --- |
| 绑定 / 表达式 / 实参 | `M.answer`、`M.answer + 1`、`f(M.answer)` | `%r<n> = #call @<键>__answer()`（访问器调用） |
| 调用 | `M.bump(M.twice)` | `%r<n> = #call @<键>__bump(%r<m>)`；尾位置发 `#call[tail]` |
| 类型 | `func keep(p: M.Point) -> M.Point` | 类型表键 `<键>__Point`；聚合按地址传，参数发 `#addr` |
| 字段访问 | `func px(p: M.Point) -> i32 { return p.x; }` | `%r<n> = #lea(%p, 0, 0, <字段偏移>)` + `%r<n2> = #load[#bits<32>](%r<n>)` |
| 跨模块静态数据 | `M.table`、`M.table[i]`、`M.base + i`、`store(M.table[i], v)` | 访问器给基址，索引与地址算术按既有规则 |
| 本模块静态数据取址 | `symbol_addr("table_storage")` | `%r<n> = #bitcast[#addr](%<键>__table_storage)`；写别的模块的数据名报 301 |

成员名与形态的约束：

- `M.bump` 当值用（不调用）报 400「模块函数成员不能当值使用，请写成调用 M.bump(…)」。
- 成员名写错 `M.nope`、模块不导出标量 `M.i32`、`M.Shape.Circle` 报 301「未绑定的名字: M」。
- `symbol_addr("answer_storage")`（`answer` 属于别的模块）报 301「symbol_addr 只能取本模块
  的静态数据，跨模块请用 M.answer」。
- 导入路径跑出源根、模块键含 `__` 分别报 312「导入路径超出源根: …」与「模块键不能含连续
  下划线: …」。
- 同名声明靠模块键分开：`modules_twin` 的两个模块各有一个 `dup`，产物里同时有
  `modules_twin_a__dup` 与 `modules_twin_b__dup`。

## 聚合值

`struct` / `enum` 的声明发出布局（`<T>__<f>_offset`、`<T>__size`、变体的 tag 与载荷偏移），
顶层构造 `let p = Pair { a: 1 b: 2 };` 发出 `data p_storage rw { … }` 与
`#proc p() -> #addr`。

- **函数体内的构造降级到 `#alloca`**：`let q: T = T { … };` 与 `return T { … };` 发
  `%r<n> = #alloca[#bits<8>](<size>)` 加每字段一条 `#store`（基址
  `#lea(%r<n>, 0, 0, <off>)`），绑定接 `%NAME = #lea(%r<n>, 0, 1, 0)`。`#alloca` 从 stack
  租约取地址（元素大小 × 常量 count，起始地址按 16 字节边界取整，越界报
  `LAINVM_TRAP_STACK_EXHAUSTED`），帧弹出时位置退回 `stack_mark`（`seed/src/vm/engine.c`），
  所以区域退出后地址失效。
- **返回聚合值有固定规则**：结果类型是聚合时，返回的地址必须是**参数**（调用方给的）或
  **调用结果**（被调用方算出来的）；刚构造的地址与局部绑定一律报 11。样例
  `bootstrap/lain/examples/types.lain` 的 `idp(p: Point) -> Point { return p; }` 走允许的
  那条路。
- **字段访问当值**：`p.x` 在返回、绑定、更大的表达式（`p.x + p.y`、`1 + p.x`）与调用实参
  位置都降级成 `%r<n1> = #lea(%p, 0, 0, <字段偏移>)` 加
  `%r<n2> = #load[<字段类型>](%r<n1>)`。操作数的类型取字段自己声明的那个，所以 `1 + p.x`
  查的是 `i32` 的算子表。样例 `bootstrap/lain/examples/field_value.lain` 与
  `bootstrap/lain/examples/field_expr.lain`。
- **变体投影当值**：`Enum.Variant(值)` 降级成「读判别字节、与 tag 比、带值 `#if` 里按载荷
  偏移取载荷」，不命中给 0（v0 不做 panic 路径）。值可以是参数、局部值或表达式。空变体
  没有载荷可投，报 10。样例 `bootstrap/lain/examples/projection.lain`。
- **函数体内的变体构造**与积类型同形：`#alloca`、tag 一条 `#store`、有载荷时再来一条，
  绑定接 `%NAME = #lea(%r<n>, 0, 1, 0)`、返回直接 `#return %r<n>`。载荷按操作数降级，
  参数、字面量与调用结果都能用；空变体只发 tag 那条。样例
  `bootstrap/lain/examples/variant_local.lain`。
- **字段值可以是表达式**：`Point { x: a + 1 y: b }` 的 `#store` 值是那条 `#add` 的结果
  （字段之间夹逗号也认），顶层静态构造同样吃字面量表达式与调用。样例
  `bootstrap/lain/examples/field_expr.lain`。

实现注记：

- 同一过程里每个构造点必须用自己的 `%r<n>`：装载器 `region_lookup`
  （`seed/src/vm/image.c`）按名字取第一个匹配定义，重名会静默指向前一个。临时编号来自
  暂存区 `+144` 的计数器（`meta_next_temp`），顶层字面名 `%base` 与它们不冲突。
- 点号形态占三格（值、点号、成员名），成员后面跟 `(` 时实参组再占一格；
  `meta_after_operand` 对形状 6 跳 3 或 4 格。左边是和类型名且成员后面跟 `(` 才是投影，
  其余是积类型的字段访问，分支由 `meta_dotted_delim` 的返回值决定。
- 初始化列表是一个组节点：字段值到哪结束由下一个字段名决定（一个槽后面紧跟 `:` 就是
  字段名），这条终点判定只在表达式里出现名字加冒号时生效。
- 局部绑定上的字段访问仍报 4：局部变量表只记「名字 → 类型名窗口」，字段访问需要类型正文，
  只有函数参数能提供。

## 仍然缺失

- `switch`、`alloca`、`proc_addr`、间接调用没有表面语法，层 0 是它们的兜底。
- `i8` / `u8` 没有算子绑定（它们只作访存宽度）；`u8` 比较先写 `b as u32` 再比。
- `addr` 上的 `==` / `!=` 没有绑定。
- 没有 `true` / `false` 字面量，写成 `0` / `1`。
- 顶层构造里写变量名会发出解析不到的 `%NAME`（顶层绑定是过程 `#call NAME()`，不是值），
  由装载器拒绝。
- 类型不符的绑定不做检查：`func bad(p: Point) -> i32 { let k: i32 = p; return k; }` 会发出
  `%k = #add[#bits<32>](%p, 0)`，产物在 `lainir_verify` 处被拒（2005），报错位置在产物。
- 一次 lowering 请求能处理的源码规模存在上限（实测约 550–650 个声明展开节点，多个源会
  累积）；具体机制未定位。
- `region_grant` 已接线，只在 Meta 自己的代码里使用（`bootstrap/std/regions.lain`），
  表面语言还没有对应写法。

## 端到端验收

- `bash scripts/regress.sh` 与 `bash scripts/regress.sh --round-trip` 跑
  `bootstrap/lain/examples/` 下的 38 个样例，全部通过。
- 最小端到端样例 `bootstrap/lain/examples/arith.lain`：驱动报 `host_status=0`、
  `output bytes=74`，产物是 `%r1 = #add[#bits<32>](1, 2)`。
- Meta 管线（`build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt`）当前 38 个样例中
  26 个通过。12 个失败：`jumps`、`loop`、`loop_if`、`loop_local`、`loop_value` 以 trap
  status 1004 结束、产物被截断；`modules_lib`、`modules_main`、`modules_twin`、`refs`、
  `strinline`、`symbol_addr`、`u8math` 返回非零 `host_status`。缺口记录在
  `docs/selfhost-gap.md`。
