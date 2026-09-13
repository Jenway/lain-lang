# 当前路线图

当前执行计划只有 [`lain-roadmap.md`](lain-roadmap.md) 一份，只保留未完成工作、依赖与验收。
当前优先事项是让库 Meta 完整处理 AST 与语言语义，并为编译期计算生成 LAINIR `#eval`。
正式 lainc 的运行能力组合、编译器固定点以及 native/发布验收仍待完成。

实现记录与历史测量见
[`2026-09-13 完成归档`](../history/roadmap-lain-completed-2026-09-13.md)；更早的记录见
[`2026-09-07 完成归档`](../history/roadmap-lain-completed-2026-09-07.md)。归档用于追溯，
不定义当前架构，也不代表当前源码重新通过所有历史 gate。

所有切片必须以可执行检查、契约测试或固定点比较验收。完成后先写入新的完成快照，再从
active roadmap 移除。新增物理能力先更新规范与契约；语言规则由库 Meta 实现。

最新执行证据见 [`编码 1d 审计快照`](../history/roadmap-lain-1d-2026-09-13.md)。
1e 的阶段交接实现见 [`部分实现快照`](../history/roadmap-lain-1e-stages-2026-09-13.md)。
宏辅助规则的库归属见 [`宏切片快照`](../history/roadmap-lain-1e-macros-2026-09-13.md)。
默认基本宏展开见 [`默认宏切片`](../history/roadmap-lain-1e-default-macros-2026-09-13.md)。
正式参数隔离与递归表达式切片见 [`正式参数快照`](../history/roadmap-lain-1e-formal-arguments-2026-09-13.md)。
剩余工作包括完整宏/attribute、正式库语义与跨阶段资源/失败验收。

普通返回表达式的已完成切片见 [`1h 表达式快照`](../history/roadmap-lain-1h-expressions-2026-09-13.md)；完整 lowering 和专用 writer 退役继续跟踪。
