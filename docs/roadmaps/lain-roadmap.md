# Lain 编译器执行路线图

整理与核验日期：2026-09-13。本文只保留未完成工作、执行约束、依赖与验收条件。
已实现切片及验收记录统一见 [`历史索引`](../history/README.md)。
完成一个切片后先追加完成快照，再从本文移除；历史验收不代表当前产物已重新通过。
阶段编号保持原编码编号，删除完成项后不重编号，便于引用。

## 1. 术语与执行链

- RawAst：仅包含 Atom/Group、拓扑、文本位置、origin/hygiene 的无语义源码树。
- Meta：Lain 在编译期操作 AST 的语言定义层，承担 expand、elaborate、lower。
- LAINIR：确定物理类型和操作的 IR；`#eval` 表达编译期执行。
- LAINVM：执行已验证的物理 IR，提供 TCB、VSpace、预算和 Trap 原语。
- 输入行 `?{}`：由调用环境供应的具名编译期输入；输出行 `!{}`：执行期间可能发出的 operation。
- 固定点：编译器生成下一代编译器后，连续两代规范化产物一致，并有代际执行验证。

```text
Lain source -> RawAst -> library Meta expand/elaborate/lower
            -> LAINIR verify + execute #eval -> runtime LAINIR -> backend
```

## 2. 实现归属与产物约束

| 位置 | 职责与修改约束 |
| --- | --- |
| `seed/` | C 物理 parser/verifier/VM 与宿主能力；仅补真实缺失的物理机制，不实现 Lain 语言规则 |
| `bootstrap/compiler/`、`bootstrap/std/` | 启动编译器与手写 LAINIR 库；本次语义迁移须把规则放进库阶段 |
| `src/lainc/`、`std/` | 正式 Lain 实现；通过同一 Meta ABI 组合，不能与 bootstrap 当成同一份代码 |
| `src/lainir/`、`src/lainvm/` | 活跃契约；正式 provider/VM 的运行能力提供方式待编码 5 决定 |
| `build/` | 所有生成产物、快照、差分报告；永不提交，不修改产物来通过验收 |

正式 provider/VM 的归档实现只用于追溯，不能直接回到源码闭包充当已验证实现。
能力由组合层显式注入，状态通过参数传递；compiler core 不导入具体 VM/provider 实现。

## 3. 未完成依赖与证明缺口

- bootstrap 的 AST/语义处理仍有 operation/module/effect/scalar 专用分派，需编码 1 迁移。
- 正式库与 lainc 尚缺完整签名语义及运行验证；库策略输入的完整正例尚未打通（编码 2/3）。
- `Eval` 的契约归属仍需从 LAINVM 迁到 LAINIR，正式组合层执行链需接通（编码 5）。
- `srclainc.l1` 缺约定可执行编译入口；两次构建一致只能证明源码闭包确定性（编码 6）。
- 现有 LAINIR 编译器自举 gate 不能证明 Lain 编译器固定点；旧 profile 脚本的入口假设需修订。
- native host ABI、剩余物理指令、alloca 生命周期及发布矩阵尚待收口（编码 7）。

## 4. 执行中的硬性边界

### 4.1 类型与函数规则

`std::type` 是标准根环境中的 Meta 值，不是 Parser 关键字或 `import("std::type")` 模块。
类型参数通过普通函数/工厂参数传递；不增加专用 generic Parser 节点、不按返回类型选择
工厂执行协议、不恢复裸 type、旧 comptime 修饰符或 generic policy。

```text
std::func(显式参数) ?{环境输入} -> 返回类型 !{输出 effect} {函数体}
```

`?{name}` 等价于 `?{name: _}`，约束由完整签名与库语义提供；输入/输出使用不同静态字段。
不得隐式捕获未声明自由变量，不自行引入运行时动态隐式输入。

### 4.4 物理执行边界

库拥有函数、类型、模块、effect、operation/handler 等规则及语义对象；core/VM 只提供通用
AST/IR/执行机制。编译期计算经过显式 LAINIR `#eval`；其执行 handler 在编译组合边界安装。
按物理 bits/addr/unit 读取结果，Trap 转诊断；不得按 Meta 返回类别选择执行协议，不恢复
EvalResult、Meta object kind/owner/generation/sidecar 包装协议。

effect 的 TCB/CPS 实现策略由库 handler 在编译期决定。见
[`../stdlib/effect-system.md`](../stdlib/effect-system.md) 与
[`../03-meta-system.md`](../03-meta-system.md)。

## 5. 剩余实施顺序

| 阶段 | 状态/依赖 | 剩余产出 |
| --- | --- | --- |
| 编码 1 | 1e 部分实现，剩余边界与 1f–1h 优先推进；在 bootstrap 链验证 | 库实现语言语义，统一生成 LAINIR 与 `#eval` |
| 编码 2（剩余） | 纳入编码 1 的库语义迁移；正式运行验收依赖编码 5 | 正式签名语义、类型检查与未覆盖形状一致性 |
| 编码 3（剩余） | 库符号/成员调用及完整策略输入依赖库语义能力 | `?{T, ord: Ord(T)}` 完整正例与严格类型约束验收 |
| 编码 5 | 先选 provider/VM 提供路径，接收编码 1 的共同契约 | 正式 lainc 的可运行编译期求值组合 |
| 编码 6 | 依赖编码 5 的可运行编译器 | Lain 编译器 gen1/gen2/gen3 执行及 gen2 == gen3 固定点 |
| 编码 7 | 物理后端/ABI 工作可独立推进；最终 native/发布验收依赖编码 6 | 后端语义覆盖、native matrix、CI 与发布 |

编码 2/3 的剩余库语义工作与编码 1 一并安排，避免重复建设；正式编译器行为验证在编码 5
运行组合接通后执行。每片具有独立执行验收，完成后从本表及正文移除对应工作。

## 7. 编码 1：将语言语义归还库 Meta 层，统一生成 LAINIR 与 `#eval`

计划修订：2026-09-13。设计与迁移清单见
[`../implementation/meta-callable-unification.md`](../implementation/meta-callable-unification.md)。
本节跟踪未完成的 1e–1h。基础与路径审计证据见完成归档。

### 7.0 目标与边界

Meta 是 Lain 在编译期操作 AST 的语言定义层，负责 expand、elaborate 和 lower。
函数、类型、模块、effect、operation 和 handler 的识别、构造、检查及 lowering 规则由库拥有。
需要编译期计算时，库 Meta 生成 LAINIR `#eval`；LAINIR 定义编译期执行，LAINVM 提供执行
已验证物理 IR 的能力。不得绕过 `#eval` 建立另一套源语言函数求值协议。

compiler core 只组合 SourceApi、AstApi、IrApi、物理执行能力及通用的阶段结果、诊断和生命周期
机制。不得提供 make-effect、make-module 等语言专用宿主原语，也不得按 Meta 返回类别选择
artifact builder 或 VM 执行协议。签名与库内部类型表示用于语义检查；物理执行只处理
Artifact、Procedure、bits/addr/unit、VSpace、预算与 Trap。

本阶段先在 bootstrap 链证明上述边界。`bootstrap/std/` 与 `std/` 共用 Meta ABI 契约，
但不是同一份实现；Lain 语言语义不得通过修改 C seed 实现。正式 lainc 的运行组合仍属编码 5。

### 7.1 当前债务与迁移范围

以下实现虽然位于 `bootstrap/compiler/`，实际属于 `stdlib.l1`，不在 compiler core 中。
`program_collect_meta_call_candidate` 依次尝试 effect operation、module factory、effect
factory 和 scalar function。scalar 路径使用简化表达式 writer；module/effect 路径扫描函数体，
构造引用语法与环境的描述值，并未完整 lower 原函数体；这些路径已有显式 `#eval`，
债务是库语义与生成覆盖不足，不能按目录误判为 core 越界。通用 writer 又通过
`program_function_is_meta` 跳过相关函数。这些是需要替换的启动实现。

剩余阶段边界债务：完整宏绑定、attribute/跨模块展开与正式库的 elaborate/签名语义
仍需补齐；跨阶段失败、非法 payload 与更广的
origin/hygiene 保留继续验收。具体调用与当前实现证据见 1e 部分实现快照。

重点审计并按真实依赖更新闭包：

```text
bootstrap/compiler/meta_call.l1
bootstrap/compiler/meta_call_vm.l1
bootstrap/compiler/meta_eval_vm.l1
bootstrap/compiler/meta_values.l1
bootstrap/compiler/meta_collect.l1
bootstrap/compiler/meta_module.l1
bootstrap/compiler/meta_record.l1
bootstrap/compiler/lower_func.l1
bootstrap/compiler/lower_program.l1
bootstrap/std/entry.l1
bootstrap/std/core_forms.l1
std/meta.lain
std/bootstrap/abi_entry.lain
```

文件移动、函数改名或加一层委托不能作为迁移证据。库实现只能依赖通用能力与库内部规则，
不得通过回调 compiler core 的 effect/module builder 保留原有语义依赖。

### 7.2 新实施顺序

编号使用 1e–1h。每片独立提交、可执行验收后再进入下一片。

| 片 | 工作 | 完成条件 |
| --- | --- | --- |
| 1e：剩余库阶段边界 | 补齐完整宏绑定/attribute、正式库阶段规则与跨阶段失败/资源/metadata 验收 | core 仅保留通用 AstApi；库阶段结果消费、origin/hygiene、诊断及失败资源路径有完整执行证据 |
| 1f：effect 完整迁移 | 在库实现 effect 构造、operation、调用检查和一个 handler 的 lowering；替换对应 compiler 专用路径 | 含 operation/handler 的程序实际运行正确；缺失 effect、签名不匹配等反例有稳定诊断和 span；库不回调旧 builder |
| 1g：显式编译期求值 | 库 Meta 为所需计算生成 `#eval`；通过共同 LAINIR 验证/求值边界执行；库消费物理结果或诊断 | 求值前可验证产物含所需 `#eval`；求值后结果正确、backend 输入不含 `#eval`；Trap 不伪装成功 |
| 1h：其余迁移与清理 | 沿同一边界迁移 module/type 等规则，退役 scalar 最小 writer、函数体扫描分派及专用 artifact builder | 全部行为回归通过；无语言类别驱动的执行协议；无替代命名的专用捷径 |

1f 复用现有生成显式 `#eval` 的物理链，建立真正消费 effect/operation 与 handler 的运行证据。
1g 补齐完整计算覆盖与结果/Trap/生命周期验证；不能因为现有 fixture 返回 42 就认定
effect 行为已被执行。新增路径不得绕过 `#eval`。

### 7.2.1 旧堵塞的重新归属

- `meta_value_kind = 4` 同时用于类型值和 effect：审计其调用者，在库内部确定表示与类型检查
  规则。不得给 compiler core 增加 effect kind 来选择执行协议。
- `formal_meta_effect_factory.lain` 声明 `-> Module` 却返回 effect：由库返回类型规则验证。
  正例签名依据 `std/allocation.lain` 的 `-> effects.Effect`，其中 `effects` 显式导入
  `std::effect`；旧 `-> Module` 保留不匹配反例，不得靠专用分支容忍矛盾。
- 旧 1b-5 的 scalar writer 退役：纳入 1h，由库 lowering 与普通物理 IR 表达式生成覆盖，
  不再按“Meta 函数”类别另建表达式语言。

effect 返回类型不匹配仍被接受，需补齐库侧返回类型校验、诊断 span 与正式侧一致性。
operation/handler 的验收必须运行并消费其结果。

现有 ABI 是否足以传递库阶段结果、生成 IR 与消费 `#eval` 结果，按阶段交接快照与实际调用链验证，不从字段存在推断语义完成。
仅缺通用能力时提出最小契约补充；遇到 §15 的条件仍须最小复现与两个方案，停止相关实现。
本次计划修订已经确定语言语义归属，不再把“库只识别名字还是完整实现 effect”列为待决定项。

普通 lowering 的执行反例：`check_local_assignment_types.py` 包含局部/参数 addr、
bool、推导类型、遮蔽恢复与后声明绑定。当前默认输出把条件块中的 addr 赋值写成
bits<64>，verifier 返回 2016；遮蔽还会生成重复 IR 名称（2011）。库需沿词法绑定
保留类型，用声明与赋值共有的类型输出，并生成确定且唯一的 IR 绑定名称；
保持参数循环更新与初始化期间的绑定可见性。不得通过修改 VM 语言策略或删除失败用例验收。

### 7.3 删除目标

对应库规则与运行验收通过后，逐项删除或拆出可复用的纯物理部分：

```text
program_collect_meta_call_candidate 中按 effect/module/scalar 分派的逻辑
lainvm_meta_scalar_function_supported
lainvm_meta_write_scalar_operand
lainvm_meta_write_scalar_expression
lainvm_meta_build_scalar_artifact
lainvm_eval_meta_scalar_call
lainvm_meta_module_factory_group
lainvm_meta_build_module_artifact
lainvm_eval_meta_module_call
lainvm_meta_effect_factory_node
lainvm_meta_build_effect_artifact
lainvm_eval_meta_effect_call
lainvm_meta_build_effect_operation_artifact
lainvm_eval_meta_effect_operation
```

`program_function_is_meta` 的跳过规则须由库的阶段处理取代；不得直接删除检查，让未消解的
语言对象泄漏进物理 IR。描述值布局属于库内部表示，迁移时审计全部消费者并保持 ABI 契约，
不把现有 kind/offset 布局升级为 compiler 或 VM 的公共协议。

删除前搜索全部源码引用，排除 `build/` 与 `.git/`；重建产物验证闭包完整。
文本搜索只用于确认删除，不替代行为验证。

### 7.4 验收与完成条件

以下独立检查入口仍待实现，登记到 `scripts/README.md` 与 baseline gate 列表：

- `scripts/check_library_effect_lowering.py`：运行含 effect operation/handler 的正例，验证结果；
  反例验证诊断码、source span；检查库闭包不依赖旧 compiler 专用 builder。
- `scripts/check_lainir_comptime_pipeline.py`：记录求值前后的可验证 LAINIR；覆盖整数计算、
  AST 复制/修改与 origin/hygiene、库内部语义数据的物理表示、含 let/if/普通调用的计算、Trap；
  证明计算确实经过 `#eval`，结果被消费且 backend 输入无 `#eval`。

每个新检查都要有能暴露绕行或错误实现的负对照。成功 artifact 必须经过 verifier，并实际
运行到预期结果；失败不得留下可继续发布的半成品。

保留并执行受改动影响的已有检查与构建：

```text
python scripts/build_lain_compiler.py
python scripts/check_bootstrap_consteval.py
python scripts/check_meta_pipeline_audit.py
python scripts/check_meta_stage_swap.py
python scripts/check_bootstrap_vm_api.py
python scripts/check_meta_form_swap.py
python scripts/check_lainir_boundaries.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
python scripts/check_lainc_lainir_api_baseline.py
```

最终完成条件：替换库 Meta 可改变 effect/module/type 等语言规则而无需修改 compiler core；
core 与 VM 没有语言专用构造或返回类别分派；编译期计算通过 LAINIR `#eval`；求值后 IR 可
独立验证、执行并进入 backend。该证据来自 bootstrap 链，不宣称编码 5 或编译器固定点完成。

## 8. 编码 2 剩余：正式签名语义与形状一致性

### 8.1 未覆盖项

1. 让正式库 elaborate 真正读取显式参数、输入行、返回类型、输出行、body 与声明环境，
   建立并检查完整函数语义；不能仅靠 `FunctionType.input_row` 字段或编译通过证明行为。
2. 将参数/返回类型解析及有效性检查归入库语义阶段，消除对 compiler core unit 类型表和
   descriptor builder 的语言规则依赖。迁移在编码 1e/1h 内安排。
3. 补齐 `check_function_shape_conformance.py` 剩余正式侧 SKIP：非法返回类型与畸形
   effect 行的诊断优先级，需要完整库类型环境。
4. 保持已约定的诊断优先级及 span；全量形状校验搬移不能抢先覆盖返回类型诊断。

Parser 继续使用普通 Atom/Group；箭头是相邻 `-` 与 `>`，不得添加函数/行/类型工厂节点。

### 8.4 剩余验收

- 剩余 SKIP 转为 bootstrap/正式库一致性断言，并补齐诊断优先级与 span 反例。
- 经完整库阶段读取并消费签名，不是只写字段；编码 5 接通后由正式编译器实际编译正反例。
- 以下现有 gate 继续回归，构建成功只证明表示与闭包，不代替正式语义执行。

```text
python scripts/check_meta_ast_conformance.py
python scripts/check_function_signature.py
python scripts/check_function_shape_conformance.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
```

## 9. 编码 3 剩余：完整库策略输入与类型约束

### 9.1 剩余工作

在库侧定义有明确契约的 `Ord(T)` 与所需成员，打通输入值的成员解析和调用，建立完整正例：

```lain
let max = std::func(a: T, b: T)
    ?{T, ord: Ord(T)}
    -> T {
    return ord.max(a, b);
};
```

上述正例需实际库符号、复合类型与策略值成员调用，并由策略改变运行结果。

在编码 1 的语义迁移中检查现有类型位置推导与位宽匹配是否满足完整库类型规则，补齐约束
冲突、复合类型不匹配和未知宽度的检查边界。不得把“出现于类型位置”或“kind/位宽相同”
当成全部类型约束求解的证明。

### 9.2 保持的语义约束

输入按精确名称在调用者词法环境从内向外查找，最近绑定遮蔽外层绑定；不按类型扫描环境。
所有约束一致才推导成功；无约束或冲突应诊断，不能凭名称大写猜类型。
约束来自显式参数、返回类型、同行类型表达式及 body 已确证的类型约束。

输入在调用前提供，输出 operation 在执行期间发出；输入不自动进入输出行，输出 handler
不自动产生同名输入。调用者缺少所需输入时显式传播或显式建立环境，不自动捕获。

### 9.4 剩余验收

- 完整策略正例编译并运行，覆盖整数类型及至少一次策略值改变结果的因果对照。
- 缺少输入、无法推导、约束冲突、复合类型不匹配、策略成员错误分别检查码与 span。
- 正式编译器的相同用例在编码 5 接通后执行；原有输入行及签名 gate 继续回归。

```text
python scripts/check_input_effects.py
python scripts/check_function_signature.py
```

## 11. 编码 5：接通正式 lainc 的 LAINIR 编译期求值组合

### 11.0 前置决定：provider 与执行能力由谁提供

正式 Lain provider/VM 的归档实现不在构建闭包。先选择并记录运行能力提供方式：

| 路径 | 工作 | 限定 |
| --- | --- | --- |
| A：使用 C seed | 用现有宿主物理能力实现契约适配，组合层提供 LAINIR 编译期求值 handler | 子执行、VSpace、预算、Trap 由宿主承担，必须做真实契约测试 |
| B：重写 Lain provider/VM | 按物理契约实现并实际执行，再组合正式 lainc | 覆盖完整调用路径，不以能编译或通过 verifier 证明可运行 |

选择前不实施本节运行组合。两条路径均不得把 effect 等语言语义加入 seed/VM。

### 11.1 契约归属与现有接口适配

- 将 `Eval` 从 `src/lainvm/api_contract.lain` 的 ExecutionShape 迁到 LAINIR 求值契约，
  在编译组合边界安装 handler；LAINVM 只保留物理执行原语。
- 审计并适配 `src/lainc/meta.lain` 中现有 `perform Vm.eval(...)`，使用编码 1 形成的
  显式 `#eval` 链；不能把直接 procedure 执行 API 当成另一套编译期语义。
- 审计 `bootstrap.eval_source` / `bootstrap.eval-next` 的传源码、fold、侧信道接口及全部
  消费者；统一到 LAINIR `#eval` 语义，移除无用途的旧接口或明确物理适配职责。
- 处理两个 `api_contract.lain` 的 import 末段匹配歧义；按 §15 先提供最小复现与方案，
  不假定路径前缀能消除冲突。

### 11.2 实现与验收

1. 组合 Memory、IR provider、执行能力、compiler driver 与所需通用 handler。
2. 使用临时执行流、调用者 VSpace 及明确生命周期；成功恢复物理结果，Trap 转编译诊断。
3. core 只依赖契约，具体实现由组合层选择；重建正式闭包并检查契约一致性。
4. 新增 `scripts/check_formal_lainc_eval.py`（待实现）：运行生成的正式编译器，编译确实
   需要 AST 处理与编译期计算的程序；记录 `#eval` 求值及结果消费，程序运行结果正确。
5. 实际运行含完整签名、输入行和库 effect 的正反例，完成编码 2/3 的正式行为验收。

现有 `check_lainvm_boundary.py` 只证明源码契约边界，不能替代 handler 被实际执行的证据。
provider 记录、函数名字或构建通过也不能作为运行证明。

现有回归命令：

```text
python scripts/check_lainvm_boundary.py
python scripts/check_bootstrap_consteval.py
```

新增入口实现后必须注册 baseline 并实际执行，明确区分计划项和现有命令。

## 12. 编码 6：建立真正的 Lain 编译器固定点

依赖编码 5 的可运行组合与已验证编译入口。

### 12.1 先定义可执行入口

`src/lainc/lainc.lain` 当前只导出 `lainc.API` factory，不能被 seed 当作命令行编译器入口。
在开始代际构建前，先建立并测试正式组合入口。入口名称、参数和宿主 capability ABI 必须
写入 `src/lainc` 的当前文档和独立 fixture；不得沿用脚本中不存在的
`compiler_compile` 假设。

先查清既有 ABI；建议入口名 `lainc_compile` 尚未确认，名称、参数和能力须有共同契约。
当前 `composed_compiler_sources()` 的 provider/VM manifest 只列契约，未选择运行实现；
须按编码 5 的组合补齐运行闭包，不把库构建清单当成可执行编译器闭包。

### 12.2 修复固定点驱动

新增或重写专用于 Lain 编译器的脚本，例如 `scripts/run_lainc_self_host.py`（待实现）：

1. 依据编码 5 的所选组合建立共同 source manifest，修订闭包驱动后用 bootstrap 得到 gen1；
2. 用 gen1 和相同 source manifest 得到 gen2；
3. 用 gen2 和相同 source manifest 得到 gen3；
4. 每一代先用 verifier 检查约定入口；
5. 用 `scripts/prove_lainc_fixed_point.py` 比较 gen2 与 gen3；
6. 失败报告列出 extern、procedure label、signature 和 body hash 的首个差异；
7. 产物和报告全部写到 `build/selfhost/`。

不要使用 `scripts/run_lainir_self_host.py` 证明这一阶段。修复
`scripts/profile_lainc_bootstrap.py` 的 source manifest、默认 artifact 和入口后，它只能作为
性能报告，不作为唯一正确性证明。

### 12.3 固定点验收

- gen1、gen2、gen3 均能编译普通程序、`std::type` fixture 和输入 effect fixture；
- gen2 与 gen3 规范化一致；
- 三代产物都不含旧 generic ABI、`EvalResult` 或残留 `#eval`；
- 从干净 checkout 运行固定点脚本不会在 `src/`、`std/` 或 `bootstrap/` 写产物。

## 13. 编码 7：剩余 backend、native 与发布验收

### 13.1 native 编译器的求值与宿主 ABI

重建当前 bootstrap/backend 产物，区分真实 `#eval` 指令与字符串中的 `#eval`。
backend 输入必须经过求值阶段；残留真实 `#eval` 属编码 1/5/6 的执行链问题，不由后端透传。

审计 manifest 中全部外部能力与 native host 的实现/声明；已有报告指出缺少
`bootstrap.vm-*` 及 artifact capture 适配，确切符号以重建后的产物为准。
只补 prologue 声明会将错误转为未定义链接符号，必须实现完整 ABI 并执行 host 契约测试。

### 13.2 剩余物理构造与语义覆盖

- 浮点运算及跨调用表示/ABI尚需实现和执行验证；支持 float 类型拼写不证明浮点运算正确。
- `#bitcast`、`#proc_addr` 的 C lowering 尚需实现；按规范逐项审计，不以编译器闭包未使用为由省略。
- `#call_indirect` 两个后端覆盖不同；不能用只比较共同子集的差分 gate 证明正确。
  为未共有构造增加独立运行 oracle 或与物理解释器对照，并核验完整物理调用签名。
- 扩大有符号/无符号、窄位宽与转换的边界值覆盖；未知构造必须显式失败且不发布产物，
  不用合法 C 注释或逗号表达式伪装支持。

### 13.3 `#alloca` 的物理语义与生命周期

当前两个 C 后端的分配方式不同：seed 使用零初始化复合字面量，Lain 后端使用 malloc。
需要解决：

1. 先确认规范是否要求零初始化或如何处理未写内存；给出成本和方案，不能用读未初始化
   内存的 fixture 强制引入新语义。
2. 满足规范的 activation 返回时释放全部 alloca，修复 malloc 路径的释放与泄漏问题。
3. 明确 activation 地址不得逃逸的验证/执行边界，覆盖嵌套调用、返回地址与错误路径。

生命周期要求已有规范，不把“是否释放”列为新的语言决定。实现前确认具体物理机制和检查
策略；零初始化尚未规定的部分先讨论。见 [`../01-lain-ir.md`](../01-lain-ir.md) 的内存和地址计算一节。

### 13.5 发布验收

- native compiler matrix：完整源码闭包与程序正反例在生成的 native 编译器上运行。
- source span、Trap source mapping 的执行验证。
- determinism、snapshot、release packaging，从干净 checkout 重建，产物只落 `build/`。
- 真实 CI runner 执行 bootstrap、stdlib、Lain 编译器 fixed-point 与 native smoke。
- 更新 baseline，新增真实 gate，移除失效架构形状断言；明确区分 LAINIR 自举与 Lain 固定点。

物理后端/ABI工作可独立推进；最终 matrix、CI 与发布验收依赖编码 6。

现有回归入口（不代表本节剩余项目已经实现）：

```text
python scripts/check_backend_c_shape.py
python scripts/check_backend_differential.py
python scripts/check_native_backend_migration.py
python scripts/check_native_backend_canonical_diff.py
python scripts/build_default_lainc.py
python scripts/check_lainc_lainir_api_baseline.py
```

差分测试比较两个后端的实际程序结果，仍需独立预期值防止两边同错。对齐问题使用能暴露
错误的构建模式；检查只在已有产物上运行时，先确认其由当前源码重建。

## 14. 每个提交的执行规则

1. 修改前运行 `git status --short`，不要覆盖其他人的未提交文件。
2. 只编辑当前切片列出的源码；发现需要跨层修改时，先说明依赖。
3. 先运行该切片的最小真实测试，再运行阶段验收；不要每次都运行耗时数分钟的全量 gate。
4. 文本搜索只检查禁用名称，不能证明行为正确。
5. 测试产物放入临时目录或 `build/`。
6. `git diff --check` 必须通过。
7. 每个切片保持独立可审查的改动与验收记录，不混入下一切片。
8. 切片完成后写入新的完成快照，再从当前路线图移除；保留历史归档，不覆盖旧快照。

## 15. 必须停止并讨论的情况

出现下列问题时，不得自行补充语言规则：

- `?{}` 是否需要运行时动态输入，而当前编译期 Meta 输入模型无法满足需求；
- 输入环境按名称解析与实际需要的类型导向搜索发生冲突；
- `std::type` 需要同时充当 Module；
- 一个 Meta 值无法通过现有物理 bits、addr 或 unit 传入 LAINVM；
- Trap 需要变成普通返回值才能继续实现；
- 固定点入口需要不同于已确认 ABI 的宿主能力；
- import 末段匹配导致同名契约绑定歧义，需要改变名字解析规则；
- 需要把函数、输入行、类型工厂或 effect 行加入 Parser 语义节点；
- 需要恢复旧裸 `type`、`comptime` 或 generic policy 才能通过测试。

发生这些情况时，应提供最小复现、涉及文件、当前行为和两个可选方案，再请求决定。
