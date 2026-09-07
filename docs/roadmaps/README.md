# 当前路线图

当前路线图只有 [`lain-roadmap.md`](lain-roadmap.md) 一份。它合并了编译器自举、lainc→LAINIR API 迁移、单 TCB/单 VSpace Eval VM 和 LAINIR 物理层维护计划。

历史版本和已经完成的旧路线见 [`../history/`](../history/)。其中带有 `roadmap-2026-09-07` 的文件是本次合并前的内容快照，不再作为当前计划入口。

所有阶段都必须以可执行检查、contract test 或固定点比较作为完成条件。新增 VM 或 LAINIR 能力必须先更新规范和 API contract，再进入 provider 或 compiler 实现。
