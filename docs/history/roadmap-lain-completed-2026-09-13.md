# Lain 已实现工作归档：2026-09-13

本文件归档当前路线图中移出的实现记录、验收证据及当时的测量限定。
当前唯一执行计划是 [`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md)。
以下原记录按原章节编号保留，章节引用指向整理前版本；历史措辞和数字不定义当前架构。
其中涉及尚未覆盖的行为、未执行的正式编译器、浮点/ABI 等限制，仍由当前路线图跟踪。
本次归档不代表重新运行所有历史 gate，不把构建确定性当成编译器固定点。

## 归档范围与本次核验

- 编码 0：类型宇宙绑定与裸 type 迁移。
- 编码 1：旧物理辅助代码、capture、builder 整理及识别谓词迁移；只归档实际成果，不宣称新目标完成。
- 编码 2：bootstrap 签名解析/存储/校验及正式表示字段；正式语义行为仍未完成。
- 编码 3：bootstrap 输入行解析、诊断、依赖位宽和现有约束来源覆盖；库策略完整正例仍未完成。
- 编码 4：旧泛型设施删除与源码迁移。
- 前端与后端修复、差分 gate 交付及历史测量。

本次现场运行 `check_input_effects.py` 退出 0，覆盖依赖形参 i8/i32、返回类型以及 i32/i64
位宽对照；整理前 §9.4 的“依赖形参类型至今不支持”已陈旧，不再进入当前计划。
`check_function_shape_conformance.py` 退出 0：9 个形状用例一致，4 个正式侧未覆盖用例明确 SKIP。
源码已有 `lain_std_function_header_status` / `lain_std_function_tail_status`，因此整理前
“函数形状校验仍未搬到库”的描述不再成立；4 个 SKIP 与完整语义归属仍在当前计划。

本次用当前工作区源码重建 backend 后，`check_backend_differential.py` 退出 1：前六例
一致，负数除法仍为 seed `3` / Lain 后端 `2147483647`。旧的 #sdiv“已修、七例一致”
记录无法成立，该修复不归档为完成，留在当前路线图 §13.4。

## 原实现记录

### 3.3 编码 0：已完成（2026-09-12）

§6.2 名称冲突解除和 §6.3 类型宇宙绑定都已完成，并已通过 §6.5 的全部验收：

| 验收项 | 结果 |
| --- | --- |
| `python scripts/check_std_type.py` | 退出码 0，`PASS std::type binding and bare type rejection` |
| `python scripts/check_bootstrap_consteval.py` | 退出码 0 |
| `python scripts/build_formal_stdlib.py` | 退出码 0 |
| `python scripts/build_srclainc.py` | 退出码 0 |
| 搜索 `import("std::type")` | 无结果 |
| 搜索裸 `type` 标注（`std/`、`src/`） | 无结果 |

反例 `scripts/fixtures/formal_bare_type_rejected.lain` 有意保留裸 `type`，由
`scripts/check_std_type.py` 验证它必须被拒绝，因此它不计入"无结果"。

工作树中尚未提交、属于本阶段的文件：

```text
bootstrap/compiler/meta_bindings.l1
bootstrap/compiler/meta_type.l1
bootstrap/compiler/lower_program.l1
scripts/check_std_type.py
scripts/fixtures/formal_std_type_value.lain
scripts/fixtures/formal_bare_type_rejected.lain
```

这些改动已经过验收，可以按 §6.5 的切片提交；不要覆盖它们。

### 3.4 已修复的陈旧断言

编码 0 的 `std::type` 迁移改了源码拼写，但漏改了按字符串匹配源码的 gate。以下三处已修正，
改动前它们使对应检查恒失败：

- `scripts/check_lainvm_boundary.py`：`"let Flow: type"` → `"let Flow: std::type"`；
- `scripts/check_lainc_lainir_api.py`：`"let SourceResult: type"` → `"let SourceResult: std::type"`；
- `scripts/check_stdlib_swap.py`：三处源码清单中的 `std/type.lain` → `std/type_policy.lain`。

教训：这类 gate 用字面量匹配源码，**改名时必须同步搜索 gate**。新增此类断言前，先确认它
检查的是行为还是拼写。

### 3.5 已修复的静默误编译（2026-09-12）

前端有两个**静默误编译**：产出错误的 LAINIR，但**退出码 0、无任何诊断**。两者根因不同：

| 缺陷 | 现象 |
| --- | --- |
| `while` 的 `&&` 条件只保留**首合取项** | `while i < a && i < b` 只按 `i < a` 退出——算错或死循环。`if` 不受影响，因为条件走的是另一套 writer |
| 值表达式 writer **只支持单个算子** | `v * 10 + d` 编译成 `#mul(%v, 10)`，丢掉 `+ d`。加括号正确，所以一直没暴露；且不限赋值——`return` 与调用实参同样受影响 |

**验证方式是运行而非读码**：一个同时含 `f(3,4)` 与 `w(10,3)` 的程序返回 **37**；对修复前的
编译器同一程序返回 **40**（f 会得 30、w 会得 10）。新增 `scripts/check_operator_precedence.py`
断言该运行，并能把失败定位到单个用例（把它还原为修复前版本即报 `expected 37, got 40`）。

**一条值得记的实测**：编译器自己的源码闭包里就有一处。`srclainc` 产物只差 **1 行**——
从「测试 `meta_ast_kind(%body) == 2`」变为「同时测试它与 `meta_ast_delimiter(%body) == 123`」。
也就是说**一个分组定界符检查此前被编译掉了**。过程数不变（362），其余逐字节相同。

**教训**：这类缺陷不会让任何 gate 变红，只会让程序给出错误答案或让 verifier 在远离病因处报错。
因此凡是「表达式形状」相关的能力，验证必须落在**运行结果**上，文本断言只能定位不能证明。


## 6. 编码 0：建立 `std::type`（已完成 2026-09-12）

本节保留实施记录与验收命令，供追溯。编码 0 的所有条款均已落地并通过 §6.5 验收；后续阶段
以 §6.5 的命令作为回归基线。阶段完成后可按既有惯例把完成证据移入 `docs/history/`。

```text
python scripts/check_bootstrap_consteval.py
python scripts/check_lainvm_boundary.py
python scripts/check_srclainc_artifact.py
```

前两个检查应通过。第三个检查可能耗时较长；必须等待退出状态，不能把长时间无输出当作
成功。记录失败命令和第一个错误，不要顺手修复无关 backend 问题。

### 6.2 释放 `std::type` 名称

修改范围：

```text
std/type.lain
std/meta.lain
scripts/build_formal_stdlib.py
scripts/check_policy_conformance.py
scripts/fixtures/formal_meta_policy_probe.l1
bootstrap/std/policy.l1
```

步骤：

1. 把仍被 `std/meta.lain` 使用的严格转换规则移到 `std/type_policy.lain`，导出 Module 名
   `type_policy`。
2. 把 import 改为 `import("std::type_policy")`。
3. 审计旧 `std/type.lain` 的其他导出；没有当前调用者的策略直接删除，不复制到新文件。
4. 删除旧 `std/type.lain`，确保 `import("std::type")` 不再表示 Module。
5. 调整 policy probe，只保留仍属于当前设计的转换、effect 和 bounds 检查。generic 检查
   在编码 4 最终删除；在此之前不得给它增加调用者。

### 6.3 在标准根环境绑定类型宇宙

bootstrap 的首要修改位置：

```text
bootstrap/compiler/meta_values.l1
bootstrap/compiler/meta_bindings.l1
bootstrap/compiler/meta_type.l1
bootstrap/compiler/meta_collect.l1
bootstrap/compiler/stdlib_contracts.l1
```

实现要求：

1. 在 Meta 值系统中为类型宇宙建立稳定身份。可以继续使用 Meta 内部的 type-value kind；
   该 kind 不能进入 LAINVM API。
2. 在每个编译单元使用的标准根环境中创建 `std` Module，并把 `type` 绑定到该类型宇宙值。
   如果当前没有集中创建标准环境的函数，在 `meta_bindings.l1` 中新增一个，再由编译入口
   调用；禁止在路径查找函数里硬编码字符串返回值。
3. 修改类型标注判断：解析标注表达式，通过 `meta_env_lookup_path` 得到值，再以 Meta 值
   身份判断它是否为 `std::type`。不得继续用
   `meta_atom_equal(annotation, "type")`。
4. `program_is_type_binding` 若继续保留，必须接收环境并完成上述解析；同步更新
   `meta_collect.l1`、`lower_program.l1` 和 `stdlib_contracts.l1` 的所有调用点。
5. `i32`、`addr`、用户 record 等类型值的静态 Meta 类型都应是同一个 `std::type` 值。

### 6.4 原子迁移裸 `type`

当 bootstrap 能解析 `std::type` 后，将以下当前源码中的类型值标注改为 `std::type`：

```text
std/**/*.lain
src/lainc/**/*.lain
src/lainir/**/*.lain
src/lainvm/**/*.lain
scripts/fixtures/*.lain
```

不要修改 `docs/history/`。不要机械替换普通英文注释或 Python 的 `type[...]`。

新增 fixture：

```text
scripts/fixtures/formal_std_type_value.lain
scripts/fixtures/formal_bare_type_rejected.lain
scripts/check_std_type.py
```

正例至少覆盖：

```lain
let Alias: std::type = i32;
let main = std::func() -> Alias { return 42; };
```

返回 `std::type` 的 callable 留到编码 1 测试；编码 0 只证明类型宇宙绑定和类型标注解析。
反例 `let Alias: type = i32;` 必须得到稳定的“未绑定名称”诊断，不能被静默接受。

### 6.5 编码 0 验收

运行：

```text
python scripts/check_std_type.py
python scripts/check_bootstrap_consteval.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
```

附加搜索：

```text
rg -n -F 'import("std::type")' std src bootstrap scripts
rg -n --pcre2 ':\s+type\b|\bcomptime\s+[^:]+:\s+type\b' std src
```

第一个搜索必须无结果。第二个搜索必须无结果。

**这两个搜索的模式本身有坑，不要按早期版本的写法改回去：**

- 必须用 `\s+`（一个以上空白），不能用 `\s*`。`\s*` 允许零个空白，因此会把 `std::type`
  里的 `:type` 也匹配上——而 `std::type` 正是本阶段要引入的拼写。用 `\s*` 写这个搜索，
  它永远不可能为空，无法作为验收条件。
- 搜索范围必须排除 `scripts/fixtures`：反例 fixture
  `scripts/fixtures/formal_bare_type_rejected.lain` 有意保留裸 `type`，由
  `scripts/check_std_type.py` 验证它必须被拒绝。把它扫进"必须无结果"里是自相矛盾的。

搜索只证明旧拼写消失，不能替代四条执行命令。

建议提交切片：

```text
stdlib: release the std type name
bootstrap: bind the std type universe
source: migrate type annotations to std type
```

禁止做法：把 `std::type` 作为 Parser 关键字；保留裸 `type` 别名；让 `std::type` 同时表示
Module 和类型宇宙；为了通过构建而改写生成的 `build/**/*.l1`。


### 7.0.1 已完成的基础工作及限定

保留 2026-09-12 的验收证据，不把修订目标标成已经完成：

| 旧子阶段 | 已有成果 | 在新计划中的限定 |
| --- | --- | --- |
| 1a | parse/find/arguments/execute/release 辅助流程收敛为 `lainvm_meta_run_artifact`，按物理返回类型读取结果 | 可复用物理辅助代码；尚未证明全部编译期计算经过显式 `#eval` |
| 1b-1/1b-2 | 内存 artifact capture、动态缓冲及写入统一 | 作为 IR 生成基础复用；不等于语言语义已迁移 |
| 1b-3 | module/effect 等描述值构造复用共同 builder | 仍有语言专用分派，完整函数体 lowering 尚未完成 |
| 1c | 24 处构造器名字比较移到库谓词；`check_meta_form_swap.py` 证明识别依赖库 | 仅完成部分边界迁移；谓词之后的语义构造、检查和 lowering 仍须移回库 |

旧 1a 验收记录：`check_bootstrap_consteval.py`、`check_lainir_boundaries.py`、
`build_lain_compiler.py`、`check_lainc_lainir_api_baseline.py` 退出 0。
内存 capture 的增长、无截断及与文件 sink 并存由 `check_bootstrap_vm_api.py` 覆盖。
这些是既有证据，后续每片仍须针对新增行为执行验收。

撤回旧计划“编译器保留模块/结构体/effect 种类及构造原语，库只保留拼写”的结论。
名字表迁移不能替代语义迁移；只新增 `meta_is_effect` 等谓词不能完成本阶段。


## 8. 编码 2：让 `std::func` 解释完整签名

### 8.1 Parser 约束


先在 `scripts/check_meta_ast_conformance.py` 增加 fixture，证明下面的源码形状可以由现有
Atom/Group 表达，两个花括号都是普通 Group：

```lain
let f = std::func(value: T) ?{T: std::type} -> T !{IO} {
    return value;
};
```

**实测的 RawAst（2026-09-12，`lain_raw_ast_dump`）**：

```text
(root (atom let) (atom f) (atom =) (atom std) (atom :) (atom :) (atom func)
  (group ( (atom value) (atom :) (atom T))
  (atom ?) (group { (atom T) (atom :) (atom std) (atom :) (atom :) (atom type))
  (atom -) (atom >)
  (atom T) (atom !) (group { (atom IO))
  (group { (atom return) (atom value) (atom ;))
  (atom ;))
```

结论：

- `?` 与 `!` **已经是单个 Atom**，lexer 无需修改；
- 两处 `{...}` 都是普通 Group（delimiter 123）；
- **`->` 不是单个 Atom，而是相邻的 `(atom -)` 与 `(atom >)`** —— 本节早先假定它是一个
  Atom，**该假定错误**。Lain 的 tokenizer 不分关键字，因此没有 `->` token。
  签名读取器必须把「紧跟 `>` 的 `-`」识别为箭头，而不是期待单一 Atom。
- 参数组是普通 `(group ( ... )`，与 `{...}` 组以 delimiter 区分。

**因此本节的第一条验收（"`->` 是 Atom"）不可能成立，须改为上表的等价断言。**

除非该形状无法由现有 Atom/Group 表达，否则不得修改 Parser 数据模型。**不要**增加
Function、InputRow 或 EffectRow AST node；也**不要**为 `->` 添加 lexer 特例——相邻
`-` `>` 已经是可用的表示。

### 8.2 callable 签名表示

bootstrap 表示与 `src/lainc` 正式表示都必须包含：

```text
declaration source and span
explicit parameter list
input row
return type
output effect row
body group
declaration environment
```

正式实现的主要修改点：

```text
src/lainc/meta.lain
src/lainc/elaborator.lain
src/lainc/effects.lain
src/lainc/types.lain
```

`src/lainc/effects.lain::FunctionType` 当前只有 `return_type` 和 `effects`；加入 input row 后，
输入和输出必须使用不同字段，不能把输入合并进 `effects`。

### 8.3 解析顺序和诊断

`std::func` Meta elaborator 按以下唯一顺序读取相邻 RawAst：

```text
parameter Group
optional ? + Group
required -> + return type expression
optional ! + Group
required body Group
```

必须拒绝：重复 `?{}`、重复 `!{}`、`!{}` 出现在返回类型之前、缺少 `->`、缺少 body、
输入条目缺名、输入条目在冒号后缺类型。每个错误保留引发错误的 RawAst source span。

空 `?{}`、空 `!{}` 与省略相应行语义相同。

### 8.4 验收

新增 `scripts/check_function_signature.py`，至少包含完整签名、两个空行、省略行及上述所有反例。
然后运行：

```text
python scripts/check_meta_ast_conformance.py
python scripts/check_function_signature.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
```

提交：

```text
meta: elaborate complete function signatures
```

### 8.5 完成状态与限定（2026-09-12）

§8.4 的四条命令全部退出 0。实际落地的范围：

| 项 | bootstrap | 形式（`src/lainc`） |
| --- | --- | --- |
| 解析 `?{}` / `!{}` 并按固定顺序读取 | ✅ | ❌ 不读签名（连箭头与返回类型都不读） |
| 描述符存储（输入行 / effect 行 / 箭头位置） | ✅ offset 128/136/144 | ⚠️ 仅 `FunctionType.input_row` 字段 |
| 条目形状校验与 source span | ✅ 12 用例 + span 负对照 | ❌ |

**必须明说的验证限定**：`build/lainir/srclainc.l1` **从未被执行**——只有
`check_srclainc_artifact.py` 构建两次比较确定性。因此形式侧唯一的验证是
「`build_srclainc.py` 退出 0」。加了 `input_row` 字段后产物从 185791 变为 185770 字节
（过程数仍 358），证明它是真实结构而非被消除的空写；但**它的行为无从验证**，因为形式
elaborator 还不读签名，`std/meta.lain` 的 `meta_elaborate_status` 仍是边界桩。

形式侧的行为验证要等到 elaborate 真的读签名——那是编码 3 的工作。


### 9.0.1 实现状态（2026-09-12）：本阶段各项已落地

调用点解析已落地：invocation environment 构造时会遍历被调函数的输入行，在**调用者**环境里
按名字查找、按声明类型检查、并绑定进 invocation environment。查找用既有的
`meta_env_lookup_path`，因此精确名、由内向外、最近绑定遮蔽的性质不变。

**可观测证据**：正例 fixture 把调用者本地的值经输入行传入，改调用者的 `41` 为 `7` 后程序
输出随之变为 `7`——值确实来自调用者环境，不是默认值。

三类失败都有诊断，都用既有码，且都指向引发错误的节点：

| 失败 | 码 | span |
| --- | --- | --- |
| 缺少输入（名字查不到） | 5108 | 条目的名称节点 |
| 类型不匹配 | 5108 | 条目的声明类型节点 |
| 无法推导（无类型裸名） | 5104 | 条目的裸名节点 |

#### 逐项状态（1–4 项均已修；第 5 项是已知的模型限制）

1. ~~值输入的类型匹配只做到 kind 级~~ **已修（2026-09-12）**：Meta 标量值现在在 payload 的
   offset 8（既有空槽，未扩结构、未加 kind）携带宽度；声明处从注解经既有宽度表取，Meta 调用
   结果取 callee 的返回宽度。比较**只在两侧宽度都已知时**进行——任一侧未知则保持旧的仅比 kind
   行为，避免让「无标注」或「宽度表不认得的注解」开始报错（有独立 fixture 锁定）。
   实测对照：旧编译器接受 `let x: i32` 对上 `?{x: i64}`（rc 0），新编译器报 **5108**（span 指向
   声明的 `i64` 节点）。
2. **§9.2 的约束来源已全部实现（4/4，2026-09-12）**：显式参数类型标注、返回类型标注、
   **同行其它条目的类型表达式**、**body 的 `let` 类型标注**。裸名 `?{T}` 若在任一位置出现，
   其 kind 推定为类型值；四处都不出现仍报 5104（「没有约束」这一支必须保留）。
   只认已确证的类型位置：body 只认 `let <名> : <类型>` 的标注，嵌套 `std::func` 形参表、return
   表达式、record 字段标注**不计入**（未能确证其必然是类型位置，计入会削弱 5104 反例）。
   §9.4 原文正例仍缺 `Ord`——那是**库符号**，全仓库不存在。
3. ~~裸名一律报 5104~~ **已修（2026-09-12）**：有类型位置约束时推导，无约束时才 5104。
   同时形参类型可经调用者环境解析：`std::func(a: T) ?{T: i32}` 现在可用，宽度由调用者提供的
   类型值决定（已用 i32→i64 的对照实测：形参从 `#bits<32>` 变为 `#bits<64>`）。
4. ~~scalar Meta 调用路径不读输入行~~ **已修（2026-09-12）**：scalar 路径现在解析输入行。
   实现要点：artifact 在显式形参之后按行序追加输入条目；调用点**复用**既有的
   `lainvm_meta_resolve_inputs`（未复制解析逻辑），从同一 invocation environment 取回值并按序
   追加实参。span 与码与 module/effect 路径一致。
   **一处刻意的偏离**：票据原要求所有条目不区分地作为 `#addr` 形参，实测不可行——
   `#proc meta_entry(#addr %local) -> #bits<32> { #return %local }` 被 verifier 以
   `2014 return type mismatch` 拒绝，且 `#addr` 携带的是 Meta 句柄而非数值。因此**标量类型**的
   条目按其声明宽度作 bits 形参（与既有显式标量形参同法），只有非标量条目才用 `#addr`。
5. **只报第一条失败条目**（与编译器 first-error-abort 模型一致）。

#### 顺带查实的两个既有缺陷（2026-09-12，均已修）

1. ~~`meta_builtin_size` 的四个 `#bits<N>` 分支不可达~~ **已修（2026-09-12）**：
   它们把整个标注与字面量 `"#bits<8>"` 这类整串比较，而**源码里的 `#bits<32>` 根本不是单个
   Atom**——实测 `lain_raw_ast_dump` 得到 `(atom #bits) (atom <) (atom 32) (atom >)` **四个**
   Atom，所以按整串比较永不命中，长度参数取值多少都无救。**我最初把它记成「长度参数写错
   （2/3 而非 8/9）」是不准确的——词法拆分才是根本原因。**
   现在宽度解析识别这四 Atom 序列并读十进制宽度，死分支已删除。**单位保持分离**：
   `meta_builtin_size` 仍返回字节数（`#bits<32>` → 4，与 `i32` 一致），位宽由同一 helper ×8
   得到，因此 record 布局与对齐无需其它改动。`#bits<32>` 的 RawAst 形状**未改**（前后 dump
   逐字节相同）。
   **行为对照**：修复前 `#bits<32>` 形参报 5104；修复后产物为 `(#bits<32> %x)`，且位宽真的驱动
   物理类型——`#bits<8>` 产 `(#bits<8> %x)` 并把实参 300 截断为 44，`#bits<32>` 保持 300。
   作为入口返回类型时 `#bits<32>` 通过、`#bits<64>` 被拒。
   回归由新增的 `scripts/check_bits_type.py` 守住。
2. ~~program 模式恒失败~~ **已修（2026-09-12）**：`program_validate` 原先把 `main` 的返回类型
   节点与字面量 `"#bits<32>"`（长度 3）比较，任何源码拼写都无法满足，故恒 **5112**。
   现改为比较**解析后的宽度**（`program_type_width_in_unit` == 32），契约因此变成语义的：
   `i32`/`u32`/`#bits<32>` 都能编译并真实运行，`i64`/`i8`/`#bits<64>` 仍报 5112。
   回归由 `scripts/check_program_entry.py` 守住——该检查有鉴别力：把编译器还原为修复前版本，
   它在第一个正例即失败。

**gate 总数因此从 18 增至 19**（新增 `check_bits_type.py`）。


## 10. 编码 4：删除旧泛型设施并迁移源码

### 10.1 必须删除的设施

删除并同步所有调用者、ABI 列表和 fixture：

```text
std/generic.lain
std/meta.lain::meta_generic_parameter_valid
std/meta.lain::meta_generic_specialization_token
bootstrap/std/policy.l1 中对应过程
scripts/fixtures/formal_meta_policy_probe.l1 中对应 probe
scripts/build_formal_stdlib.py 中对应 ABI_SUPPORT_ENTRIES
src/lainc/elaborator.lain 中 SpecializationArgument、SpecializationKey、Specialization
src/lainc/diagnostics.lain 中只服务旧 specialization 的诊断
```

`ComptimeValue` 当前也被 `src/lainc/effects.lain` 用作 effect 参数。不能仅按名字删除：先把
仍然有效的普通 Meta 值用途迁到统一 MetaValue 表示，再删除只表达旧泛型参数分类的字段和
constructor。

不要在此阶段新增 replacement generic registry。类型函数直接执行；性能缓存留到固定点
之后根据 profile 决定。

### 10.2 迁移目标

重点检查：

```text
std/core/vec.lain
std/core/result.lain
std/core/slice.lain
std/core/string.lain
std/core/memory.lain
std/memory_model.lain
src/lainir/api_contract.lain
src/lainvm/api_contract.lain
src/lainc/*.lain
```

这些文件中的类型参数都是普通 `std::type` 参数；需要环境供应的依赖放入 `?{}`；由调用者
明确传递的策略继续作为显式参数。不要仅为了减少实参就擅自把 Allocation、Bounds 或
Memory 移入输入行。

`src/lainvm/interpreter.lain` 原在此列，但该文件已暂停并归档
（`docs/history/formal-implementations/`，见 §2）。重写它时同样适用本节要求，但那是
归档恢复后的工作，不阻塞本阶段。

### 10.3 验收

运行：

```text
python scripts/check_policy_conformance.py
python scripts/check_stdlib_conformance.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
python scripts/check_srclainc_artifact.py
python scripts/check_input_effects.py
```

再搜索 `generic_policy`、`meta_generic_`、`SpecializationKey`、旧 `comptime` 语法。当前源码
和当前文档必须无结果；Python 自身的 `type[...]` 与历史文档不在此检查范围。

提交可以按标准库模块拆分，最后收口：

```text
stdlib: finish ordinary meta type factories
```

**完成状态（2026-09-12）**：§10.1 的清单已全部删除并同步调用者，§10.3 的六条命令全部退出 0，
文本搜索无结果。

实测到的两处值得记下：

1. **`ComptimeValue` 不能按名删除**，这与 §10.1 的预判一致：effect 参数向量仍在用它。
   被删掉的是**旧的泛型参数分类**（按 kind 分成四路 payload）与它的四个 constructor；保留的
   是单一 tagged 编译期值（`kind`/`type_id`/`payload`/`valid`），它不含「哪些 Meta 值可作为
   泛型参数」的策略，也不含按返回类型分派的协议。effect store 改用 `types.MetaValue` +
   `same_meta_value`，排序仍为 kind 再 payload。
2. **删除是真实的代码缩减**：`srclainc.l1` 的过程数从 **358 降到 353**。因此本阶段**不能**用
   「产物逐字节不变」作为验证判据——那是前面几个阶段的判据。本阶段的判据是两侧标准库一致性
   （`check_policy_conformance`）、`check_stdlib_conformance`、两个构建、确定性 gate
   （`check_srclainc_artifact`）、以及 19 道 baseline 全绿。

**残留的合法提及**（不是遗漏）：`std/effect.lain:3` 的注释 "Generic Meta facilities"、
`std/meta.lain` 的 `callable_phase_comptime`（phase 常量，与旧 `comptime` 模型无关）、以及
`std/core/vec.lain` 里 "specialization" 的普通用法（按类型特化存储）。


### 13.0 backend 行内 else 缺陷：已修（2026-09-12）

`src/lainc/backend_c.lain` 的 `emit_line` 曾把 `} else {` 当**行首前缀**匹配后直接返回，
**丢弃该行剩余内容**。于是 `} else { %r = 2 }` 只发射 `} else {`，丢掉 `%r = 2` 与结尾的 `}`，
函数少一个闭合大括号、下一个函数嵌套进去，`zig cc` 报 `function definition is not allowed here`。

全量产物 `build/lainc-native.c` 因此有 **3 处**未闭合（都在 `program_std_type_member` 里形如
`} else { #return #call meta_value_nil() }` 的三行）。

**修复**：三个前缀分支（`} else {`、`else {`、单独 `}`）不再丢弃行尾，改为消费完整逻辑行；
行尾若是 `}`，先把其前的内容作为语句发射，再单独发射 `}`。递归起点严格前进，故有界。

**独立复验**（括号扫描前先用正则剥掉字符串/字符字面量与注释——不剥会误判，我踩过）：
`final_depth` 从 **+3 → 0**，出现在 `depth>0` 处的顶层函数定义从 **33 → 0**；
最小复现 `scripts/fixtures/backend_inline_else.l1` 的产物现在能被 `zig cc` 编译；
`check_native_backend_canonical_diff.py` 与 `check_native_backend_migration.py` 未回归。


#### 13.0.2 后端覆盖面审计（2026-09-12）：编译器闭包内无缺口，但闭包外有

把 `build/bootstrap/lainc.l1`（20661 行、362 过程的完整编译器）交给后端，统计产物中的
`/* unsupported L1: ... */` 标记：**真正的未支持构造为 0 条**
（早先看到的数十条标记全是**源码注释**被透传，不是构造）。输入 artifact 用到的构造已被全覆盖，
含 `#call` 7341、`#addr` 4344、`#if` 2867、`#let` 2530、`#return` 2306、`#eq` 916、`#lea` 906、
`#store` 478、`#add` 466、`#load` 414、`#ne` 386、`#continue` 368、`#loop` 312、`#break` 307、
`#sge` 180、`#sub`、`#zext`、`#mul`、`#slt`、`#sgt`、`#sle` 等。

**但这只说明「编译器闭包用到的构造」被覆盖，不等于「LAINIR v1 被覆盖」。** 后者我在接着
逐条测试时发现两处真实缺口（都不被编译器闭包触发，因此前面所有 gate 都看不见）：

| 缺口 | 实测 |
| --- | --- |
| **`emit_c_type` 的宽度/浮点映射不全** | 只处理 `#unit`、`#addr`、`addr`、`#bits<32>`、`#bits<1>`，**其余一律回落 `uint64_t`**。故 `#bits<8>` 得 `uint64_t`（应为 8 位）、`#float<32>`/`#float<64>` 也得 `uint64_t`（应为 `float`/`double`） |
| **未知表达式被原样透传** | `#fadd(%x, %y)` 直接写进 C（`return #fadd(x, y);`），**既不支持也不报 `unsupported L1`**，静默产出非法 C |

**关键差别**：seed 的发射器遇到不认识的类型**返回失败**（`seed_emit_c_type` 的兜底是
`#return 0`），而 Lain 后端**静默替换成 `uint64_t`**。「失败」与「静默给出错误答案」是两回事，
后者正是 §3.5 那条教训的同一形态。

**已修（2026-09-12，提交 `5c0a5ff`）**：

- `emit_c_type` 补齐 `#bits<8>`→`int8_t`、`#bits<16>`→`int16_t`、`#float<32>`→`float`、
  `#float<64>`→`double`，显式写出 `#bits<64>`→`uint64_t` 与 `#never`→`void`；
  兜底改为**标记并失败**（不发 artifact，seed 驱动报 status 1）。
  **`#bits<32>` 仍是 `uint32_t`、`#bits<1>` 仍是 `uint8_t`**：实测若把 `#bits<32>` 改成 seed 的
  `int32_t`，闭包产物会有 125 处差异，其中 14 处是 host ABI 的 `extern` 原型（真实 C 签名是
  `uint32_t`/`int`），改动会扩散到 ABI 边界，故保留。
- `emit_expr` 的未知 `#` 构造不再透传，改走既有的 `/* unsupported L1: ... */` 通道。
  **另外三个同源泄漏一并修掉**，均无任何 gate 覆盖：
  - `#sdiv` 曾被匹配但落在 else 链之外，仍落入兜底 → 除法产出 `L1_sdiv#a, b)`，
    **即除法在后端里本来就是坏的**；
  - `#call_indirect` 与 `#call` 共享前缀，尾部被泄漏；
  - `#alloca(<类型>)` 被透传成 `L1_alloca(#bits<8>)`。
- **float 运算有意未实现**：本后端把一切值（含地址）都当 `uintptr_t` 传参，浮点值没有跨调用边界的
  表示；正确实现需要位转换 helper 与调用约定改动，属新特性而非修复。边界由 fixture 钉住
  （断言出现 marker），不伪造 lowering。

**修正结论**：native 构建当前的障碍仍只在 `#eval` 与宿主 ABI（对编译器闭包而言成立），
但后端**并非「LAINIR v1 全覆盖」**——float 运算是未实现（现已显式标记而非静默产出非法 C），
整数宽度与类型映射已补齐。

#### 13.0.3 前端表达式形状审计（2026-09-12）：第二轮无缺口

上一节记录的两个静默误编译修好后，又对易错形状做了两轮穷举性检查，**均正确**：

| 形状 | 产物 |
| --- | --- |
| `1 + 2 + 3` | `#add(#add(1, 2), 3)` |
| `v * 2 * 3` | `#mul(#mul(%v, 2), 3)` |
| `v + d * 10` | `#add(%v, #mul(%d, 10))` |
| `v - d - 1` | `#sub(#sub(%v, %d), 1)` |
| `v * 2 + d - 1` | `#sub(#add(#mul(%v, 2), %d), 1)` |
| `(v + d) * 2` | `#mul(#add(%v, %d), 2)` |
| `v / 2 + d` | `#add(#sdiv(%v, 2), %d)` |
| `a - -b` | `#sub(%a, #sub(#trunc(0), %b))` |
| `(a + 1) * (a - 1)` | `#mul(#add(%a, 1), #sub(%a, 1))` |
| `a + a * a` | `#add(%a, #mul(%a, %a))` |
| `while i < a \|\| i < b` | 单条 `#eq(#ne(#add(#zext(#slt…), #zext(#slt…)), 0), 0)` |
| `while i < a && i < b` | 两条 `#break` |
| `if i < a && i < b` | `#eq(#add(#zext(#slt…), #zext(#slt…)), 2)` |

**另一个值得单独处理的问题**：backend 产出畸形 C 时**退出码为 0**，没有任何诊断，失败只在
`zig cc` 阶段以级联错误暴露。已由 `scripts/check_backend_c_shape.py` 补上（括号平衡 +
无嵌套函数定义），该 gate 有负对照。

本节各项与编码 5 的 A/B 选择无关，因此不受其阻塞。

#### 13.0.3b backend load/store 宽度误编译：已修（2026-09-12）

与 §13.0.2 的「静默替换」同类，但后果是**内存安全**：

| 源 | 修复前 | 应为 |
| --- | --- | --- |
| `#load[#bits<32>]` | `L1_load64`（**越界读 4 字节**） | `L1_load32` |
| `#load[#bits<16>]` | `L1_load64`（**越界读 6 字节**） | `L1_load16` |
| `#store[#bits<16>]` | `L1_store64`（**破坏相邻 6 字节**） | `L1_store16` |

根因：load 分支只区分 `#bits<8>`、其余**全部回落** `L1_load64`（`L1_load32` 在 prologue 定义了
却从未被选中）；store 分支覆盖 8/32，**16 落入 `L1_store64`**；prologue 没有 16 位宏。

**已修**：两条路径按宽度分派，prologue 补 `L1_load16`/`L1_store16`，**未知宽度改走 unsupported
标记而非默认 64 位**。

**运行验证**（比文本强，且我独立复跑过）：

- 64 位 store 在 `p+2` 埋 `0xFF` 哨兵 → 16 位 store 写 1 → 读回哨兵字节：
  **修复后返回 65（哨兵存活）**，修复前返回 **7（哨兵被 8 字节写清零）**。
- 全 1 缓冲做 16 位 load：**修复后得 65535**，修复前得 **4294967295**（多读了 6 字节）。

**顺带发现（与 §3.5 同一形态）**：同一份 `srclainc.l1` 经两个版本的后端，产物只差 **3 处**——
两个新宏，外加**两处 load 从 64 位纠正为 8 位与 32 位**（偏移 8 的 1 字节 tag、偏移 24 的 4 字节
字段，此前都在越界读）。也就是说**编译器自己的产物里就有被误编译的 load**。

#### 13.0.4 已交付：两个 C 后端的差分测试

本轮定位到的后端缺陷里，**最有诊断力的一步是拿两个后端对同一输入做对照**——
`seed` 的发射器与 Lain 后端都能把同一份 LAINIR 翻成 C，而它们的产物本应语义一致。实测例：

| 输入 | seed | Lain 后端 |
| --- | --- | --- |
| `#load[#bits<32>]` | `lainir_load_i32` | **`L1_load64`** ← 越界读 |
| `#load[#bits<16>]` | `lainir_load_i16` | **`L1_load64`** |
| `#store[#bits<16>]` | 16 位写 | **`L1_store64`** ← 破坏相邻字节 |

**文本比对不可行**（两者命名与 addr 表示不同：`lainir_*` + `uint8_t *` 对 `L1_*` + `uintptr_t`），
所以差分必须是**语义级**：两份 C 各自编译成可执行文件，对同一组 fixture 断言**相同输出或相同
退出码**。

这与 §11.0 那条「Lain VM 与 C seed 的行为差分」是同一手段，可用同一套 fixture 语料。
**已交付（2026-09-12，提交 `26d80ef`）**：`scripts/check_backend_differential.py` 实现了上述语义差分，
并已进入主入口（gate 总数 37）。两个 fixture：`differential_load_store.l1`（窄 load/store 宽度）、
`differential_arithmetic_control.l1`（优先级、`&&` 条件、除法）。

**它第一次运行就找到一个真缺陷**：Lain 后端的 load/store helper 是**解引用强转指针**，
而 seed 走 `memcpy`。`#alloca(n)` 给出的是字节数组，故偏移 2 上的 32 位访问**真的未对齐**——
seed 能处理，Lain 后端则 panic（`load of misaligned address ... requires 4 byte alignment`）。
helper 已改为 memcpy 形式并接收 `uintptr_t`。

**关于这个 gate 自身的一条教训**：用 `-O2` 构建时，**即使后端未修复它也通过**——
优化会去掉对齐检查，而 x86 容忍未对齐读，两者只在 Debug 构建下才显现差异。
因此该 gate **不启用优化**，原因写进了脚本注释。

##### 扩面后连环找到的缺陷（2026-09-12）

把 fixture 从 2 个扩到 4 个（地址转换、整数运算、无符号比较族）后，**又找到三个缺陷**，
全部落在两个后端之一，且全部**不影响退出码**：

1. **seed 的 `#ptr2int`/`#int2ptr` 括号不平衡**（`seed/lainir/compiler.l1:439`、`:448`）：
   两个字面量各多一个 `(`（开 4 闭 2），产物是 `return ((uint64_t)(uintptr_t)(p);`——
   少一个右括号，**不能编译**。闭包不用 `#int2ptr`，所以从未触发。**已修**（对齐邻近第 232 行
   的 `((uint8_t *)(uintptr_t)` 形状）。
2. **Lain 后端缺 `#ult`/`#ule`/`#ugt`/`#uge`**：只有 `#slt`/`#sle`/`#sgt`/`#sge` 的分支。
   闭包里这四种各出现 **0 次**，故未触发。
3. **未支持运算符的"标记"会变成合法 C**：兜底写的是 `/* unsupported L1: #ult */`——那是
   **注释**，于是操作数被留下，`ult(2, 1)` 实际成为 `(2, 1)`，即**合法 C 的逗号表达式，求值为 1**。
   实测差分：seed 得 −41、Lain 后端得 −38，差 3 正好来自 `#ult` 与 `#uge` 两处。

**第 3 条带回一条要修正的结论**：§13.0.2 记的「把未知表达式改为走标记通道」**对运算符类不够**。
标记在**语句**位置是有效的（表达式被丢掉，语句仍完整）；在**表达式**位置则留下一个合法的
逗号表达式，把「静默透传」换成了「静默算错」。运算符位置必须**让编译失败**，而不是标记。

**这三条的修复（2026-09-12）**：

- 第 1 条：seed 两个括号不平衡的字面量已改为与邻近第 232 行同形 —— `((uint8_t *)(uintptr_t)` /
  `((uint64_t)(uintptr_t)`，各少一个多余的 `(`。
- 第 2 条：Lain 后端补上四条无符号比较，宏为 `((uint64_t)(a) < (uint64_t)(b))` ——
  **两侧都升宽到无符号再比**，因为 `#bits<8>`/`#bits<16>` 在本后端是 `int8_t`/`int16_t`，
  直接比会按有符号比较。
- 第 3 条：未识别的**表达式**改为置 status 使整次编译失败、不发布产物；**语句**位置的标记保持
  不变（那里表达式被丢掉，语句仍完整）。`backend_float_expr.l1` 因此从「接受的用例」移到
  「拒绝的用例」—— 这是**纠正而非放水**：它原先断言的正是「静默算错的浮点表达式被正确产出」。

##### 差分测试扩面的下一个收获：seed 的无符号比较也是错的（2026-09-12）

补上无符号比较后，差分测试立刻暴露 **seed 侧同样错**：`seed/lainir/compiler.l1:375-382` 把
kind 33–36（`EXPR_ULT`/`ULE`/`UGT`/`UGE`）映射到与 kind 29–32（有符号）**完全相同的 C 运算符**。
即 `#ult` 与 `#slt` 编译成同一个比较。

**这个缺陷只有在操作数的高位为 1 时才可见**：`int8_t b = 255` 在 C 里是 `-1`，有符号 `-1 < 3`
为真，无符号 `255 < 3` 为假。我第一版 fixture 用的是 1/2/2 这类正整数，**两个后端都通过**——
**fixture 的取值选择本身决定了它能否发现问题**。

**已修**：两个操作数都显式转 `uint64_t` 再比，形如 `((uint64_t)(b) < (uint64_t)(3))`；
有符号 29–32 逐字节未动（实测 `#slt` 仍发射 `b < 3`）。

**一条关于差分测试的教训**：它的诊断力取决于 **fixture 的取值是否落在两个实现会分叉的区间**。
本轮三次"扩面即发现缺陷"（地址转换、无符号比较、窄操作数）都说明：**覆盖面比断言密度更重要**，
而"用一个刚好能区分的值"比"用一个能算对的值"更要紧。

##### 第六个缺陷：三种整数转换被完全丢弃（2026-09-12，已修）

继续扩面到 `#trunc`/`#sext`/`#zext` 后，差分测试报出：

```
differential_conversions.l1: seed returned '4508', Lain backend returned '70300'
```

**根因**：Lain 后端的转换分支（`src/lainc/backend_c.lain:355-369`）匹配 `#trunc`/`#sext`/`#zext`、
推进过类型注解，**但什么都不发射**。`#trunc[#bits<8>](300)` 编译成 `(300)`，而 seed 给出
`(int8_t)300` = 44。

**修法的要点不在"补上强转"，而在结果值的 C 类型**：截断必须产出 `uintN_t` 而非 `intN_t`。
`(int8_t)200` 是 −56，位模式虽然保住了，但**任何后续 `#zext`/`#sext` 都会按有符号读回**——
`#zext[#bits<32>](#trunc[#bits<8>](200))` 会得 4294967240 而非 200。
`uintN_t` 恰有 N 个值位，故 C 的无符号转换**就是**位截断；`#sext` 再自行按有符号解释。
宽度 1 是例外（`uint8_t` 多 7 个位），改用 `& 1u` 掩码。

符号扩展的**源宽度从注解文本解析**（覆盖 `#trunc`/`#sext`/`#zext`/`#load` 作操作数；
`#load` 必须包含，因为 `L1_loadN` 返回 `uintN_t`，不知道宽度就会零扩展）。
**宽度无法确定时拒绝而非猜测**：从 1 位源做符号扩展没有可用的 C 类型，该情形直接失败。

**逐条实测**（不只跑 fixture）：`trunc<8>` 对 300/200/70000 得 44/200/112；`trunc<16>(70000)` 得
4464；`zext(trunc<8>(200))` 得 200；`sext(trunc<8>(200))` 得 −56、`sext(trunc<8>(44))` 得 44；
`trunc<1>` 掩码生效（3→1、2→0）。

