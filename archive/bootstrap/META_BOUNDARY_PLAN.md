# bootstrap Meta 边界与迁移计划

状态：计划，2026-09-16。本文只规定 bootstrap 的 Meta 机制如何分层、当前代码如何迁移和怎样验收；它不表示迁移已经完成。seed 的物理执行边界另见 `../seed/REFACTOR_PLAN.md`，总路线与阶段编号以 `../docs/roadmaps/lain-roadmap.md` 为准。

## 1. 四个明确的归属

| 层 | 应负责的事 | 不得负责的事 |
| --- | --- | --- |
| `bootstrap/compiler` 的 core | 取得源文件、token/RawAst 拓扑、source/span/origin/hygiene、AstApi、通用阶段编排和诊断；组合库与物理 IR/VM provider | 识别 function/type/module/effect/operation 的语言含义；按 Meta 返回类别选 builder 或执行协议 |
| `bootstrap/std` 的 Meta 库 | 宏/attribute 展开、绑定和名字检查、类型/effect/handler 规则、编译期输入检查、layout、完整函数体 lowering；生成物理 LAINIR | 依赖 compiler 目录中的语言专用 descriptor/builder；把未 lowering 的 Lain 源函数交给 VM |
| LAINIR provider | 解析和验证物理 Artifact；定义 `#eval` 结果可否写回产物、执行后改写并消除 `#eval`；给编译器一份 backend 可消费的 Artifact | 管理 Meta AST 对象的语言含义或让编译期临时对象引用进入运行期产物 |
| seed/LAINVM provider | 执行已验证 LAINIR procedure、TCB/activation/预算、内存和外部能力授权、Trap | 解释 Lain 声明、构造 Meta 类型/模块/effect、改写 LAINIR Artifact |

`bootstrap/std` 与正式 `std/` 是两份源码。它们须分别满足同一 Meta 契约、分别通过行为用例；不能用其中一份的通过宣称另一份已经完成。bootstrap 是启动实现，不建立永久的第二套 Lain 语言语义。目录不决定归属：当前放在 `bootstrap/compiler` 的语言规则也须迁到库边界，不能仅移动文件或让库回调原来的 builder。

## 2. 现有调用链与具体越界

当前 `compiler/compiler_api.l1::compiler_compile_request` 建立 `syntax_units_build()` 索引，然后依次调用 `std/entry.l1` 的 `lain_std_expand`、`lain_std_elaborate`、`lain_std_lower`；每步读取 `lain_meta_pass_result_v1` 的 status、result 与 owner。`raw_ast.l1` 只造 Atom/Group 树，`ast_runtime.l1` 把 RawAst 操作暴露为 `lain_ast_v1_*`；`std/macro_expand.l1` 通过这些操作克隆、替换 AST 节点并维护 syntax context。这部分已有库调用路径。

目前的缺口要按行为定位，不因入口名带 `lain_std_` 就认为实现已在库中：

| 当前代码 | 实际做的事 | 迁移/保留判据 |
| --- | --- | --- |
| `compiler/meta_values.l1`、`meta_bindings.l1` | 用编译期记录和 lexical environment 保存 scalar/module/function/type 等 Meta 值 | 对象表示与绑定规则由库拥有；core 只给通用存储/生命周期能力，不按 kind 分派执行 |
| `compiler/meta_collect.l1`、`meta_call.l1`、`meta_module.l1`、`meta_record.l1` | 在源码上识别和构造语言对象，Meta 调用依次尝试专用路径 | 由库的普通函数/工厂语义与统一绑定取代；检查结果须实际进入后续环境和 lowering |
| `compiler/lower_program.l1`、`lower_func.l1` | `lain_std_elaborate_program`/`lain_std_emit_program` 在 compiler 文件中实现；含声明校验、函数体检查和物理 IR writer | 通用 Artifact writer 可保留为 IrApi；语言检查、layout、函数体 lowering 归库；不得保留同名专用函数作为库委托 |
| `compiler/meta_eval_vm.l1`、`meta_call_vm.l1` | 为部分编译期计算合成临时 LAINIR；`meta_entry` 生成 `#eval`，部分对象返回 `#addr` | 完整 lower 原函数体，按物理签名执行；`#eval` 产物与 Meta 对象引用的交付分开，不能让 `#eval -> #addr` 传出临时对象 |
| `std/core_forms.l1`、`std/entry.l1`、`std/macro_expand.l1` | 形式谓词、阶段入口、宏展开已在库 | 扩成完整语义及结果消费；仅识别名字、返回 status 不算完成迁移 |

这些是**现状与待改的代码**，不是为了旧源码继续可用而保留的接口。`compiler/lower_program.l1::lain_std_lower_program` 当前还有标为 Compatibility 的合并入口；先查清所有调用者，若主编译链只用分开的阶段，迁移后删除该入口，不把它纳入目标 Meta ABI。

## 3. 目标执行链和接口

```text
bootstrap host 注入 SourceApi/外部能力
  -> compiler core 建 RawAst、SyntaxIndex 和 CompileContext
  -> std Meta.expand 操作 AstApi，返回阶段结果
  -> std Meta.elaborate 建绑定、检查语义、消费编译期值，返回阶段结果
  -> std Meta.lower 生成可验证的物理 LAINIR Artifact
  -> LAINIR verify + fold #eval（经 VM execute 请求）
  -> 不含真实 #eval 的 runtime Artifact -> backend
```

core 与库之间的阶段接口保留 `context + input -> {status, result, diagnostic, owner}` 的语义，字段和所有权以行为用例验证；阶段失败不能缓存或发布部分源码/Artifact。AstApi 只暴露节点拓扑、文本、span、origin/hygiene 和对象生命周期；库独立决定节点的语言含义。IrApi 只暴露物理过程、类型、数据、指令的构造/验证/打印；库生成的计算有明确的 LAINIR 文本和物理签名。

Meta 若要在编译期执行函数，先由库 lower 成已验证的物理 LAINIR procedure，显式提供参数、环境输入、内存/对象授权与预算。seed VM 只运行该物理 procedure 并给出物理结果或 Trap；LAINIR 的 `#eval` fold 决定哪些结果能写入 Artifact。Meta AST 对象引用留在编译上下文，由 AstApi/对象协议解释，不能通过 `#eval` 产物结果交付。VM 操作必须经 LAINIR 可表述的调用边界；现有 `bootstrap.vm-artifact-*`、`vm-arguments-*`、`vm-eval-*` 是需审计的启动入口，不自动成为最终稳定 ABI。对象引用的物理编码和执行请求的授权字段须与 seed 计划的第 1 阶段共同定稿。

`#eval` 不能代替 Meta 对 AST 的读写，VM 不能代替库处理函数/type/module/effect；也不新增绕过 LAINIR 的源语言函数解释协议。外部 I/O 由宿主 capability 注入，不能成为 core 或 Meta 的全局状态。

## 4. 实施片与交付

1. **接口和调用证据。** 画出 `compiler_compile_request -> std/entry -> compiler/lower_program -> seed` 的实际调用图；列出全部 `lain_std_*`、`lain_ast_v1_*`、`bootstrap.vm-*` 的调用者、参数物理类型、对象所有权及失败处理。用一个宏 AST 改写、一个类型/函数绑定、一个普通 `#eval` 和一个权限拒绝用例固定边界。若现有 LAINIR 物理值无法表达对象引用/授权，按路线图 §15 给最小复现与两个方案，先讨论再改物理语义。
2. **库阶段语义。** 按路线图编码 1e–1h 把剩余宏/attribute、effect/operation/handler、module/type、普通编译期函数体和显式输入绑定迁到 `bootstrap/std`；每片删除被取代的 compiler 专用分派与 builder。迁移后检查库不回调这些语言专用函数，并验证 Meta 值实际被环境、下一阶段及最终 IR 消费。
3. **统一 lowering 与求值交接。** 由库的完整函数体 lowering 生成物理 IR，展示求值前含 `#eval` 的 Artifact、seed verify/execute/fold 的结果与求值后 Artifact。除去 `meta_entry` 用 `#eval -> #addr` 传对象引用的路径；AST 对象协议与 `#eval` 结果协议分别有成功、失败和生命周期用例。Trap 变编译诊断，backend 输入没有真实 `#eval`。
4. **双实现验证与清理。** bootstrap 运行用例与正式 `std`/`src/lainc` 的相同契约用例分别执行；补齐阶段结果释放、origin/hygiene、跨模块绑定、资源耗尽和无隐式输入反例。更新源码闭包与 bundle，删除无调用者的旧入口；只把通过实际执行的切片归档，路线图继续列未完成项。

## 5. 每片完成证据

每个改动必须提交最小正例与反例、生成的求值前/后 LAINIR、实际运行结果或稳定诊断、调用者及源码闭包差分。验证入口至少覆盖 `python scripts/build_lain_compiler.py`、`python scripts/check_meta_ast_conformance.py`、`python scripts/check_meta_stage_swap.py`、`python scripts/check_stdlib_swap.py`、`python scripts/check_comptime_function_calls.py`、`python scripts/check_eval_tcb.py` 和 `python scripts/check_lainc_lainir_api_baseline.py`；有前置产物缺失时先构建，不把脚本缺产物记为语义失败。正式编译器的执行组合仍需路线图编码 5 的单独验收。

当前 `check_comptime_function_calls.py` 的 `transitive` 用例尚失败，不能将本计划或现有单表达式执行路径写成“普通编译期函数已完成”。只有所有阶段与消费者的行为证据通过，才移除路线图中的对应未完成项。
