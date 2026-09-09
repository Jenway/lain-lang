# C3：分离 LAINVM

完成日期：2026-09-09。提交：`6290d40 lainvm: separate execution from lainir`。

Lain 写的解释器已从 `src/lainir/api/` 移入 `src/lainvm/interpreter.lain`。LAINIR API
现在只保留物理 IR 的表示、构造、验证和打印；LAINVM 拥有 TCB、VSpace、Trap、预算、
capability、调度状态和指令执行。

LAINVM 的公开根过程与子 TCB 执行入口只产生物理 `Value` 或 `Trap` effect。内部的
`Flow` 仅用于解释器控制流，不是公开结果协议。`execute_child` 使用调用者 VSpace、
CSpace 与预算账户，创建独立 TCB 执行 lowering 已经准备好的根过程参数。

验证：

- `python scripts/check_lainvm_boundary.py`；
- `python -c "... composed_compiler_sources ..."`，确认 LAINIR 与 LAINVM source manifest
  分开且组合闭包包含 VM；
- `zig build`；
- `lainir-vm-control-test`；
- `python scripts/check_eval_tcb.py`；
- `python scripts/check_lainir_physical_safety.py`。

冻结的旧 bootstrap compiler 依赖已删除的 legacy Meta/Eval 源码，因此不用于验证本阶段的
Lain 源码。新 VM 的实际编译、行为对照和固定点比较由 C5 的新 bootstrap 路径完成。
