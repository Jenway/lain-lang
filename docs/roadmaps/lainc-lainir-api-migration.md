# lainc 到 LAINIR API 迁移计划

基线日期：2026-09-06。

状态：执行中。Builder、Artifact、Eval 三层 v1 shape 已建立，`src/lainc`
已切到注入的 capability，旧动态 L1 模块已移出 compiler source closure。默认
provider 仍在补齐完整 verifier、canonical printer、evaluator 与 owner/limit 语义，
因此 API 目前属于内部迁移接口，尚未发布为稳定扩展点。

## 当前进度

| 阶段 | 状态 | 已有证据 | 尚缺内容 |
| --- | --- | --- | --- |
| 0 行为基线 | 部分完成 | `check_lainc_lainir_api_baseline.py` 汇总 source boundary、seed/recording provider 行为、默认 Provider(Memory) smoke、formal/bootstrap compiler fixture parity、三个 formal compiler canonical artifact 静态快照和 source-closure 双构建确定性；16 个成功 fixture 的 canonical IR/执行结果、resolved import 与 4101/4103 import failure 已覆盖，失败路径也固定为不产生 artifact；legacy 语法拒绝检查已建立；compiler API v3 已公开诊断字段与 message 访问器，source elaborator 的 unresolved-import 路径已传递 token span | 其余前端路径的 source offset 与位置快照 |
| 1 API v1 | 部分完成 | `api_contract.lain` 已定义 Builder/Artifact/Eval shape 与 schema v1；Eval 的 `Capabilities` 是显式参数，空 capability 为默认值；recording provider 已验证非法 data layout 失败且不留下部分状态，以及跨 owner handle 使用失败 | 正式 provider 的 owner/失败原子性、capability 的跨 provider 可执行 contract tests |
| 2 默认 provider | 进行中 | 独立 source manifest、组合构建入口、默认 provider 已存在；`l1_interpreter.lain` 已定义整数、data/activation memory、procedure address 与 indirect call；`check_lainir_provider_smoke.py` 已验证 `Provider(Memory)` 特化、artifact verifier 和最小执行路径 | 浮点执行（等待 literal/ABI 规范）、完整 printer、seed adapter 与差分报告 |
| 3 lowering | 已完成边界迁移 | `lower.lain` 只使用 provider handle；静态边界检查通过 | 真实程序输出差分 gate |
| 4 artifact | 已完成边界迁移 | compiler core 通过 Artifact API verify/print；compiler API v3 已公开诊断数量、错误码、source id、span 与 message 访问器；unresolved import 已由 elaborator 的 syntax node 传递 start/end | 其余前端 span 传递与位置 gate |
| 5 Meta eval | 进行中 | Meta 只通过 Eval 构造器/访问器交互，不读取 IR 或 Eval value/result 布局；`l1_interpreter.lain` 定义 step/call-depth/allocation 限制、结构化控制流、位宽整数与 64-bit typed activation memory，seed 行为 gate 覆盖已落地的 scalar/memory 语义；每次 Meta eval 显式传递空 capability，外部 capability 只由 LAINIR provider 的 dispatcher 工厂提供 | 默认 provider module 的实际 Eval 调用、非 scalar 对象 owner、nested-eval 限制 |
| 6 清理与固定点 | 进行中 | 五个 `src/lainc/l1_*` 与旧 `bootstrap_lainc.lain` 已从仓库删除，静态检查拒绝重新导入；formal stdlib 可重建；`build_srclainc.py` 会验证输出为可解析的 LAINIR artifact，clean output 已实际生成；`check_lainir_boundaries.py` 与 `check_srclainc_artifact.py` 均通过；`run_lainir_self_host.py` 已完成 gen1→gen2→gen3 native 编译并确认 gen2/gen3 C 输出一致，且已纳入迁移总 baseline | native 差分报告、ABI manifest 与最终 clean build |

## 目标

`src/lainc` 通过稳定的能力 API 生成和执行 LAINIR。它不导入 LAINIR 的存储模型、验证器、打印器或解释器实现。

迁移完成后的依赖方向为：

```text
src/lainc lowering
  -> LainIrBuilderApi
  -> LainIrArtifact

src/lainc Meta
  -> LainIrEvalApi
  -> EvalResult

src/lainir 或 seed
  -> 实现 BuilderApi、ArtifactApi 和 EvalApi
```

这个迁移是编译器自举主线的一部分。五个 `src/lainc/l1_*.lain` 旧动态 L1 模块和旧的 `bootstrap_lainc.lain` monolith 已经删除；静态检查保留旧模块路径清单，以阻止重新引入。当前 compiler source closure 只包含拆分后的 `src/lainc` 模块和显式注入的 provider。

## 边界决定

### lainc 拥有的内容

- 从 elaborated program 到物理操作的 lowering 决策；
- 源语言类型到 LAINIR 物理类型的映射；
- record layout 展开后的 offset、alignment 和具体 `#lea` 参数；
- procedure、控制流、调用和 `#eval` 单元的生成顺序；
- 源码 span 与编译器诊断的关联。

### LAINIR 实现拥有的内容

- unit、procedure、region、value 和 instruction 的内部表示；
- handle 的分配与有效性；
- canonical LAINIR 验证；
- artifact 的序列化和规范文本输出；
- `#eval` 的资源限制、执行和物理结果；
- parser、verifier、interpreter 和 native backend 之间的语义一致性。

### 不跨越边界的数据

API 不暴露内部 vector、节点结构、裸 enum 数字或可变 `Unit` 字段。`lainc` 不读取 expression 表、procedure 表或 region 表来恢复语义，也不修改已经生成的节点。

跨越边界的对象只包括不透明 handle、规范物理类型、显式操作参数、诊断、artifact 和 eval result。

## API 分层

API 分为三个窄接口，避免 builder、输出格式和执行环境互相绑定。

### 1. `LainIrBuilderApi`

负责构造一个 LAINIR unit。第一版至少覆盖当前 lowering 和近期自举需要的操作：

- 生命周期：`new_unit`、`finish`、`discard`；失败结果需要时可携带不可验证、不可发布的 `empty_artifact` marker；
- 物理类型：`bits_type`、`float_type`、`addr_type`、`unit_type`、`never_type`；
- module item：`add_data`、`declare_extern`、`begin_procedure`、`end_procedure`；
- region：`begin_region`；
- value：parameter、literal、procedure/data address；
- 明确整数和浮点操作以及宽度转换；
- `call`、`call_indirect`；
- `alloca`、`lea`、typed `load/store`；
- `if`、`loop`、`break`、`continue`、`return`；
- 每个可诊断构造操作接收 source location 或可选 span handle。

`finish` 是提交点。成功后返回不透明 `LainIrArtifact`；失败时返回结构化诊断。完成后的 unit 不再允许修改。

Builder 方法使用有语义的操作名，例如 `signed_divide` 和 `unsigned_divide`。API 中不出现与已删除兼容语法对应的模糊操作，也不提供 `field` 或宽泛的 `primitive`。

### 2. `LainIrArtifactApi`

负责处理已完成的 artifact：

- `verify`：执行规范验证并返回结构化诊断；
- `write_canonical_text`：按需输出规范 LAINIR 文本；
- `schema_version`：报告 API/artifact schema；
- 可选的 hash/compare 操作用于 gen2/gen3 固定点。

正常编译结果是 artifact。文本只是 CLI、调试和测试可请求的表示，不作为 `lainc` 内部可变 IR。

### 3. `LainIrEvalApi`

负责执行编译期 LAINIR：

- 输入：已验证 artifact、procedure handle、物理参数和资源限制；
- 输出：共享 `EvalResult`，包含 result kind、物理值或对象 owner，以及结构化诊断；
- capability：外部调用必须由调用方显式提供，默认环境没有隐式宿主能力；
- 保证：使用和运行时 LAINIR 相同的验证与执行语义。

Meta 只持有 `LainIrEvalApi` 和必要的 builder 能力。它不接收 `AccessShape`，也不遍历 LAINIR 内部表。

## 最小数据模型

v1 先固定语义角色，不固定 provider 的结构体布局。Lain 侧可用 `ModuleShape` 表达，seed 侧可用 ABI table 表达，但二者必须满足同一组 contract tests。

| 名称 | 含义 | 生命周期 |
| --- | --- | --- |
| `BuilderContext` | 一次尚未提交的构建会话 | 从 `new_unit` 到 `finish` 或 `discard` |
| `TypeHandle` | provider 内的物理类型引用 | 不超过所属 `BuilderContext`/artifact |
| `ValueHandle` | SSA 值、常量或物理地址引用 | 不超过所属 `BuilderContext`/artifact |
| `ProcedureHandle` | procedure 声明或定义引用 | 不超过所属 `BuilderContext`/artifact |
| `RegionHandle` | 结构化控制流 region 引用 | 仅在所属构建会话内有效 |
| `LainIrArtifact` | 已完成且不可变的 LAINIR 单元 | 由调用方持有，显式释放或随 owner 释放 |
| `EvalValue` | 带明确物理类型的求值参数或结果 | scalar 直接拥有；对象值携带 owner |
| `Diagnostic` | 阶段、错误码、source location 与上下文 | 独立于失败的 builder 临时状态 |

所有 handle 都带隐含 owner。跨 unit 混用 handle 必须稳定失败，不能依赖数组下标碰巧有效。`finish` 成功后消费 `BuilderContext`；失败时保留诊断并释放未发布资源。artifact 只有通过 `verify` 后才能交给 evaluator 或 backend。

## v1 操作约定

下面的伪签名用于固定调用关系，实际 Lain effect 集由 provider 使用的 memory model 补全。

```text
Builder.new_unit(options) -> BuilderContext
Builder.begin_procedure(ctx, name, signature, location) -> ProcedureHandle
Builder.begin_region(ctx, procedure, location) -> RegionHandle
Builder.add(ctx, type, lhs, rhs, location) -> ValueHandle
Builder.alloca(ctx, size, alignment, location) -> ValueHandle
Builder.lea(ctx, base, index, scale, offset, location) -> ValueHandle
Builder.load(ctx, type, address, location) -> ValueHandle
Builder.store(ctx, region, type, value, address, location) -> unit
Builder.finish(ctx, entry) -> Result<LainIrArtifact, Diagnostic>

Artifact.verify(artifact) -> Result<VerifiedArtifact, Diagnostic>
Artifact.write_canonical_text(verified) -> Bytes
Artifact.canonical_hash(verified) -> Hash

Eval.evaluate(verified, procedure, arguments, limits, capabilities)
  -> Result<EvalValue, Diagnostic>
```

`Builder` 接收已经确定的物理类型、offset 和操作。它不查询 Lain 类型，也不替 `lainc` 推导字段布局。`Eval.evaluate` 的 capabilities 是显式输入；文件、进程、网络和宿主符号都不会因执行发生在编译期而自动可用。`empty_capabilities()` 是 compiler Meta 路径唯一可构造的默认值。provider 可以在构造时接收外部 dispatcher；只有调用方同时传入该 provider 所属的 `external_call_capabilities()`，外部 procedure 才会交给 dispatcher。capability 不携带 LAINIR 存储地址或 host 回调布局。

`Limits.nested_evals` 为共享 Eval session 保留配额。当前 structured L1 model
没有 `#eval` expression kind，单次 `Eval.evaluate` 不会在 evaluator 内递归启动另一轮
eval；因此 provider 不得将这个字段表述为已经执行的限制。实现嵌套 eval 前，必须先引入
session/context 生命周期，再由 `#eval` lowering 在每次嵌套调用时传递它。

## API 合约要求

第一版 API 必须写成独立于具体实现的约定，并由至少两个 provider 验证：正式 seed/LAINIR provider 和测试 provider。

合约需要固定：

- handle 的有效期、owner 和不可复制条件；
- `finish`、失败和取消后的资源所有权；
- physical type 与操作的合法组合；
- procedure/region 的结构化构造顺序；
- source location 如何进入诊断；
- artifact 与 API schema version 的兼容规则；
- evaluator 的 step、depth、allocation 和 nested-eval 限制；
- 确定性要求：相同输入产生相同 canonical artifact；
- 错误原子性：失败的 builder 调用不得留下可观察的半个节点。

具体 API 可以是 Lain `ModuleShape`、seed ABI table 或二者的适配层。测试必须针对同一份行为合约，避免两套接口独立演化。

### 当前尚未迁移的 ABI

`src/lainc/compiler_context.lain` 的内部 `Diagnostic` 已有 `source_id`、`start`
和 `end` 字段，但现有 compiler core 对 tokenizer/parser/elaborator/lower 的失败路径尚未
传递具体 span；当前已覆盖 elaborator 的 unresolved import，但 tokenizer/parser、其余
elaborator、lower 和默认 Artifact provider 仍未产生可用的非零 location。公开的
`compiler_api.API` v3 已导出诊断计数、错误码、source id、start、end 与 message 的访问器，
但这些 accessor 只能暴露现有数据。因此现有 4101/4103 fixture 固定的是错误码和无
artifact 的原子性，不能被表述为位置诊断 contract；必须先补 source span，再加入位置
快照。

`src/lainir/lain/eval_result.l1` 已有 object kind、owner、transfer 和 release 的
bootstrap ABI，`check_eval_object_matrix.py` 覆盖的是这条 seed `#eval` 路径。它不属于
`src/lainir/api_contract.lain` 的 `EvalShape`：默认 `EvalApi.Value/Result` 目前只承载
物理 scalar value。把 type/module/AST object owner 纳入迁移，必须先将其定义为
provider-owned opaque API，并由默认 provider 和 test provider 共同执行；不得把旧 ABI
的通过结果计入这一完成条件。

默认 provider smoke test 已有独立 harness：
`scripts/check_lainir_provider_smoke.py` 用具体的
`memory_model.Model(arena_min.Policy, bounds.Unchecked)` 实例化
`lainir_default_provider.Provider`，编译完整 `std/` 与 provider source closure，
再交给 seed verifier 和 runner。固定 fixture
`scripts/fixtures/lainir_provider_smoke.lain` 返回 schema version `1`；这条 gate
同时证明 factory specialization、artifact 验证和最小执行路径都能工作。

该 probe 的调试过程也留下了实现层面的修正：`l1_unit_builder` 的
`i32_literal`/`parameter` 补齐了 `Memory.Bounds.Effect`，`as` 转换在 lowering 中
落成显式 `#sext/#trunc`，provider 的可变字段访问改为先取得局部引用，interpreter
的位运算改成现有明确整数指令，循环 lowering 会跳过显式终结符后的重复尾
`#continue`。这些修正使源码特化和 artifact verifier gate 都能重复通过；后续工作
集中在真实 compiler 调用面、Eval owner/session 和第二个 provider contract test。

尝试把 Builder -> `finish` -> `verify` -> `evaluate` 直接放入 smoke fixture 的
顶层初始化会得到 5109：这些调用携带 `Memory.Allocation/Bounds` effect，而顶层
初始化没有可安装 handler 的执行上下文。这个结果确认了 Eval smoke 不能靠额外的
顶层调用伪造完成；下一项应提供显式 handler 环境，在一次受控 procedure 中执行
完整 builder/artifact/eval 链，并同时检查成功、失败和资源限制路径。

native backend 仍是独立缺口：`scripts/run_lain_backend.py` 对固定 artifact 的
验证本身可以通过，但编译 `src/lainc/backend_c.lain` 会在旧的 `@foreign` 声明处
得到 1001（当前 active compiler 不接受该语法）。因此 gen1/gen2/gen3 self-host
只证明 compiler source closure 的固定点，不代表 Lain-written C backend 已完成
API 迁移；backend 的外部 capability 声明需要单独改为当前 contract 支持的形式。

### Eval owner/session 的迁移顺序

这部分按三个可独立验收的 contract 进入 API：

1. **Session**：provider 创建一个不透明 session，持有一次评估的 activation arena、
   resource counters 和 capability snapshot。`Eval.evaluate` 要么创建并消费自己的
   session，要么由显式的 `evaluate_in(session, ...)` 加入调用方 session；调用方不能
   从 scalar `Value` 反推出 session 地址。
2. **Object**：provider 定义不透明 `Object` 和 `ObjectOwner`，`Result` 通过 kind、
   object accessor 和 owner accessor 返回它们。成功、trap、limit 和取消都必须有
   明确的 release/transfer 规则；`Value` 与 `Object` 不能互相伪装成物理 address。
3. **Nested eval**：只有在 session 存在后，`Limits.nested_evals` 才由每次
   `evaluate_in` 消耗。配额耗尽必须返回稳定诊断，并释放当前 nested session；没有
   `#eval` expression kind 的 provider 不得通过调用深度或普通 procedure call 冒充
   nested-eval 计数。

阶段 5 的实现顺序是 Session contract tests、默认 provider 的 session 生命周期、
Object/Owner contract tests、默认 provider 的 object adapter，最后才是 nested-eval
lowering。旧 `eval_result.l1` 可以继续作为 bootstrap compatibility fixture，但不能
直接成为这些 shape 的布局。

## 迁移阶段

阶段之间的依赖为：

```text
阶段 0 行为基线 ──> 阶段 1 API v1 ──> 阶段 2 provider
                                           ├──> 阶段 3 lowering ──> 阶段 4 artifact
                                           └──> 阶段 5 Meta eval
                                                    │
                                                    v
                                           阶段 6 删除旧实现
                                                    │
                                                    v
                                              gen2/gen3 固定点
```

阶段 3、4 可按调用面逐步迁移。阶段 5 涉及对象 owner 和 capability，必须在 Eval contract tests 建立后开始。阶段 6 只能在新路径覆盖全部调用点并通过差分 gate 后执行。

### 阶段 0：建立行为基线

目的：记录切换前 `src/lainc` 已经支持的程序和诊断，防止迁移改变语言行为。

工作：

- 为 empty API、scalar return、函数调用和现有 Meta 调用保存 canonical 输出；
- 记录当前 verifier failure 和 Meta failure 的错误码与 source location；
- 列出 `l1_ir`、builder、verifier、printer、interpreter 的全部调用点；
- 禁止在迁移期间继续扩大 `src/lainc/l1_*` 的公共表面。

完成条件：有一个可重复运行的迁移 gate，覆盖当前成功和失败路径。

交付物：fixture 清单、旧调用点清单、canonical artifact/diagnostic 快照，以及单一的 `check_lainc_lainir_api_baseline.py` 入口。

### 阶段 1：冻结 API v1

目的：让 `lainc` 和 LAINIR 实现能够依照同一约定并行开发。

工作：

- 定义 Builder、Artifact 和 Eval 三个 shape；
- 定义不透明 handle、diagnostic、source location、artifact 和 eval result；
- 为每个 API 操作写前置条件、结果和所有权规则；
- 建立 schema version 和新增操作的演化规则；
- 用 contract tests 固定 canonical 指令、拒绝条件和资源生命周期。

完成条件：API 文档和 contract tests 可以在不导入 `src/lainc/l1_ir.lain` 的 provider 上实现。

交付物：版本化 contract、reference test provider、跨 owner/失败原子性/确定性测试。

### 阶段 2：增加现有 LAINIR provider

目的：用当前 seed/LAINIR 实现兑现 API，不先改动编译器语义。

工作：

- 在 `src/lainir`/seed 边界实现 BuilderApi；
- 将现有 verifier 和 canonical printer 接到 ArtifactApi；
- 将现有 `#eval` 执行入口接到 EvalApi；
- 建立一个临时适配器，使旧 lowering 结果可以与新 provider 做差分比较；
- 保证 provider 不引入源语言类型、module、generic、effect 或 AST 概念。

浮点物理类型和操作名称已经进入 Builder API，但 float literal 文本与调用 ABI 尚未
进入 LAINIR 规范。因此默认 Lain evaluator 在该规范冻结前不实现浮点算术；完成时
必须以 seed interpreter 的 IEEE 语义做 differential test，不能在 provider 中引入
host 类型或 ad-hoc 浮点表示。

完成条件：contract tests 全部通过；同一 fixture 经旧路径和 provider 得到相同 canonical LAINIR、诊断和执行结果。

交付物：默认 provider、seed adapter、provider source manifest 和差分报告。

### 阶段 3：迁移 lowering

目的：`src/lainc/lower.lain` 只通过 BuilderApi 生成 LAINIR。

工作：

- 将 `Lower(Memory)` 改为显式接收 BuilderApi；
- 用 API physical type handle 替换 `L1.TypeId`；
- 用 API value/procedure/region handle 替换 `ExprId` 等具体 ID；
- 删除对 `unit.expressions` 等内部表的读取和回写；
- 删除通过首个 literal 修补返回值的 bootstrap bridge，改由 frontend 提供真实 semantic value；
- 让 `Lower.Result` 返回 artifact、entry handle 和诊断。

完成条件：`lower.lain` 不导入 `l1_ir` 或 `l1_unit_builder`，也不访问任何 LAINIR 内部字段；迁移 gate 输出不变。

交付物：只依赖 BuilderApi 的 lowering，以及阻止具体 LAINIR import 和内部字段访问的静态边界检查。

### 阶段 4：迁移编译结果处理

目的：compiler core 使用 ArtifactApi 完成验证和输出。

工作：

- 将 `Compiler`/`compiler_api.API` 工厂扩展为接收 LAINIR capabilities；
- 用 `ArtifactApi.verify` 替代 `l1_verifier.Verifier`；
- 用 `ArtifactApi.write_canonical_text` 替代 `l1_printer.Printer`；
- 明确 `emit_text_l1` 只控制外部表示，不改变是否生成 artifact；
- 将 LAINIR 诊断映射到 compiler diagnostic，并保留位置。

完成条件：`compiler_core.lain` 不导入 `l1_ir`、`l1_verifier` 或 `l1_printer`；公开 compiler API gate 全部通过。

交付物：artifact 驱动的 compiler result、诊断映射测试、CLI 文本输出适配层。

### 阶段 5：迁移 Meta 执行

目的：Meta 通过正式 EvalApi 执行编译期 procedure。

工作：

- 将 Meta procedure lowering 接到 BuilderApi；
- 用 ArtifactApi 验证完成的编译期 artifact；
- 用 EvalApi 替代 `l1_interpreter.Interpreter`；
- 删除 `AccessShape`、`DirectUnitFacts` 和对 unit 内部表的遍历；
- 统一 scalar、type、module、AST 结果的 owner 转交；
- 对外部 capability、资源超限、嵌套 eval 和 trap 建立稳定诊断。

完成条件：`meta.lain` 不导入 `l1_ir` 或 `l1_interpreter`；所有编译期执行都能在正式 LAINIR evaluator 中观察到并受同一组限制。

交付物：EvalApi adapter、owner 转交测试、资源限制与 capability 拒绝测试。

### 阶段 6：删除旧动态 L1 实现

目的：完成依赖反转并收紧仓库边界。

工作：

- 保持五个旧模块不再出现于 `COMPILER_SOURCES.txt`，并由静态检查阻止重新引入；
- 保持 `src/lainc/l1_ir.lain`、`l1_unit_builder.lain`、`l1_verifier.lain`、`l1_printer.lain` 和 `l1_interpreter.lain` 不存在；
- 搜索 active compiler source closure，确认旧 L1 kind 数字、兼容注释和 bootstrap 特例均未重新引入；
- 重新生成 bootstrap/compiler artifact，并运行固定点比较。

完成条件：`src/lainc` 中没有第二套 LAINIR model、verifier、printer 或 interpreter；clean build、真实程序 gate、formal stdlib gate 和 gen2/gen3 gate 全部通过。

交付物：删除提交、更新后的 source manifest、重生成 artifact 与固定点证据。

阶段 2 应建立统一组合构建入口：

```text
python scripts/build_srclainc.py
```

`python scripts/check_srclainc_artifact.py` 从相同源闭包构建两次，并比较
canonical artifact、extern 集、procedure header 与每个 body hash。它是当前
source-closure 的确定性 gate；它不替代 compiler 自举后的 gen2/gen3 gate。

该入口显式组合 formal stdlib、`src/lainc/COMPILER_SOURCES.txt` 与所选 LAINIR provider source closure；compiler 与 provider 两份 manifest 的所有权保持分离。

## 与编译器自举主线的关系

阶段 0 和阶段 1 可以与 formal stdlib 的 `3003` 调查并行。阶段 2 完成后，LAINIR 维护者可以独立维护 provider。

阶段 3 至阶段 5 必须在建立 `src/lainc` 固定点之前完成，因为 gen2/gen3 应验证最终依赖边界。若 `3003` 被确认发生在旧动态 L1 或 Meta interpreter 路径中，相应迁移阶段提前成为 formal stdlib 修复的一部分；在得到调用证据前，不把二者假定为同一故障。

先前 formal stdlib 的 `3003` 阻塞已经解除；`python
scripts/build_formal_stdlib.py` 当前可以生成 formal stdlib artifact。后续工作直接以
formal stdlib、真实 compiler artifact 和固定点作为 gate。API 迁移不得用兼容类型、
模糊整数操作或 host 端类型特例绕过失败。

## 执行批次

| 批次 | 内容 | 可并行工作 | 合并门槛 |
| --- | --- | --- | --- |
| A | 行为快照、调用点盘点、API v1 文档 | `3003` 根因调查 | 基线 gate 可重复，contract review 完成 |
| B | reference provider 与 seed/default provider | lowering 调用面分类 | 两个 provider 通过相同 contract tests |
| C | lowering 与 ArtifactApi 迁移 | Meta owner/limit fixture | canonical 输出和诊断差分为零 |
| D | Meta EvalApi 迁移 | backend 差分扩充 | scalar/type/module/AST 与限制测试通过 |
| E | 删除 `src/lainc/l1_*` 和 bootstrap 特例 | 文档清理 | clean build、formal stdlib、真实程序 gate 通过 |
| F | gen2/gen3 与 native 切换 | 无 | canonical hash、ABI manifest 和回归一致 |

每个批次单独提交。批次 B 之后允许 LAINIR 维护者和 `lainc` 维护者按冻结接口并行；接口变化必须先更新 contract tests，再同步两侧实现。

## 风险与回退点

- **API 泄露实现布局**：以第二个 test provider 验证；若无法实现，回到阶段 1 缩窄 shape。
- **旧路径与新路径同时演化**：阶段 0 后冻结旧 `l1_*` 功能，只接受阻塞迁移的修复。
- **Meta 对 evaluator 内部表的隐式依赖**：先把每个读取点变成可观察 fixture，再设计必要的 Eval result；不把内部表加入 API。
- **owner 转交产生悬挂对象**：所有非 scalar 结果必须携带 owner，并有 success、trap、limit、nested eval 四类释放测试。
- **canonical 文本掩盖 artifact 差异**：固定点同时比较 verifier、canonical hash、ABI manifest 和 procedure body hash。
- **删除旧实现后冷启动断裂**：阶段 6 前保留可重建的 seed/bootstrap snapshot；删除后立即运行 clean rebuild。

## 并行开发规则

- LAINIR 维护者修改 provider 和 contract tests，不修改 source-language lowering 规则；
- lainc 维护者只通过已冻结 API 请求新的物理能力；
- API 行为变化需要版本变更、contract test 和两侧迁移说明；
- 新指令先进入 `docs/01-lain-ir.md` 的规范，再进入 API；
- provider 内部重构只要通过 contract tests，不要求修改 `src/lainc`；
- 迁移期间旧路径仅用于差分验证，不接收新功能。

## 非目标

- 不在 API 中表达 AST、module、generic、effect 或源语言 type；
- 不让 `lainc` 通过拼接文本绕过 typed builder 和 verifier；
- 不为旧 L1 kind 数字或旧文本语法建立长期兼容层；
- 不在 `src/lainc` 中保留备用 verifier、printer 或 evaluator；
- 不要求第一版 API 覆盖尚未进入 LAINIR 规范的 SIMD 等长期能力。
- 浮点常量文本及其 bit-pattern 约定在 `docs/01-lain-ir.md` 定义前不进入
  Builder API v1；现阶段只冻结浮点物理类型和已有浮点运算。

## 最终验收

迁移整体完成需要同时满足：

1. 依赖检查：`src/lainc` 不包含或导入 LAINIR 具体实现；
2. 合约检查：正式 provider 和测试 provider 通过同一套 API contract tests；
3. 语义检查：parser、verifier、evaluator 和 native backend 的差分测试通过；
4. 编译检查：empty、nonempty、多文件和真实程序 compiler API gate 通过；
5. Meta 检查：编译期值、对象 owner、错误和资源限制 gate 通过；
6. 自举检查：formal stdlib 可重建，gen2/gen3 canonical artifact 一致；
7. 清理检查：五个 `src/lainc/l1_*` 模块及其 kind-number 特例全部删除。

## 当前实施记录（2026-09-07）

Provider(Memory) 的第一轮真实特化已经越过 API 源码编译阶段。独立的
`l1_unit_builder.Builder(Memory)` probe 先确认了 `i32_literal`、`parameter` 的
`Memory.Bounds.Effect` 要求；随后修正宽度转换、builder 的可变字段访问、verifier
的参数类型读取、printer 的偏移输出，以及 `as` 在 lowering 中的物理转换。完整的
formal stdlib + API + `default_provider` source closure 现在可以生成 provider
artifact，说明 provider 不再依赖一组未声明的 compiler 内部特例。

下一道门曾经是生成 artifact 的 seed verifier：Lain 源码里的显式 `break`/`continue`
会被 lowering 同时保留为结构化终结符和循环尾部的隐式 `continue`，形成诊断
2010。现在 `program_write_while` 会识别循环体尾部的显式终结符，避免重复追加；
provider 中的链式可变字段访问、unit 调用包装和 `as` 物理转换也已同步修正。
固定 smoke fixture `scripts/fixtures/lainir_provider_smoke.lain` 经过完整
Provider(Memory) source closure 生成的 artifact，已通过 seed verifier 并运行
成功，返回 schema version `1`。`scripts/check_lainir_provider_smoke.py` 已把这条
检查固化为自动化 gate。Provider 的实现边界因此具备可重复的源码特化和 artifact
执行证据；下一步继续迁移真实 `lainc` 编译调用面。

当前真实调用面的验证还暴露出 native backend 的独立缺口。`scripts/run_lain_backend.py`
可以验证输入 artifact，但随后用 active compiler（以及同一份已构建的 compiler artifact）
编译 `src/lainc/backend_c.lain` 时，在首个 `@foreign` 声明处失败，诊断为
`1001 unexpected character near '@'`。因此 gen1/gen2/gen3 的固定点只覆盖 compiler
frontend 和 LAINIR provider，不足以证明 Lain-written native backend 已完成迁移。
冻结 seed compiler 复测得到相同结果，说明问题属于 backend 源码仍依赖未纳入当前
source-language/API 合约的外部能力声明，不是 backend 脚本选错 compiler。

后续工作单独列为 native backend migration：先定义 backend 所需的 host capability
声明 ABI，再把 `@foreign` 声明迁移到该 ABI或将 backend 暂时移出 active compiler
source closure；完成后增加 backend compile、artifact verify、native emission 和
in-process execution 四段 gate。在此之前，`docs/implementation/lain-written-backend.md`
中的 executable backend 描述仅代表历史实现目标，不能作为当前迁移完成证据。
ABI v1 的第一版草案见
[`docs/implementation/lain-backend-capability-abi.md`](../implementation/lain-backend-capability-abi.md)。
它把 host link name 留在 provider/driver，把 backend 源码看到的边界收敛为
`backend.source_*`、`backend.allocate`、`backend.copy_bytes` 和
`backend.artifact_*` 八个逻辑 capability；该草案须在 gate 1 的实际 lowering
结果上验证后才能冻结。
源码 legacy inventory 已由 `scripts/check_lain_backend_abi.py --report` 固化；当前
报告七个已知 `@foreign` 声明并按 ABI v1 给出逻辑 capability 映射，迁移完成后该
命令将作为零 legacy 的第一道 gate。
`src/lainir/api_contract.lain` 现在同时公开 `BackendShape`；它只冻结逻辑函数形状，
不要求默认 provider 在 backend migration 完成前提供实现。

阶段 5 的一次真实尝试也已经给出明确缺口：在 Provider(Memory) smoke 中构造一个
只返回整数 literal 的 unit，再通过 `Eval.evaluate` 执行，artifact verifier 可以通过，
但 evaluator 运行时报告 `#alloca address escaped procedure activation`。该路径已从
现有 smoke 撤回，避免把失败实验混入绿色 baseline；它说明 Eval 的结果/临时对象
owner 转交仍未闭合，下一步应先增加独立的 activation-lifetime fixture，再修改
`l1_interpreter` 的返回值封装和释放规则。

`l1_interpreter` 同时修正了一个独立误判：`address_at` 现在先检查值的物理类型为
`addr`，不会再把普通整数 payload 当作 compact address handle。该修正尚未消除
上述真实 activation escape，后续 fixture 仍需定位返回值内部的临时地址来源。

进一步追踪确认，当前 smoke 走的是 seed C interpreter；它在
`seed/src/interpreter/interpreter.c` 的 procedure return 边界直接拒绝 callee frame
拥有的所有地址。Provider 的 `Value`、`Result`、`Limits` 等 record 都以物理 `addr`
承载，因此这里必须先定义“按值 record 返回”的复制 ABI，再决定如何保留真正裸地址
逃逸的诊断。一次临时 promotion 实验会导致 Eval fixture 不终止，已撤回；目前不把
seed interpreter 的行为改成未经验证的隐式复制。
