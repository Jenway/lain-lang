# C1：`#eval`、TCB 和 Trap 执行契约

完成日期：2026-09-09。

规范已固定以下规则：`#eval` 隐式按值捕获自由物理局部值；每次执行创建临时 TCB 并共享
调用者 VSpace；嵌套 TCB 共同消耗预算；正常执行只交付普通 LAINIR 值；Trap 沿调用点
同步传播而不进入值包装。

LAINIR API contract 已删除旧 `EvalShape` 及其 `Result { status, value, trap... }` 公开
协议。未来 VM 公开接口归 `src/lainvm/`，C3 迁移 Lain 实现。

规范和实现差异审计见
[`../implementation/c1-eval-contract-audit.md`](../implementation/c1-eval-contract-audit.md)。
