# lain0 推进计划（stage 1：完成 Lain 语言覆盖）

> 本文件是**可执行的任务序列**。每完成一项就跑 `scripts/regress.sh --round-trip`，
> 过了就 commit + push，然后回来勾选。

## 北极星

**用 Lain 写出 Meta，让 lain0 把它编译成 LAINIR，跑在 LainVM 里。**

这是 stage 2。到那一步，Lain 才不再依赖手写 LAINIR（`bootstrap/`）自举。

## 为什么不能跳步

```
stage 0  bootstrap/     手写 LAINIR Meta          ✅ 已冻结（D-001）
stage 1  lain0          C++ 编译器：Lain → LAINIR  ← 当前，进行中
stage 2  Meta in Lain   Lain 写的 Meta，被 lain0 编译后在 VM 跑
```

stage 2 的**硬前提**是 lain0 覆盖 Lain 语言。Meta 要用表存符号、用聚合表示
AST 节点、用循环遍历、用字符串处理源码——lain0 少一个特性，Meta 就有一处
写不出来。这不是"补特性 vs 做 demo"的取舍，是依赖。

## stage 1 的完成定义

**`bootstrap/lain/examples/` 下 31 个样例全部编译通过。**
每个样例对应一组语言特性，31/31 即"语言覆盖完成"。

### 进度

**31 / 31** ✅ 全部通过（含 `--round-trip`）

剩的 11 项（T1–T5）全部收掉：表索引、聚合字段访问与构造、enum 变体构造与投影、
类型转换、指针 addr↔bits、模块加载（import 真正加载被导入模块）。

已通过：
```
arith bits bytes cast choose convert extern field_expr field_value flow hex
if_default jumps local_type locals loop loop_if loop_local loop_value memory
modules_lib modules_main neg ops pointer projection table types value_if
variant_local widths
```

已覆盖的能力：标量运算与多宽度、字面量（十进制/0x/负号）、类型转换、
过程（`func`/`extern`/调用/尾调用）、语句（`let`/`return`/赋值/`if-else`）、
指针内存（`load`/`store`/`lea`/`ptr_of`/`addr_of`）、**循环全套**（`loop` 与
`for` 值形态、`break`/`continue` 带实参、循环体内 `if` 早退与参数合并、
无 `acc` 靠 `break` 带出结果）、**值形态 `if`**、**字符串字面量**。

## 任务序列（剩余 11 项）

### T1 · 表收尾 —— `table`

差两块 Lower：

1. `bytes[1]` → `#lea(%base, %i, 元素字节数, 0)`（docs/04:208 口径）
   - base 要发 `#call @bytes()` 拿首址（表名是访问器过程）
   - **scale 必须取准**：样例里同时有 `[u8]` 和 `[u64]`，取错全错
2. `len(table)` → 元素个数

`lowerStaticLet` 里的表分支（字节小端展开）和字符串分支**已实现**，
Sema 的顶层绑定**已修**（`d86c530`）。

### T2 · 聚合：struct 构造与字段访问 —— `locals` `field_value` `field_expr` `types`

这 4 个是同一块，**最大头**：

```
struct Pair { a: i32 b: i32 }
let p: Pair = Pair { a: v b: 3 };   // 构造
return p.a;                          // 字段访问
```

- `types` 是**嵌套** struct（`struct Line { a: Point … }`），要处理布局嵌套
- 降级形状查 `docs/04-bootstrap-dialect.md` 的聚合段；`TypeStore` 已有
  `AggregateLayout`，`Ast.hpp` 已有 `ConstructExpr`
- 现状：AST/Parser/Sema 大概率齐了，**缺 Lower**（和前面几项一样的模式）

### T3 · 聚合：enum / variant —— `projection` `variant_local`

```
enum Shape { Circle(i32) Empty }
```

variant 的构造与判别、字段投影（projection）。

### T4 · 模块 —— `modules_lib` `modules_main`

```
let M = import("examples::modules_lib");
let a: u64 = M.answer;
```

跨文件绑定与 `M.x` 的名字解析。注意 `modules_main` 依赖 `modules_lib`
（同一目录下的另一个文件），两个要一起过。

### T5 · 收尾 —— `cast` `pointer`

```
p as u64      // #bitcast[#bits<64>]
w as addr     // #bitcast[#addr]   （D-002 口径）
ptr_of(p) / addr_of(w)
```

`cast_test` 手测过能出正确 IR，样例却不过——多半是别的小缺口，先查再改。

## 验收标准（两道独立闸门，缺一不可）

1. `verify` —— IR 结构合法
2. `round-trip` —— `print(parse(text)) == text`

```bash
bash scripts/regress.sh            # 只跑 verify
bash scripts/regress.sh --round-trip   # 两道都跑 ← 以这个为准
```

此前 round-trip 默认关闭，导致"验证通过"名不副实（`5d78613` 已修）。
现在带 `--round-trip` 的通过数应与不带时一致；**不一致就说明有验收漏洞**。

## 已知规则（踩过的坑，别再踩）

详细版见 skill `lain0-compiler` 的 `references/lainir-lowering-rules.md`。要点：

- **区域可见性**：分支内定义的值在外层不可见。合成的 `#break`/`#continue`
  必须引用循环**入口参数**，不能引用分支内的临时值（症状：`2002 undefined value`）
- **分支合并**要靠 `#if` 带结果 + `#yield`；已终结的分支（break/continue/return）
  不必 yield，验证器视其为该路径已结束
- **else 必须从「if 之前」的绑定状态开始降级**，否则会读到 then 分支的值
- **终止指令后不能再有指令**（`2003`）；`lowerStmt` 的 Jump 分支必须置 `terminated`
- `#loop` 必须有标签（for 没有标签语法，生成内部唯一名）
- **比较指令的 `ty` 是操作数宽度**，不是结果宽度（`#bits<1>`）
- Sema 的 `typeOf` 必须走完整个构造（含条件），否则表达式类型缺失、退回期望类型
- **别把编译器输出接管道再读 `$?`**——那是过滤器的退出码，不是编译器的
- 驱动在 verify **之后**才填 IR 文本，失败时看不到 IR；调试时临时在
  `lainir_verify` 前打印 `lainir_print_to_string(ir)`

## 提交纪律

- 一次一个可推送的增量，不要攒一天再推
- commit 前 `git status --short`：后台/并行 agent 曾改过文件，`git restore -- <file>`
- LF→CRLF 警告是噪音，忽略
- 遇网络导致 push 失败先重试（SSH 认证是好的，多是临时连接重置）

## 语言扩展（feat 后续的增量，已落地）

stage 1 的 31 样例全绿后，按用户要求补的语言特性（均过 `--round-trip` 回归）：

1. **u8 / i8 比较与算术**：放开 `selectBinary` 的 `width==8` 闸门（Sema.cpp）。
   样例 `u8math.lain`。
2. **字符串字面量内联形态**：表达式位置的 `"..."` 在模块级发 `data` 块 + 取址过程，
   调用它拿 `#addr`（复用 LowerDecl 的 `buildAccessor`/`emitDataAddr`，已从匿名命名空间
   提到 `ln::` 以便跨编译单元复用）。样例 `strinline.lain`。
3. **ref / mut ref / slice 类型体系**（用户「不要 ptr，要 ref/mut ref/slice/addr」）：
   - `ref T` / `mut ref T` → LAINIR 即 `#addr`，附 pointee 类型；`load` 读、store 写 mut ref。
   - `slice T` → `(addr base, u64 len)` 2 字段聚合。
   - 内建 `ref_of` / `mut_ref_of`（值 bitcast 成 #addr）、`slice_of(base, n)`（栈上 16 字节结构）。
   - 关键字 ref/mut/slice；TypeStore 加 Ref/Slice 两种 Entry::K。
   - 样例 `refs.lain`。
   - **未做（下一增量）**：slice 的 `.ptr`/`.len` 字段访问与 `s[i]` 索引（构造与返回已通）。

回归：31 -> 34（arith…refs，含 --round-trip）。
