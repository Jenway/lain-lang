# 编码 1b 设计：统一的 Meta callable 执行

状态：设计稿（2026-09-12）。对应 [`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md) §7（编码 1）。
本文只描述方案，不改代码。编码 1a（管道统一）已完成并提交。

## 1. 目标与非目标

**目标**：bootstrap 中只有一条 Meta callable 执行路径——按 callable 的签名和 body 工作，而不是按返回类别分派。

**非目标**：不改变语言语义；不引入第三套编译器；不恢复 `EvalResult` 或任何按 Meta 返回类别选择执行协议的做法。

## 2. 现状：四道栅栏

### 2.1 通用 lowering 有意跳过 Meta 函数

```lainir
// lower_program.l1:8827  program_write_function
#if #call program_function_is_meta(%source, %function) { #return }
```

判据在 `lower_program.l1:501` `program_function_is_meta`：返回 `Module` / `ModuleShape`、返回 std 类型成员（`std::type`），或函数体含 Meta 构造器（`program_node_has_meta_constructor:453`）。

注释说明这是**阶段边界**：避免 Meta-only 的 module 返回泄漏成物理 `%module`。

### 2.2 两条路径写向不同 sink

| 路径 | 写入目标 | 证据 |
| --- | --- | --- |
| 通用 lowering | `bootstrap.artifact-write-*` | `lower_program.l1:8738-9268` 区间内 41 处 literal、3 处 byte |
| Meta 调用路径 | `lainvm_eval_buffer_append_*` | `meta_call_vm.l1` 内 36 处 literal 等 |

`lainvm_eval_buffer_*` 实现在 `meta_eval_vm.l1:22-93`，是手工 buffer。

### 2.3 Meta 路径的表达式能力是最小切片

`lainvm_meta_write_scalar_expression`（`meta_call_vm.l1:130`）只支持：操作数为十进制字面量或单个 `%参数`；表达式为**一个**二元运算（`+` / `-` / `*` / 否则 `sdiv`）。

对比通用 lowering 处理 `let`、`while`、`if`、effect 调用、赋值语句、record 字面量（`program_write_expression:8738`）。

### 2.4 module / effect 的 builder 不 lower body

`lainvm_meta_build_module_artifact`（`meta_call_vm.l1:408`）与 `lainvm_meta_build_effect_artifact`（`:441`）直接把「指向语法的 Meta 值」写死进 LAINIR：

```lainir
#store 2, #lea(base=%result, idx=0, scale=0, offset=0)   // kind 2 = module
#store %meta_source, ... offset=8
#store %meta_start,  ... offset=16
```

随后由 `program_collect_module` 从语法惰性收集成员。**这是有意的表示方式**——模块值引用其语法，而不是物化已收集的成员表。

因此路线图 §7.2 步骤 4「使用现有 lowering 设施把 callable body lower 成临时 LAINIR procedure」的前提**今天不成立**：设施存在（`program_write_expression` 等），但被 §2.1 的栅栏隔开，且写向另一个 sink（§2.2），并且缺少 Meta 值构造这条规则（§2.4）。

## 3. 关键约束（已取证）

### 3.1 阶段不重叠——sink 在 Meta 求值时是空闲的

```text
lain_std_lower_program (lower_program.l1:8979)
  program_collect_module(...)          <- 收集阶段；Meta 调用在这里求值
  program_validate(...)
  program_unit_set_writing(unit, 1)
  artifact_begin()                     <- 发射阶段开始
  program_write_function(...)
  artifact_finish()
```

Meta 调用求值发生在 `artifact_begin()` **之前**，两者不并发。这消除了「sink 重入」这一类风险。

### 3.2 artifact sink 是文件，而 vm-artifact-parse 要内存

```c
// seed/src/host/bootstrap.c:2016
static LainirRunStatus artifact_begin(...) {
  if (count != 0 || context->artifact) {      // 已打开则报错
    *error = "bootstrap.artifact-begin has invalid state";
```

sink 是单一文件句柄（`fopen(context->artifact_path, "wb")`），而 `bootstrap.vm-artifact-parse(#addr %data, #bits<64> %length)` 需要**内存中的字节**。

### 3.3 能力清单中没有内存 artifact sink

宿主现有能力（`bootstrap.c` 的 `add_capability` 全量）覆盖：`artifact-begin/write-byte/write-literal/write-span/write-identifier/finish`（流式写文件）、`write-artifact(addr,len)`（一次性写文件）、`vm-artifact-parse(data,len)`（读内存）。

**没有任何能力能让 LAINIR 产出内存中的 artifact 文本。**

### 3.4 buffer 有 4096 硬上限且静默截断

```lainir
// meta_eval_vm.l1:39-40  lainvm_eval_buffer_append_byte
#if #sge(%length, 4096) { #return }    // 无诊断、无错误，静默丢弃
```

当前所有 Meta artifact 都远小于 4096，所以未暴露；这是个「下游 verifier 报错、源头看起来正常」的失败模式。

## 4. 设计

### 4.1 sink 统一：给宿主加内存 capture 模式

通用 lowering 一次都不改——它继续调用 `bootstrap.artifact-write-*`。改的是**宿主为该能力族增加一个内存后备模式**：

```text
bootstrap.artifact-capture-begin()                       -> #unit
bootstrap.artifact-capture-end()                         -> #unit
bootstrap.artifact-capture-data()                        -> #addr
bootstrap.artifact-capture-length()                      -> #bits<64>
```

capture 活动期间，`artifact-write-*` 写入内存缓冲而非文件；`artifact-capture-end` 之后恢复文件模式。缓冲动态增长，不设固定上限。

理由（路线图 §2）：这属于「宿主能力确实不足」，是允许修改 C seed 的情形。改动量小（一个上下文缓冲 + 四个入口 + 在 `artifact_write_*` 里加一次模式判断），且不改变任何现有调用的行为。

**替代方案（更小暴露面）**：只加 `bootstrap.vm-artifact-parse-capture()`——直接解析当前 capture 内容，不把 data/length 暴露给 LAINIR。两者都可行；本文推荐前者，因为它与现有「addr + length」风格一致，且 capture 内容将来可能还需要做别的用途（例如写入诊断）。

**被否决的方案**：保留 buffer 只修截断。它不统一 sink，两条路径继续并存，且 buffer 的 4096 上限与 artifact sink 的无上限行为不一致。

### 4.2 构造器名硬编码：一个必须先记录的违规

本节最初提出的方案是「建一张 Meta 值构造规则表，按 `std::module`、`std::effect` 等名字
查表」。**该方案方向错误，已废弃**：它把既有的设计违规编码成设计，只是把七个 `if` 换成
七个表项。

#### 4.2.1 现状：bootstrap 认识七个构造器名，形式实现一个都不认识

`lower_program.l1:453` 的 `program_node_has_meta_constructor` 用字面量比较识别
`std::module`、`std::effect`、`std::handler`、`std::effect_operation`、
`std::type_with_namespace`、`std::handler_type`、`std::meta_type` 七个名字；
`program_function_is_meta`（`:501`）另加 `Module`、`ModuleShape`；
`program_is_ignored_declaration`（`:2805`）另加 `struct`、`module`、`import`。

全仓库字面量出现次数（`"名字"` 形式）：

| 名字 | `bootstrap/compiler` | `src/lainc` |
| --- | --- | --- |
| `module` | 7 | 0 |
| `struct` | 5 | 0 |
| `effect` | 3 | 0 |
| `handler` | 2 | 0 |
| `effect_operation` | 3 | 0 |
| `type_with_namespace` | 1 | 0 |
| `handler_type` | 1 | 0 |
| `meta_type` | 1 | 0 |
| `import` | 6 | 1 |

唯一两者都出现的是 `import`——它确实特殊，因为需要路径解析
（`src/lainc/elaborator.lain:575`）。

#### 4.2.2 正确形态：名字无关，由库决定含义

`src/lainc/elaborator.lain:661`：

```lain
} else if body != Syntax.invalid_node() {
    // 带 {...} 体的声明；名字是什么不参与判断
    elaborate_function_body(...)
```

形式编译器只区分两件事：**是不是 `import`**（需要路径），以及**有没有 `{...}` 体**。
`std::module { ... }` 与 `std::func() { ... }` 走同一条路径，含义由库的绑定决定。

这与 [`../00-intro.md`](../00-intro.md) §3 一致：

> `foo(x)` 在 RawAst 中只表示为一个名字后面跟着圆括号组。它可能是运行时调用、类型工厂
> 调用、effect 应用、宏调用或 DSL 形式。**Meta 根据绑定和上下文决定它的含义。**

也与 [`../03-meta-system.md`](../03-meta-system.md) §2 一致：Parser 不判断
`std::func`、`std::struct`、`std::module`、`import`、类型应用或 effect 的含义。

**编译器该提供的是原语，不是名字表**：「构造一个 kind=N 的 Meta 值，带这些字段」，
就像它给类型构造提供原语一样。至于什么源形式对应哪种构造，属于库。

#### 4.2.3 为什么会这样，以及它不该在 bootstrap 里修

要做到名字无关，lowering 必须已经知道每个表达式的绑定与类型——那是 elaborate 的产出。
`src/lainc` 有这一层（`elaborate_function_body` 产出 `ExprId`），bootstrap 没有。
按 [`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md) §3.2，部分 `elaborate`
语义尚未落地。

**所以七个名字是 elaborate 缺失的症状，不是设计选择。**

而 bootstrap 的定位是启动工具、不是长期架构（`bootstrap/README.md`）。**在过渡实现上
精修这件事没有价值**——正确路径是等形式实现接管（路线图编码 4/5），届时
`program_node_has_meta_constructor` 与 `program_function_is_meta` 整体消失。

结论：**1b 不新增任何名字匹配；本议题记录在案，不在 bootstrap 中推进。**

### 4.3 统一入口的算法

```text
1. 从环境解析 callee，确认是 callable Meta 值
2. 从 callable 的静态签名取显式参数与返回类型
3. 建立 invocation environment，按序绑定实参；参数个数/类型错误在执行前诊断
4. lower callable body：
     - 运行期部分：program_write_expression 的现有规则
     - Meta 值构造：**不做名字匹配**（§4.2）；本阶段只沿用现有构造路径
     写入 capture sink（§4.1）
5. 把 Meta 实参按签名要求的物理类型（bits / addr / unit）写入 VM argument vector
6. parse capture 内容 -> 找入口 procedure -> 执行
7. 只按 LAINIR 物理返回类型读取 bits / addr / unit
8. 按 callable 的静态返回类型解释该物理结果，绑定回调用环境
9. Trap 直接转编译诊断；Trap 时不得制造普通返回值
```

这与路线图 §7.2 的九步一致，只是把步骤 4 的实现前提（§4.1）补齐，并把名字匹配排除在
本阶段之外（§4.2.3）。

每片独立验证、可单独回退。

| 片 | 内容 | 验收 |
| --- | --- | --- |
| **1b-1** | 宿主加 capture 模式（§4.1）。纯 C 改动，无调用者变化 | `zig build`；现有全部 gate 不变 |
| **1b-2** | `meta_eval_vm.l1` 的 buffer 与 `meta_call_vm.l1` 的写入改为 capture sink；删除 `lainvm_eval_buffer_*` | `check_bootstrap_consteval.py`（5 类 fixture 全过） |
| ~~1b-3~~ | ~~抽出 Meta 值构造规则表~~ **已废弃**（§4.2：方向错误，不新增名字匹配） | — |
| ~~1b-4~~ | ~~effect / type / AST 值接入规则表~~ **已废弃**（同上） | — |
| **1b-3** | 先只把 module 的构造从手写 LAINIR 改为**复用现有调用路径**（不做名字表，不新增匹配） | `formal_meta_module_factory.lain` + 全量 baseline |
| **1b-4** | 4 个入口收敛为单一路径；删除 §6 的清单 | 全量 baseline + §7.4 |
| **1b-5** | 把 scalar 的最小 expr writer（§2.3）退役，改用通用 lowering | 新增含 `let`/`if`/调用 的 Meta callable fixture |

**1b-1 与 1b-2 是 sink 统一**，与构造器名争议无关，可独立推进。

**1b-3 之后的能力收敛依赖 elaborate 的产出**（§4.2.3）。若 elaborate 尚不能提供绑定与类型
判定，1b-3 到 1b-5 应推迟到编码 2/3 之后，或与它们交错推进。

**1b-5 必须放最后**：它是唯一会放大可 lower 表达式集合的一步，风险最高，且需要完整的
参数与返回类型信息。

## 6. 删除清单（来自路线图 §7.3）

```text
lainvm_meta_scalar_function_supported
lainvm_meta_build_scalar_artifact
lainvm_eval_meta_scalar_call
lainvm_meta_module_factory_group
lainvm_meta_build_module_artifact
lainvm_eval_meta_module_call
lainvm_meta_effect_factory_node
```

外加本文新增的删除项：`lainvm_eval_buffer_*`（1b-2 之后零调用者）。

**不在 1b 删除**：`program_node_has_meta_constructor`、`program_function_is_meta`
（§4.2.3）。它们随形式实现接管而消失，不在这里精修。

每项删除前必须先确认零引用（用全仓库搜索，排除 `build/` 与 `.git/`）。

## 7. 测试清单（来自路线图 §7.4）

```text
scripts/fixtures/formal_meta_scalar_call.lain          （已有）
scripts/fixtures/formal_meta_module_factory.lain       （已有）
新增：返回 std::type 的 callable
新增：返回 AST handle 的最小 callable
新增：effect factory 与 effect operation              （部分已有）
新增：主动触发 Trap 的 Meta callable
```

要求：每个成功 artifact 必须由 verifier 接受、能运行到预期结果、且不含 `#eval`；失败用例检查诊断码与 source span。

建议新建 `scripts/check_meta_callable_execution.py`，而不是继续把用例堆进 `check_bootstrap_consteval.py`——后者已经在承担 5 类路径的回归。

## 8. 风险与决策点

### 8.1 已解决：构造器名硬编码（原「规则表归属」问题已作废）

本节原先是「规则表该住在 bootstrap 还是主 lowering」。该问题随 §4.2 的重写而作废——**不建表**。

真正的结论：bootstrap 的构造器名硬编码是 elaborate 缺失的症状，应从形式实现接管中消失，
不在 bootstrap 里精修。1b 不新增任何名字匹配。

### 8.2 修改 C seed 的合规性

§4.1 需要改 `seed/src/host/bootstrap.c`。路线图 §2 允许在「宿主能力确实不足」时修改。内存 capture 确实不存在（§3.3），所以合规。但应在提交信息里写明这一判定依据。

### 8.3 4096 静默截断

1b-2 会顺带消除它（capture 动态增长）。若 1b-1/1b-2 延迟，建议先把 `lainvm_eval_buffer_append_byte` 的静默丢弃改成诊断，作为独立的低风险修复。

### 8.4 不改变的行为

以下必须逐字保持：

- 诊断码 5108、5111、5110、3101 及其触发条件；
- 4 个入口的对外签名（在收敛完成前）；
- module / effect 的 Meta 值布局（offset 0/8/16/24/32 等），因为 `program_collect_module` 依赖它。

## 9. 与其他阶段的关系

- **编码 0**（已完成）：`std::type` 与标准根环境。1b-4 的「返回 std::type 的 callable」fixture 依赖它。
- **编码 2**（`std::func` 完整签名）：§4.3 步骤 2「从 callable 的静态签名取参数与返回类型」需要签名可被读取。当前只能用 `program_function_params` 与返回类型节点，这会限制完整性。
- **编码 4/5**（形式实现接管）：§4.2 记录的名字硬编码在这两个阶段消失。

**排期结论**：先推进 1b-1/1b-2（sink 统一，与上述依赖无关）；1b-3 及其后的能力收敛推迟到
编码 2/3 之后，或与它们交错。
