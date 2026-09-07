# Lain 项目路线图

基线日期：2026-09-07。

本文是当前唯一的路线图。它描述从正式标准库到编译器自举固定点，再到 Eval VM 和 LAINIR 物理层演进的依赖关系。历史计划和逐次实施记录保存在 [`../history/`](../history/)，不参与当前阶段判断。

## 1. 当前目标

Lain 的编译链必须形成以下稳定边界：

```text
Lain source
  -> RawAst
  -> standard-library Meta
  -> LainIrBuilderApi
  -> LainIrArtifact
  -> EvalSession (仅用于 #eval)
  -> runtime LAINIR
  -> interpreter / native backend
```

其中：

- `src/lainc` 只依赖 Builder、Artifact、Eval 三组能力 API，不读取 LAINIR 的内部表；
- Meta 负责 AST 操作、语言规则和 lowering；
- `#eval` 在独立的 EvalSession 中执行，不直接调用宿主解释器状态；
- LAINIR 只表达物理类型、明确整数操作、地址计算和 typed memory；
- 标准库 effect、ownership 和异步接口属于标准库，不进入 compiler core 或 LAINIR。

## 2. 当前事实与硬约束

已经确定的物理语义：

- `#data` 是只读静态数据；
- `#alloca` 只在当前 procedure activation 内有效；
- 字段访问展开为 `#lea` 加 typed `#load/#store`；
- 不保留 `#field`、`#primitive`、legacy 类型别名和模糊整数操作；
- Eval 有 step、call-depth、allocation quota 和显式 capability；
- provider、compiler 和测试实现必须共同遵守同一份 API contract。

当前代码证据已经覆盖 Builder/Artifact/Eval 的 v1 shape、默认 Provider(Memory) smoke、source-closure 构建、backend capability ABI、formal stdlib 构建和 gen1→gen2→gen3 的部分固定点。剩余工作集中在 Eval owner/activation 语义、真实 clean native build、历史 native 输出差分和完整诊断位置覆盖。

当前阶段状态：

| 阶段 | 状态 | 判断依据 |
| --- | --- | --- |
| A 正式标准库 | 已达到当前目标 | formal stdlib 可重建，旧 `3003` 阻塞已解除 |
| B API contract | 进行中 | v1 shape 和 recording provider 已有，正式 provider contract 仍需补齐 |
| C 默认 provider | 进行中 | Provider(Memory) smoke 和 backend ABI 已通过，完整 Eval/owner 语义未闭合 |
| D Eval VM | 未完成 | 单 TCB/单 VSpace 仍需实现并加入 contract tests |
| E lainc 迁移 | 边界已迁移，调用面进行中 | lowering/artifact 边界已切换，真实 Meta Eval 仍依赖 D |
| F 真实程序/native | 进行中 | fixture gate 已有，历史 C 差分和最终 clean build 未完成 |
| G 自举固定点 | 部分完成 | gen1→gen2→gen3 已有证据，最终 clean rebuild 仍需确认 |
| H 工具链清理 | 未开始 | 等 F/G 验收 |
| I VM/LAINIR 后续演进 | 计划中 | 不阻塞当前自举固定点 |

## 3. 依赖图与阶段

```text
A formal stdlib
      |
      v
B API contract + test provider
      |
      +--> C LAINIR provider
      |          |
      |          v
      |     D 单 TCB/单 VSpace Eval VM
      |          |
      v          v
E lainc lowering / Artifact / Meta migration
      |
      v
F 真实程序与 native backend
      |
      v
G gen2/gen3 fixed point
      |
      v
H 工具链切换与删除过渡实现
      |
      v
I 多 TCB VM 控制面与平台 lowering
```

A、B 可以并行推进。C 必须在 E 的真实调用面之前可运行。D 是 Meta Eval 的前置条件。E、F 完成后才能进行 G；I 不阻塞当前自举固定点。

## 4. 阶段 A：正式标准库成为默认语义实现（当前已达到）

目标：从当前源码重建 formal stdlib，并让它负责完整的 expand、elaborate 和 lower。构建阻塞已经解除，剩余内容是把替换证明纳入稳定 gate。

工作：

- 修复类型值经过 closure specialization 到 physical lowering 的传播；
- 完成 binding、type elaboration、函数调用、控制流和 module 生成；
- 统一 scalar、type、module、AST 四类编译期结果的 owner 转交；
- 为 function、struct、module、import、generic、effect、bounds 和 `#eval` 建立正负例；
- 比较 bootstrap 与 formal 实现的 canonical AST、诊断、依赖图、canonical LAINIR 和执行结果。

验收：删除旧 formal artifact 后，`python scripts/build_formal_stdlib.py` 仍成功；完整 `std/**/*.lain` 可由 bootstrap 编译；一致性矩阵通过；bootstrap 只保留为冷启动 snapshot。

## 5. 阶段 B：冻结 API contract

目标：让 lainc 和 LAINIR provider 能够独立演进。

API 分为三层：

- `LainIrBuilderApi`：物理类型、值、procedure、region、指令和终结符的结构化构造；
- `LainIrArtifactApi`：verify、canonical text、diagnostic、source location 和 artifact schema；
- `LainIrEvalApi`：session 创建、限制、capability、执行、trap、result 和 owner transfer。

必须固定：

- schema version、handle owner、有效期和失败原子性；
- procedure/region 的构造顺序；
- artifact 的 canonical 输出和确定性；
- source location 到诊断的传递；
- Eval 的 step、depth、allocation、nested-eval 和 capability 规则；
- scalar 返回值的复制 ABI，以及非 scalar provider-owned object 的释放 ABI。

验收：recording provider 和默认 provider 通过同一套 contract tests；失败的 builder 调用不留下可观察的半成品；相同输入产生相同 canonical artifact。

## 6. 阶段 C：默认 LAINIR provider

目标：由 seed/LAINIR 实现阶段 B 的 API，不让 compiler 重新依赖旧 L1 实现。

工作：

- 将现有 verifier、canonical printer 和 evaluator 接到 Artifact/Eval API；
- 维护独立 provider source manifest，不把 provider 私有模块加入 compiler source closure；
- 覆盖整数、只读 data、activation memory、procedure address、indirect call 和明确转换；
- 完成 backend capability ABI 的 host binding 和差分报告；
- 在 provider 中补齐 owner、失败原子性、capability 和浮点 ABI 的测试。

验收：Provider(Memory) smoke、seed verifier、最小执行路径和 backend ABI gate 全部通过；provider 可以在没有 `src/lainc/l1_*` 的情况下构建。

## 7. 阶段 D：单 TCB / 单 VSpace Eval VM

目标：让 `#eval` 在独立的轻量执行上下文中运行。这是当前自举主线的一部分，不是固定点之后才开始的远期设计。

### D1. Eval contract

一次 `#eval` 创建一个 `EvalSession`，session 拥有一个 `EvalVSpace` 和一个 `EvalTCB`：

```text
EvalApi
  -> EvalSession
     -> EvalTCB + EvalVSpace
        -> activation / memory / quota / capability
        -> typed LAINIR execution
        -> value or owned object result
```

`EvalTCB` 至少记录当前 procedure、instruction position、activation、fuel/depth、trap 和 result 状态。`EvalVSpace` 至少管理只读 `#data`、activation 内 `#alloca`、地址边界和 capability 集合。结果只能通过 result channel 离开 session，禁止 TCB 或 activation 地址逃逸。

### D2. 实现顺序

- 固定 session 的创建、运行、成功返回、trap、limit、取消和销毁；
- 固定 activation 的建立与回收，以及递归调用的隔离；
- 定义 scalar 按值返回的复制 ABI；
- 定义 type、module、AST 等非 scalar 结果的 opaque owner；
- nested eval 创建子 session，或由 contract 明确拒绝；
- 对越界、activation escape、超限、trap 和 capability 拒绝建立稳定诊断。

验收：seed provider 和 test provider 通过 session/lifetime/quota/capability/trap contract tests；现有 scalar fixture 在 Eval VM 内可重复执行；真实 Eval 不再依赖宿主 interpreter 的隐式地址复制。

## 8. 阶段 E：迁移 lainc

目标：`src/lainc` 只通过阶段 B 的能力 API 生成 artifact、验证输出并执行 Meta。

顺序：

1. `lower.lain` 改为显式接收 BuilderApi，删除对 `l1_ir`、unit 表和具体 ID 的读取；
2. compiler core 通过 ArtifactApi verify、print 和诊断映射；
3. Meta 通过 EvalApi 和阶段 D 的 EvalSession 执行编译期 procedure；
4. 删除 `src/lainc/l1_ir.lain`、builder、verifier、printer、interpreter 和旧 bootstrap monolith；
5. 保持旧路径只用于差分验证，不再接受新功能。

验收：source-boundary checker 拒绝旧模块重新进入 compiler closure；empty、nonempty、多文件、module、record、函数调用和控制流程序均通过 compiler API gate；Meta 不读取 interpreter 或 memory model 内部表。

## 9. 阶段 F：真实程序与 native backend

目标：把 API 迁移从 fixture 推进到真实 compiler 输出。

工作：

- 扩展 module factory、captured binding、qualified member 和 unresolved member 诊断；
- 比较 parser、verifier、evaluator、native backend 的 canonical artifact、ABI manifest、procedure header 和 body hash；
- 完成 backend capability 的历史 C 输出差分；
- 完成最终 clean native compiler build。

验收：正式 provider、native backend、真实程序 gate 和 clean build 同时通过；失败路径不生成部分 artifact。

## 10. 阶段 G：自举固定点

目标：由 `src/lainc` 生成的编译器接管自身构建。

工作：

- bootstrap 编译器生成 gen2；
- gen2 生成 gen3；
- 比较 canonical artifact、verifier 结果、ABI manifest 和每个 procedure body hash；
- 从干净目录只用 seed、bootstrap snapshot 和源码重建；
- 运行 formal stdlib、真实程序和 native 回归。

验收：gen2 与 gen3 一致，clean rebuild 成功，native `lainc` 可以编译真实程序。

## 11. 阶段 H：工具链切换与清理

- CLI、LSP 和测试 runner 使用同一正式标准库 artifact/version；
- 默认构建、发布和测试移除过渡编译器；
- 保留带 ABI version、source hash 和 artifact hash 的 bootstrap snapshot；
- 删除后重新运行阶段 F/G 的全部 gate。

验收：默认链不再依赖过渡实现，仍可从 seed 和 snapshot 重建 native `lainc`。

## 12. 阶段 I：固定点之后的 VM 与 LAINIR 维护

这些工作不阻塞当前自举固定点，除非某项成为明确的物理能力缺口。

### I1. 多 TCB VM 控制面

在单 TCB/单 VSpace Eval VM 上扩展 VSpace、TCB、Endpoint、Trap 和 CSpace 的公共 contract。每个对象都必须先有规范、至少一个 provider 和验证 fixture；对象名称本身不构成新的 LAINIR 指令。

### I2. 平台 lowering

定义解释器、native backend、Linux 和裸机对 VM 对象的 lowering。软件 MMU、`#swap_context` 和 demand paging 只有在多个 backend 共享同一 contract 后才进入实现。

### I3. LAINIR 物理维护

- data relocation、只读保护和跨 backend alignment；
- activation 地址逃逸、use-after-return、越界和存活字节预算；
- 浮点文本、ABI、NaN/无穷/舍入差分；
- trap 的 procedure/source span、统一错误分类和生成式 kind 映射。

长期约束：不恢复 legacy 类型和模糊整数操作；字段布局继续展开为 `#lea + #load/#store`；不加入宽泛的 `#primitive`；AST、module、generic、effect 和 source-language type 保持在 LAINIR 之外。

## 13. 并行开发规则

- formal stdlib、API contract 和 test provider 可以并行；
- LAINIR provider 维护者修改 provider 和 contract tests，不修改 lainc lowering 规则；
- lainc 维护者只通过冻结 API 请求新的物理能力；
- API 行为变化必须同时更新 schema version、contract test 和两侧迁移说明；
- 单 TCB/单 VSpace Eval VM 完成前，Meta owner/result 迁移不得宣称完成；
- 固定点之后，VM 控制面和 LAINIR 物理维护可以独立于 compiler core 推进。

## 14. 总验收

路线图完成至少需要：

1. formal stdlib 可从源码重建；
2. recording provider、默认 provider 和 Eval VM 通过同一 contract tests；
3. `src/lainc` 不包含 LAINIR 具体实现；
4. 真实程序、native backend、clean build 和 gen2/gen3 gate 通过；
5. `#eval` 的 session、activation、owner、quota、capability 和 trap 语义有可执行证据；
6. 默认工具链不依赖过渡实现；
7. 后续 VM 对象和 LAINIR 新能力都遵循规范→contract→provider→fixture 顺序。

`docs/04-lain-vm.md` 是 VM 的架构设计说明；当它与本路线图或 `docs/01-lain-ir.md` 冲突时，以当前规范和 contract tests 为准。
