# Lain 项目路线图

基线日期：2026-09-07。

本文只记录从当前代码状态开始的未完成工作。已经完成的阶段、旧方案和逐次实施记录见 [`../history/`](../history/)。

## 当前基线

以下工作已经达到当前路线图所需的基线，不再作为独立阶段重复追踪：

- formal stdlib 可以从源码重建，旧的 `3003` 构建阻塞已解除；
- `src/lainc` 的 Builder、Artifact、Eval v1 shape 已建立，并通过 capability 注入；
- 旧 `src/lainc/l1_*` 模块和旧 bootstrap monolith 已移出 compiler source closure；
- lowering 和 compiler result 已切到 API 边界；
- 默认 Provider(Memory) smoke、seed verifier、backend capability ABI 和 source-closure 确定性 gate 已有证据；
- `#data`、activation-scoped `#alloca`、`#lea`、typed `#load/#store` 和明确整数操作已经成为当前物理语义；
- gen1→gen2→gen3 已有部分固定点证据。

这些基线仍需在最终验收中重新运行，但不再拆成新的 roadmap 阶段。

## 当前未完成目标

当前剩余工作按依赖顺序排列：

```text
P0 Eval contract
  -> P1 单 TCB / 单 VSpace Eval VM
  -> P2 provider owner/result 与 Meta Eval
  -> P3 真实 lainc/native build
  -> P4 gen2/gen3 与工具链清理
  -> P5 多 TCB VM、平台 lowering 和 LAINIR 后续能力
```

`#eval` 的执行环境是 P1 的一部分。它不能继续作为直接调用宿主 interpreter 的临时路径存在。

## P0：冻结 Eval contract

目标：让 EvalApi 的行为由 contract 固定，而不只是固定函数签名。

必须明确：

- `EvalSession` 的创建、运行、成功返回、trap、limit、取消和销毁；
- activation 的建立、回收和递归调用隔离；
- step、call-depth、allocation quota 的计数点和诊断；
- capability 的显式传入、空 capability 默认值和拒绝错误；
- scalar 按值返回的复制 ABI；
- type、module、AST 等非 scalar 结果的 opaque owner 和释放规则；
- nested eval 创建子 session，或由 contract 明确拒绝；
- 相同输入的确定性和失败原子性。

验收：recording provider、默认 provider 和 seed adapter 通过同一组 session、lifetime、quota、capability、trap 和 determinism tests。

## P1：实现单 TCB / 单 VSpace Eval VM

目标：让每次 `#eval` 在独立的轻量执行上下文中运行。

```text
EvalApi
  -> EvalSession
     -> EvalTCB + EvalVSpace
        -> activation / memory / quota / capability
        -> typed LAINIR execution
        -> value or owned object result
```

`EvalTCB` 至少保存当前 procedure、instruction position、activation、fuel/depth、trap 和 result 状态。`EvalVSpace` 至少管理只读 `#data`、activation 内 `#alloca`、地址边界和 capability 集合。结果只能通过 result channel 离开 session，禁止 TCB 或 activation 地址逃逸。

工作：

- 实现 session、TCB 和 VSpace 的最小内部表示；
- 将现有 step/depth/allocation 限制接到 TCB/VSpace 生命周期；
- 在返回边界区分 scalar copy、owned object transfer 和非法裸地址逃逸；
- 为越界、use-after-return、activation escape、超限和 capability 拒绝生成稳定 trap；
- 为 nested eval 建立子 session 或稳定拒绝路径。

验收：现有 scalar fixture 在 Eval VM 内可重复执行；seed interpreter 不再需要未经定义的隐式地址复制；所有失败路径都有 contract test。

## P2：完成 provider 与 Meta Eval 迁移

目标：让正式 provider 真正兑现 P0/P1，Meta 只观察 EvalApi。

工作：

- 将 verifier、canonical printer 和 evaluator 的真实调用接到 Artifact/Eval API；
- 完成非 scalar provider-owned object 的 owner transfer/release；
- 覆盖 module、type、AST 结果、递归、nested eval、trap、limit 和 capability；
- 删除 Meta 对 interpreter、memory model、unit expression table 的读取；
- 保持 provider source manifest 与 compiler source closure 分离。

验收：默认 provider 和 test provider 通过同一 contract tests；Meta 的编译期执行不依赖具体 interpreter 实现；artifact、诊断和结果保持确定性。

## P3：完成真实 lainc 与 native backend

目标：把已经迁移的 API 边界推进到真实 compiler 输出。

工作：

- 扩展 module factory、captured binding、qualified member 和 unresolved member 诊断；
- 补齐前端和 lowering 的 source span；
- 比较 parser、verifier、evaluator、native backend 的 canonical artifact、ABI manifest、procedure header 和 body hash；
- 完成 backend capability 的历史 C 输出差分；
- 完成最终 clean native compiler build；
- 保证 empty、nonempty、多文件、module、record、函数调用和控制流程序 gate 通过。

验收：正式 provider、native backend、真实程序 gate 和 clean build 同时通过；失败路径不生成部分 artifact。

## P4：固定点与工具链清理

目标：让 `src/lainc` 生成的编译器接管自身构建，并移除过渡实现。

工作：

- bootstrap 编译器生成 gen2，gen2 生成 gen3；
- 比较 canonical artifact、verifier 结果、ABI manifest 和每个 procedure body hash；
- 从干净目录只用 seed、bootstrap snapshot 和源码重建；
- CLI、LSP 和测试 runner 使用同一正式标准库 artifact/version；
- 默认构建、发布和测试移除过渡编译器；
- 保留带 ABI version、source hash 和 artifact hash 的 bootstrap snapshot。

验收：gen2 与 gen3 一致，clean rebuild 成功，native `lainc` 可以编译真实程序，默认链不依赖过渡实现。

## P5：固定点之后的 VM 与 LAINIR 演进

这些工作不阻塞 P4，除非某项成为明确的物理能力缺口。

### P5.1 多 TCB 控制面

在单 TCB/单 VSpace Eval VM 上扩展 VSpace、TCB、Endpoint、Trap 和 CSpace 的公共 contract。每个对象都必须先有规范、至少一个 provider 和验证 fixture；对象名称本身不构成新的 LAINIR 指令。

### P5.2 平台 lowering

定义解释器、native backend、Linux 和裸机对 VM 对象的 lowering。软件 MMU、`#swap_context` 和 demand paging 只有在多个 backend 共享同一 contract 后才进入实现。

### P5.3 LAINIR 物理维护

- data relocation、只读保护和跨 backend alignment；
- activation 地址逃逸、use-after-return、越界和存活字节预算；
- 浮点文本、ABI、NaN/无穷/舍入差分；
- trap 的 procedure/source span、统一错误分类和生成式 kind 映射。

长期约束：不恢复 legacy 类型和模糊整数操作；字段布局展开为 `#lea + #load/#store`；不加入宽泛的 `#primitive`；AST、module、generic、effect 和 source-language type 保持在 LAINIR 之外。

## 并行开发规则

- P0/P1 由 Eval/API 与 provider 两侧共同维护，contract test 是唯一共同边界；
- provider 维护者修改 provider 和 contract tests，不修改 lainc lowering 规则；
- lainc 维护者只通过冻结 API 请求新的物理能力；
- API 行为变化必须同时更新 schema version、contract test 和两侧迁移说明；
- P1 完成前，Meta owner/result 迁移不得宣称完成；
- P4 完成后，P5 可以独立于 compiler core 推进。

## 总验收

1. formal stdlib 可从源码重建；
2. recording provider、默认 provider 和 Eval VM 通过同一 contract tests；
3. `src/lainc` 不包含 LAINIR 具体实现；
4. 真实程序、native backend、clean build 和 gen2/gen3 gate 通过；
5. `#eval` 的 session、activation、owner、quota、capability 和 trap 语义有可执行证据；
6. 默认工具链不依赖过渡实现；
7. 新的 VM 或 LAINIR 能力遵循“规范 -> contract -> provider -> fixture”顺序。

`docs/04-lain-vm.md` 是 VM 的架构说明；若它与本路线图或 `docs/01-lain-ir.md` 冲突，以当前规范和 contract tests 为准。
