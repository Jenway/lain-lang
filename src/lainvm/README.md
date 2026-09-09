# LAINVM

This directory is the formal Lain implementation boundary for LAINVM.

LAINIR defines and verifies physical procedures, blocks, instructions and
values. LAINVM executes verified LAINIR and owns the execution model: TCB,
VSpace, Trap, scheduling, activation lifetime and `#eval` temporary TCBs.

`interpreter.lain` is the current Lain implementation of the execution side:
TCB, VSpace, activation lifetime, Trap state, scheduler state and instruction
execution live here. The C seed implementation remains under `seed/` and must
follow the same contract.

`src/lainir/api/` owns only the IR model, construction, verification and
printing. It does not import or construct a VM. A compiler or host that needs
execution explicitly composes `lainvm.Interpreter` with its LAINIR model and
external-call policy.

`execute_child` 是编译期 `#eval` 的 VM 原语。LAINIR lowering 先把自由局部变量物化为
临时根过程的物理参数；该入口随后创建独立 TCB、复用调用者的 VSpace，并以普通 `Value`
或 `Trap` 结束。它不接收源级 AST、类型值或模块值。
