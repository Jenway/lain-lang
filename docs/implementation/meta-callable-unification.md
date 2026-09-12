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

### 4.2 Meta 值构造规则

通用 lowering 需要能 lower 表达式位置的 Meta 值构造。这不是新的语言语义，而是**把 §2.4 手工写死的构造改成从节点推导**：

| 源形式 | Meta 上下文中的 lowering |
| --- | --- |
| `std::module { ... }` | 构造 kind=2 的 Meta 值：source、本节点 span、当前 environment |
| `std::effect(...)` | 构造 kind=4 的 Meta 值（现有 effect builder 的形状） |
| 类型值（`std::type` 成员） | 构造类型值引用 |
| AST handle | 构造 AST 值引用 |

新增的是一张「Meta 值构造」规则表，由 `program_write_expression` 在 Meta 上下文下查表；普通（运行期）上下文不查表，行为不变。

**这条规则必须有明确的归属**：它是 Meta 层的能力（`std/` 定义），不是 compiler core 的语义硬编码。因此实现位置应在 bootstrap 的 Meta 部分（`meta_call*`、`meta_values`），而不是 `lower_program` 的运行期 lowering 主路径——`program_write_expression` 只需在 Meta 上下文下委托给 Meta 值的构造器。

这一点需要在动手前再确认（见 §8）。

### 4.3 统一入口的算法

```text
1. 从环境解析 callee，确认是 callable Meta 值
2. 从 callable 的静态签名取显式参数与返回类型
3. 建立 invocation environment，按序绑定实参；参数个数/类型错误在执行前诊断
4. lower callable body：
     - 运行期部分：program_write_expression 的现有规则
     - Meta 值构造：§4.2 的规则表
     写入 capture sink（§4.1）
5. 把 Meta 实参按签名要求的物理类型（bits / addr / unit）写入 VM argument vector
6. parse capture 内容 -> 找入口 procedure -> 执行
7. 只按 LAINIR 物理返回类型读取 bits / addr / unit
8. 按 callable 的静态返回类型解释该物理结果，绑定回调用环境
9. Trap 直接转编译诊断；Trap 时不得制造普通返回值
```

这与路线图 §7.2 的九步一致，只是把步骤 4 的实现前提（§4.1、§4.2）补齐。

## 5. 切片计划

每片独立验证、可单独回退。

| 片 | 内容 | 验收 |
| --- | --- | --- |
| **1b-1** | 宿主加 capture 模式（§4.1）。纯 C 改动，无调用者变化 | `zig build`；现有全部 gate 不变 |
| **1b-2** | `meta_eval_vm.l1` 的 buffer 与 `meta_call_vm.l1` 的写入改为 capture sink；删除 `lainvm_eval_buffer_*` | `check_bootstrap_consteval.py`（5 类 fixture 全过） |
| **1b-3** | 抽出 Meta 值构造规则表（§4.2），先只让 module 走它 | 新增 module factory fixture + 现有 `formal_meta_module_factory.lain` |
| **1b-4** | effect / effect_operation / type / AST 值分别接入规则表 | §7.4 的六类 fixture |
| **1b-5** | 4 个入口收敛为单一路径；删除 §6 的清单 | 全量 baseline + §7.4 |
| **1b-6** | 把 scalar 的最小 expr writer（§2.3）退役，改用通用 lowering | 新增含 `let`/`if`/调用 的 Meta callable fixture |

1b-1 与 1b-2 是 sink 统一；1b-3 到 1b-6 是能力收敛。**1b-6 必须放最后**，因为它是唯一会放大可 lower 表达式集合的一步，风险最高。

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

### 8.1 需要先确认：Meta 值构造规则的归属

§4.2 把规则表放在 bootstrap 的 Meta 部分、让 `program_write_expression` 委托。但也可以反过来：扩展 `program_write_expression` 自身，在 Meta 上下文下直接处理这些形式。

两者的差别是**边界**：前者保持 compiler core 的运行期 lowering 不知道 Meta 值；后者把 Meta 知识加进主 lowering。按 `docs/03-meta-system.md` §2 的分层，**前者更符合**。但这需要与现有 bootstrap 的模块划分对上（`program_write_expression` 在 `lower_program.l1`，而 Meta 值在 `meta_values.l1`）。

**这是本设计里唯一必须先定的边界问题。**

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
- **编码 2**（`std::func` 完整签名）：1b 的步骤 2「从 callable 的静态签名取参数与返回类型」需要签名可被读取。**若签名尚未可读，1b 的步骤 2 只能继续用现有的 `program_function_params` / 返回类型节点**，那会限制 §4.3 的完整性。这可能意味着编码 1b 与编码 2 需要交错推进，或 1b 先做 sink 与值构造、把「按签名绑定」留到编码 2。

**这一点需要在排期时确认。**
