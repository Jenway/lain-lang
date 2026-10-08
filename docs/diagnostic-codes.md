# 诊断码

Lain 的失败报告分两套机制，共用同一片四位十进制号码空间：

- **Trap**：VM 拒绝执行。引擎把码写进 LainVmTrap.status 并以 LAINVM_SLICE_TRAPPED 返回，
  由驱动或 TCB 的 fault_handler 处置。
- **拒绝码**：函数失败。函数把码写进 L1Diagnostic 并返回非 0（0 只表示成功）。

两条规则：

- **一段只属于一个模块**：新码落在自己段里。
- **退休的号码不复用**：例如 2025 已废弃，留着空洞也不补。

## 号段表

| 段 | 归属 | 码表位置 |
| :--- | :--- | :--- |
| 1000-1199 | Trap（vm/engine 发射，vm/quota 借 1044） | lain/vm/trap.h |
| 2000-2999 | LAINIR 验证 | lain/ir/verify.h |
| 3000-3999 | canonical 文本解析 | lain/text/parse.h |
| 9000-9099 | 映像装载 | lain/vm/engine.h |
| 9100-9199 | TCB admit 与重绑 | lain/vm/tcb.h |
| 9200-9299 | 后端 | lain/backend/emit.h |
| 9300-9399 | 编译期 apply 与其宿主 | lain/meta/apply.h |
| 9400-9499 | AstOut 与 Meta 库共享 | lain/meta/ast_v1.h、bootstrap/std/*.l1 |

段边界的登记表在 seed/include/lain/ir/codes.h（每个段一对 BASE/LIMIT 常量）；
每个有正式码表的模块都用 _Static_assert 把自己的码钉在段内：
trap.h、engine.h、tcb.h、verify.h、parse.h、emit.h、apply.h、ast_v1.h。

## 两套机制不混用

码只表示原因，不决定 kind：Trap 的 LAINVM_TRAP_EXECUTION / _CAPABILITY / _STATE
由发射点按当时的情形给，所以同一个码在不同站点可以有不同 kind。
拒绝码没有 kind —— 它有 L1Diagnostic 的消息与位置。

## 跨语言契约

9300-9399 与 9400-9499 同时被 C 侧（seed/）和 Meta 侧（bootstrap/std 的 .l1）
读取：只能新增，不能改义，也不能挪号。
