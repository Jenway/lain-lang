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

### 阶段 C：模块工厂求值（差距 4、5、7）——✅ 已完成（核心路径）

- **多源拼接**：`compiler_compile_library` 把所有源拼接进一个共享大缓冲
  （`append_source_to_big`），`compile_source` 增加 `seg_begin`/`seg_end2`
  范围参数——meta 表名字偏移全局一致，**跨源 meta 查找不再错位**。
- **`let NAME: Module = MOD.FACTORY(ARGS);`**：`try_bind_module_value`
  识别（`scan_let_mark` 找 `: Module` 注解 + 调用形态 + 第一段 kind=2
  模块）→ `eval_module_factory`：
  - 工厂函数（kind=3）的 body_open 从 meta 取（同一大缓冲，偏移一致）
  - 建工厂模块值行（kind=2，payload=工厂名 span，payload_length>1 标记）
  - 扫描工厂体：`std::func` 成员经 `emit_function2` emit 为
    `f0_<factory>_<member>`（mrow=工厂模块行）；consteval/record/常量
    成员绑定；`return std::module {...}` 跳过
- **调用解析**：`emit_operand2` 中模块行 payload_length>1 时前缀用
  payload 的工厂名 span（`lex.five()` → `#call f0_make_five`）。
- 顶层 consteval 检测抽成 `try_consteval_call`（guard 风格），解决
  compile 顶层嵌套超限（frozen 5106）。
- 验收：`run_lainc_archive_c.py`（`t.make(1)` → `lex.five()` →
  `f0_make_five` → 运行 5），M1/M2/阶段 A/B 保持。
- 已知限制（本阶段记录）：
  - 工厂体 `return std::module { name: value }` 的导出列表不处理
    （体内所有绑定都算成员）。
  - 工厂参数不绑定（工厂体内的参数引用如 `Memory.Allocation` 未解析，
    效果子句跳过）。
  - `let X: type = ...` 类型别名、`vector.Vec(...)` 泛型实例化仍未绑定
    （emit_type 回落 addr）。

### 阶段 D：语言特性补齐（差距 8、9、10、11）——✅ 已完成（核心路径）

- 效果子句 `! {...}`：emit_function2 body_open 扫描跳过（阶段 B）。
- `&mut T` 参数：emit_type `&` → addr（阶段 B）。
- 方法调用 stub：非模块 receiver 调用降级 0（阶段 B）。
- **工厂体互调**：裸名调用若 callee 是工厂模块成员（kind=3、owner 行
  payload_length>1），前缀用工厂名 span（`bare_call_prefix` /
  `bare_call_prefix_len`）——`classify(byte)` → `#call f0_make_classify`
  等，工厂体内函数可互相调用。
- **字段访问失败降级**：`x.field` 在 per-function types 表查不到且 x
  不是模块时降级 0（原 fall-through 会输出未定义的 `%field`）。
- 验收：`run_lainc_archive_d.py`——工厂体内 record 定义、`||` 链、
  else、字段 stub、互调全链，`process()` → 42。M1/M2/阶段 A/B/C 保持。
- 已知限制（本阶段记录）：
  - `let X: type = <type-expr>` 类型别名与 `vector.Vec(...)` 泛型实例化
    仍未绑定（emit_type 回落 addr；record 字段宽度对别名取默认）。
  - 工厂参数不绑定（`Memory.Allocation` 等仅在效果子句中出现，已跳过）。

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
- **emit_operand2 不可动（阶段 E 实测）**：在 emit_operand2 内加任何
  逻辑（`&mut` 参数跳过、record 构造 stub，两种写法均试）都破坏 M2
  收敛（gen2≠gen3）。根因：emit_operand2 是「编译自身时被自身处理」的
  自举核心——frozen（gen1 生成者）与 gen1（gen2 生成者）对改动的翻译
  产生分歧。阶段 E 所需的 `&mut` 值参数、`Lex.Span {...}` record 构造
  stub 必须**绕开 emit_operand2**（放 emit_arg_one / emit_stmt_let /
  emit_call_args 等外围，逐个小步验证 M2）。
- **规模**：参照 frozen lainc.l1 相关机制约 2000+ 行 LAIN-IR；src/lainc
  复刻预计数百至上千行 Lain，分阶段合入，不一次大爆炸。

## 6. 验收命令

```text
python tests/lainir_lain/run_lainc_m1.py        # →42
python tests/lainir_lain/run_lainc_m2.py        # gen2==gen3 固定点
python tests/lainir_lain/run_lainc_module.py    # module 机制
python tests/lainir_lain/run_lainc_archive_a.py # 阶段 A：多源+import
python tests/lainir_lain/run_lainc_archive_b.py # 阶段 B：std stub
python tests/lainir_lain/run_lainc_archive_c.py # 阶段 C：模块工厂
python tests/lainir_lain/run_lainc_archive_d.py # 阶段 D：工厂体语言面
python tests/lainir_lain/run_compiler_source_closure.py  # frozen 参照（不改）
```

## 7. 下一项

阶段 D（工厂体语言面）已完成并提交。**阶段 E（archive 主干闭包）实施中**
——**核心验收达成**：原版 `tokenizer.lain` + `syntax.lain` 跨文件工厂链
编译产物 **l1check 干净（verifier-valid）**（正式测试 run_lainc_archive_e.py，
提交 c045eb3 等）。

2026-08-20 续：修复了阻碍 archive 链的 record 构造缺口（提交 7987edc、
0fa7d9c）：

- **record 构造 let 绑定丢失**：emit_record_value 用 expr_start（`=` 后，
  可能指向空白）解析 record 名，parse_ident 得到空 span → meta_table_find
  必失败 → 整个 let 静默无输出（`f0_Syntax_parse` 里 `%unit` 未定义）。
  修复：先 trim_start 再 emit_path_tail。
- **限定 record 构造**（`Syntax.Unit {...}`）：emit_stmt_let 只在 RHS 首段
  后是 `{` 时进 record 分支，限定名落到 emit_expr 输出裸 `%Unit`。修复：
  用 emit_path_tail 定位尾段再判 `{`。
- **addr 字段**：record 字段值若为指针形变量（`Memory.String` 等）固定
  `#bits<W>` 声明与 addr 变量不匹配。新增 `meta_type_is_addr`（镜像
  emit_type 决策），emit_record_value 对单标识符字段值查 types 表判定。
- **顶层类型别名**：`let NAME: type = TYPE;` 此前被静默忽略（不登记
  meta kind=4），emit_type 对任何别名回落 addr。compile_source 现在绑定
  kind-4 行，`NodeId` → `#bits<64>` 可解析。
- 附带：emit_type / meta_type_is_addr 支持限定类型别名 `Mod.Member`
  （解析模块行 + meta_lookup_qualified 递归 payload）——library 模式下
  已验证 `SX.NodeId` 参数 → `#bits<64>`。
- 循环内避免 `continue`（frozen 前端会把 `continue` 当变量 emit 成
  `%continue` 未定义；lint：循环体用 if/else 设标志）。

已验证：record 构造 emit alloca+逐字段 store；tokenizer+syntax 链
l1check 干净；阶段 A–D fixture 全绿（5/42）；M2 固定点 gen2==gen3 保持。

**阶段 E 剩余**：

1. **generated_syntax 卡点**：工厂内模块绑定（`let Syntax: Module =
   syntax.Syntax(Memory);` 在工厂体内）+ 成员引用。`clone_from` 的参数
   `Syntax.NodeId` 仍回落 addr——工厂体内嵌套模块绑定的登记/查找路径
   未走通（顶层 library 模式已通，工厂体内不通）。

   已定位的精确行为（跨文件 + 入口模式，qual_type 系列最小复现）：
   - 顶层 `let SX: Module = q.syntax_mod(0);`（限定调用）+ 顶层函数用
     `SX.NodeId` 参数 → 解析为 `#bits<64>` ✅（qual_type8）
   - 工厂体内 `let SX: Module = q.syntax_mod(0);` + 工厂体内成员函数
     `clone_from(id: SX.NodeId)` → 成员 proc 不 emit、`OM.clone_from(7)`
     调用不编译（qual_type9/12/13）
   - 工厂体内成员函数用**普通 i32 参数**也不 emit（qual_type10/11）——
     所以问题不止限定类型，工厂体内模块绑定后的**成员函数 emit** 本身
     就断：eval_module_factory 扫描工厂体时 `let SX: Module = ...` 走
     else → try_bind_module_value，成功后 `current = semi2 + 1` 应继续
     处理 clone_from，但实际未 emit
   - 顶层**裸工厂调用** `let OM: Module = outer_mod(0);` 不被识别
     （try_bind_module_value 要求限定 `mod.fn(...)` 形态）——archive
     自身都走限定调用，此项仅影响测试 fixture
   - `compiler_compile`（单源）模式工厂链完全不工作（qual_type1-7）；
     library 模式是正确基线
   - 下一步：在 eval_module_factory 的 else 分支核对 try_bind_module_value
     调用后 current 的推进（怀疑 scan_semi 的 `end` 参数或
     emit_function2 的返回被 else 分支覆盖）

2. **逐模块扩展**：generated_syntax → types → effects → … →
   compiler_core → compiler_driver → compiler_api → lainc，每模块
   l1check；新增模块可能暴露新语法缺口。
3. 最后实例化 `lainc.API` 运行；验收 24 文件全部编译、产物
   verifier-valid。
