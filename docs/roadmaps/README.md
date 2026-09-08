# 当前路线图

当前路线图只有 [`lain-roadmap.md`](lain-roadmap.md) 一份。错误的求值结果协议已经删除，`seed`、`bootstrap` 和 `src` 已经分离；下一阶段固定 `#eval`、临时 TCB、共享 VSpace 与 Trap 的语义，并在 seed 中实现这条执行路径。

已经完成的阶段归档在 [`../history/roadmap-lain-completed-2026-09-07.md`](../history/roadmap-lain-completed-2026-09-07.md)。更早的路线版本和旧方案见 [`../history/`](../history/)，它们只用于追溯。

所有阶段都必须以可执行检查、contract test 或固定点比较作为完成条件。一个阶段完成后先写入完成快照，再从 active roadmap 移除。新增 VM 或 LAINIR 能力必须先更新规范和 API contract，再进入 provider/runtime 或 compiler 实现。
