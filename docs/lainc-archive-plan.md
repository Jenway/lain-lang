# 编译 `src/compiler-archive` 计划（src/lainc 路线）

> 落盘于 2025 会话。目标：让 `src/lainc/lainc.lain`（stage1，Lain 写的
> 编译器，M2 自举固定点）能编译 `src/compiler-archive/` 闭包，并形成
> stage2 自举闭环。对应 `docs/lainc-bootstrap-roadmap.md` 阶段四/五。
> 状态：计划（未开工）。

## 1. 目标

```text
stage1 (src/lainc) ──编译──> src/compiler-archive 24 文件闭包 ──> stage2 lainc
stage2 再编译自身 ──> stage3；验收 stage2 == stage3 字节一致 + l1check 干净
```

- 每完成一个 archive 模块，产物过 `lainir-print`（l1check）并可
  `lainir-seed run`。
- 不一次把闭包当大目标：按依赖顺序逐个模块编译（roadmap 阶段四原则）。

## 2. 现状基线

### 2.1 src/lainc 已有能力（M2 固定点，87543 bytes）

- 单文件编译：`compile(source, output)` / 入口 `compiler_compile`（读单源）。
- `std::func` / `std::struct` / `std::module` / `std::consteval`。
- meta 表（6 段：`kind|ns|nl|ps|pl|mod_row|`，kind 1/2/3/4/6）、consts 小表、
  layout 表（record 字段 offset/width）、per-function types 表。
- 常量折叠、单层 consteval 求值（算术、多参绑定、无递归）。
- module 机制：命名空间隔离（`f0_<mod>_<member>`）、限定调用/常量引用
  （`meta_lookup_qualified`）、模块内 consteval 顶层折叠。
- 产物是 LAIN-IR 文本，经 seed 的 verifier/interpreter 验证运行。
- 自举约束：frozen 前端陷阱全集（复合表达式只 lower 首操作、`0-1` 需变量、
  `#add/#sub` 提升 64 位、循环内内层循环后的语句被丢、有返回值递归坏）。

### 2.2 src/compiler-archive 结构（4732 行 / 24 文件）

- 每文件模式：若干 `let X: Module = import("std::...")` 或
  `import("packages::lain::compiler::...")` + `@export let NAME: Module =
  std::module { @export let Factory = std::func(Memory: ...) -> Module { ...
  return std::module {...}; }; }`。
- 全部是**编译期元编程**：模块工厂、类型别名、泛型 Vec 实例化、record 定义、
  方法（`TokenVector.new()`、`memory.byte_at()`）、效果子句
  `! {Memory.Allocation.Effect}`、`&mut` 引用参数、`||`、`else if`、
  `Token {...}` record 构造。
- 内部依赖链（packages::lain::compiler::*）：tokenizer → syntax →
  generated_syntax/meta/source_workspace；types → effects/elaborator/lower/
  modules；... → compiler_core → compiler_driver → compiler_api → lainc。
- std 依赖 7 个：`std::memory_model`、`std::core::vec`、`std::core::memory`、
  `std::core::slice`、`std::core::source`、`std::effect`、`std::platform::api`。
  **仓库中无 std 源码**——frozen lainc.l1 将其作为「外部能力」
  （14666 行注释：std 源不在 workspace 时给 `vector.Vec(T)` 等一个类型值
  stub）。

### 2.3 可参照实现（不要照抄，语义对齐）

`src/lainir/lainc.l1`（frozen，LAIN-IR 链）已实现同语义：

- `compiler_compile_library` 多源入口（16803 行）。
- import 拓扑：`workspace_import_*`（2551+ 行）、`program_bind_import_aliases`
  （15985 行）、`program_materialize_import_target`（6432 行）。
- std 外部能力 stub（14666 行语义）。
- 模块工厂/闭包/import 绑定（12539+ 行 prebind、6363 行闭包）。

src/lainc 是 Lain 写、受自举约束，需重新实现，不能复用 LAIN-IR 代码。

## 3. 差距清单（对照 archive 实际代码）

| # | 差距 | 现状 | archive 需求（例） |
|---|------|------|--------------------|
| 1 | 多文件会话 | compile() 单 source | compiler_compile_library 多源 + import 拓扑 |
| 2 | import 解析 | 无（import 语句被静默跳过） | `import("packages::lain::compiler::tokenizer")` 绑定模块 |
| 3 | std 外部能力 | 无 | 7 个 std 模块 stub + `vector.Vec(T,A,B)` 类型值 |
| 4 | 模块工厂求值 | consteval 仅单层算术 | `tokenizer.Tokenizer(Memory)` 执行函数体返回模块值 |
| 5 | 类型别名 | 无 | `let TokenKind: type = i32;` |
| 6 | 泛型类型应用 | 无 | `vector.Vec(Token, Memory.Allocation, Memory.Bounds)` |
| 7 | `-> Module` 返回 | emit_type 未知类型输出 addr | 模块值类型 |
| 8 | 效果子句 | 无 | `! {Memory.Allocation.Effect}`（可先降级忽略） |
| 9 | `&mut` 引用参数 | 无 | `output: &mut TokenVector`（可先降级为按值/addr） |
| 10 | 方法调用语法 | 无 | `TokenVector.new()`、`memory.byte_at(s, i)` |
| 11 | `\|\|` / `else if` / 链式比较 | 部分（`&&` 只 lower 首操作） | `byte == 32 \|\| byte == 9 ...` |
| 12 | `@export` 可见性 | 无（`@` 被当 @foreign link） | 跨模块导出语义 |
| 13 | 产物链接 | 单产物 | 多模块 unit 合并 |

## 4. 分阶段计划

> 每个阶段结束：M1（→42）与 M2（gen2==gen3 固定点）回归必须保持绿；
> 新增 fixture 进 `tests/lainir_lain/` 并注册 `run_all.py`。

### 阶段 A：多源会话与 import 解析（差距 1、2、12）——✅ 已完成

- `compiler_compile_library()` 入口：读全部 host source，共享
  meta/consts/layout 会话；最后一个源是入口（无前缀），其余是 import
  库（顶层名 emit 为 `f0_<basename>_<name>`，basename 取自 host 路径）。
- `let X: Module = import("...")` 解析：取 import 字符串最后 `::` 段，
  与每个 host source 的路径 basename 字节匹配，绑定 kind=2 别名行
  （payload = 目标源索引）。
- `emit_label`/`emit_function2`/`emit_foreign_wrapper` 增加 `prefix` 缓冲
  参数（空 = 无前缀）；`compiler_compile_library` 进入入口特例。
- `@foreign` 增加 `bootstrap.source-path-data/length`。
- 验收：`run_lainc_archive_a.py`（裸函数库 `b.five` → `f0_b_five` → 5；
  模块库 `b2.math.seven` → `f0_math_seven` → 7），M1/M2 保持。
- 已知限制（本阶段记录）：
  - import 别名必须等于目标源 basename（archive 结构性约定，如
    `compiler_api.compiler_api.API`）；别名 ≠ basename 时成员解析错。
  - 源必须按依赖序传入（无拓扑排序）。
  - 裸名成员绑定跨库可能冲突（调用方走别名 kind=2 前缀构造，不查成员
    绑定，故无碍）。
  - gen1（frozen lainc 直接产物）的入口名是 `f0_compiler_compile_library`
    （frozen emit_label 无此特例）；gen2 起（自举后）入口名正确。

### 阶段 B：std 外部能力 stub（差距 3、6）——✅ 已完成

- `import("std::...")` 识别：import 字符串以 `std::` 开头时绑定 kind=2
  stub 行（payload_length=0 标记外部能力，无 host source）。
- stub 成员值调用在 `emit_operand2` 中降级为字面量 0（能力占位）；
  非模块 receiver 的方法调用（`output.push(...)`、`TokenVector.new()`）
  同样降级为 0。
- `-> Module` 工厂函数：`emit_function2` 检测返回类型 Module 后只绑定
  meta（kind=3），**不 emit 运行时 proc**（工厂体是编译期构造，其内
  类型别名/嵌套函数不再泄漏进产物）。
- 效果子句 `! {...}`：`emit_function2` 的 body_open 扫描跳过（不解析
  `Memory.Allocation.Effect` 等路径）。
- `||` 逻辑或：新增 `scan_or` + `emit_or_expr`
  （`#ne(#add(#zext64(#ne(L,0)), #zext64(#ne(R,0))), 0)`）；`&&` 右操作
  递归（`a && b && c` 全 lower）。
- 验收：`run_lainc_archive_b.py`（std import stub 运行 42；tokenizer.lain
  编译产物 l1check 干净、无工厂 proc），M1/M2/阶段 A 保持。
- 已知限制（本阶段记录）：
  - std stub 成员调用产物是字面量 0，**语义不完整**（仅 l1check；运行
    语义需要真正的 std 实现，属后续阶段）。
  - 类型别名 `let X: type = ...`、泛型 `vector.Vec(T, ...)` 尚未绑定
    （emit_type 回落 addr）。
  - 函数体外的 record 定义（`std::struct` 在普通函数体内）未支持。
  - 测试脚本写入的 entry 文件必须 LF（Python `Path.write_text` 在
    Windows 会写 CRLF，破坏 compile 文本扫描）。

### 阶段 C：模块工厂求值（差距 4、5、7）——最大工程

- `F = std::func(...) -> Module` 的编译期调用：绑定参数 → 逐语句执行函数体
  （类型别名绑定、record 定义、泛型实例化、嵌套 `std::module` 构造、
  `return <module>`）→ 产生模块 meta 值。
- 本质是「迷你模块解释器」；在 frozen 前端约束下实现（拆小函数、避免
  有返回值递归；参照现有 consteval 求值的非递归单层策略扩展为语句序列）。
- 模块体内的 `let Lex: Module = tokenizer.Tokenizer(Memory);`（syntax.lain
  19 行）调用链必须工作。
- 验收：tokenizer 工厂可被 syntax 调用返回模块值；合成 fixture 全链。

### 阶段 D：语言特性补齐（差距 8、9、10、11）

- 效果子句 `! {...}`：解析并忽略（记录到 meta，emit 时丢弃）。
- `&mut T` 参数：降级为 addr/按引用 emit。
- 方法调用 `x.f(args)`：按「receiver 限定调用」解析（现有 emit_path_tail
  已支持 `a.b(...)` 形态，扩展 meta 驱动解析）。
- `||`、`else if`、链式比较：拆句/循环展开（frozen 约束）。
- 验收：tokenizer.lain 全函数编译 + 运行；`is_space`/`tokenize` 逻辑正确。

### 阶段 E：archive 主干闭包（roadmap 阶段四）

- 按依赖顺序逐模块编译：tokenizer → syntax → generated_syntax → types →
  effects → ... → compiler_core → compiler_driver → compiler_api → lainc。
- 每模块产物 l1check + `lainir-seed run`；最后实例化 `lainc.API`（嵌入方
  提供 MemoryModel）运行。
- 验收：24 文件全部编译，产物 verifier-valid，无 `(error ...)` 产物。

### 阶段 F：自举闭环（roadmap 阶段五）

- stage1（src/lainc）编译 archive 得 stage2；stage2 编译自身得 stage3；
  stage2 == stage3 字节一致 + 双 l1check。
- 验收：闭环固定点；`src/lainc` 的 M2 测试迁移/并行于 archive 版本。

## 5. 风险与对策

- **阶段 C 是最大工程**：模块解释器在 frozen 前端下实现函数体执行。对策：
  把解释器拆成无递归小函数；先支持「类型别名 + record + 嵌套 module +
  return module」最小子集，再扩展；每步用合成 fixture 验证。
- **语法面宽**：archive 是完整语言。对策：先「解析并降级」（效果/引用/
  方法），不追求完整语义，产物以 verifier-valid 为先。
- **std 无源码**：不写 std 库，按 frozen lainc.l1 的外部能力语义给 stub。
- **M2 固定点脆弱**：任何 emit 改动都可能破坏自举。对策：每阶段改动后
  立即跑 M1/M2；lint（check_lens.py）+ LF 检查。
- **规模**：参照 frozen lainc.l1 相关机制约 2000+ 行 LAIN-IR；src/lainc
  复刻预计数百至上千行 Lain，分阶段合入，不一次大爆炸。

## 6. 验收命令

```text
python tests/lainir_lain/run_lainc_m1.py        # →42
python tests/lainir_lain/run_lainc_m2.py        # gen2==gen3 固定点
python tests/lainir_lain/run_lainc_module.py    # module 机制
python tests/lainir_lain/run_lainc_archive_a.py # 阶段 A：多源+import
python tests/lainir_lain/run_compiler_source_closure.py  # frozen 参照（不改）
```

## 7. 下一项

阶段 B 已完成并提交。下一项是**阶段 C：模块工厂求值**（最大工程）——

- `tokenizer.Tokenizer(Memory)` 编译期调用：绑定参数 → 逐语句执行工厂体
  （类型别名绑定、record 定义、泛型实例化、嵌套 `std::module` 构造、
  `return <module>`）→ 产生模块 meta 值。
- 本质是「迷你模块解释器」；在 frozen 前端约束下实现（拆小函数、避免
  有返回值递归；在现有非递归 consteval 求值基础上扩展为语句序列）。
- 模块体内的 `let Lex: Module = tokenizer.Tokenizer(Memory);`（syntax.lain
  19 行）调用链必须工作。
- 验收：tokenizer 工厂可被 syntax 调用返回模块值；合成 fixture 全链；
  保持 M1/M2/阶段 A/B 绿。
