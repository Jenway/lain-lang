# Lain 已完成路线快照

基线日期：2026-09-07。

本文保存当前路线图中已经有可执行证据的工作。后续路线图只追踪未完成目标；历史路线和更早的设计记录见本目录的其他文档。

## 编译器与 LAINIR API

- `src/lainc` 已建立 Builder、Artifact、Eval v1 API shape，并通过 capability 注入。
- lowering 和 compiler result 已切到 API 边界；活跃 source closure 不包含 LAINIR 具体实现。
- 默认、formal、seed、recording provider 的接口约定已有 contract 和 smoke 验证。
- formal stdlib 可从源码重建；旧 `3003` 构建阻塞已解除。
- `src/lainc` 旧 `l1_*` 模块和旧 bootstrap monolith 已移出 compiler source closure。
- source closure 连续重建已通过 canonical artifact、extern、procedure label/header 和 body hash 比较：361 procedures、75 externs。
- gen2/gen3 self-host 已通过，seed compiler 可生成独立的 `lainir-compiler.exe`。

## Lain-written C backend

- active compiler 生成 backend artifact 和 ABI manifest，`zig cc` 可以编译、链接并执行 native `lainc`。
- backend migration、formal provider smoke、历史 C procedure 差分和 artifact determinism gate 已通过。
- native `lainc` 已能编译 `src/lainc/backend_c.lain` 并生成 L1 artifact。
- native matrix 当前有 20 个成功单文件、1 个成功多文件和 41 个带稳定诊断码的失败 fixture。
- 空输入、失败原子性、无 NUL artifact 和 canonical procedure/body hash 均有检查。

## Bootstrap 与发布链

- snapshot manifest 包含 ABI version、source closure hash 和 artifact hash，并已通过校验。
- 默认 native build、bootstrap package、版本化安装和端到端 release gate 已在本地通过。
- `.github/workflows/lain-bootstrap.yml` 已接入 seed build、snapshot、native matrix、package/install gate 和综合 API baseline。

实际 CI runner 的运行时长和依赖仍属于当前路线图，不在本快照中宣称完成。

## Eval / VM contract

- CompileContextV1 的 owner、step、quota、recursion 和 capability 语义已有检查。
- recording provider 已验证 VSpace owner/quota/reset、TCB owner/state/step quota、Endpoint rendezvous、CSpace capability allow/deny 和 Trap 字段保留。
- Eval result matrix 已覆盖 scalar、type、module 和 AST result kind。
- 物理安全检查已覆盖只读 `#data`、activation-scoped `#alloca` 和 activation 内越界 load。

这里完成的是 API 和 recording contract，不代表真实的多对象 LainVM runtime 已完成。

## LAINIR 物理语义清理

- 当前物理语义使用只读静态 `#data`、当前 procedure activation 生命周期的 `#alloca`、`#lea`、typed `#load/#store` 和明确整数操作。
- legacy 类型、模糊整数操作、`#field` 和宽泛 `#primitive` 已从当前规范和实现路径移除。

