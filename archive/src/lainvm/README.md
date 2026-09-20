# LAINVM

This directory holds the LAINVM **execution contract**.

LAINIR defines and verifies physical procedures, blocks, instructions and
values. LAINVM executes verified LAINIR and owns the execution model: TCB,
VSpace, Trap, activation lifetime and `#eval` temporary TCBs.

`api_contract.lain` declares the boundary a VM provider must satisfy:
`ExecutionShape` with Artifact, Procedure, Value, ValueVector, the Allocation
and Eval effects, and the `eval` entry. It is the interface `src/lainc`
compiles against, and it is what a provider is measured by.

`execute_child` 是编译期 `#eval` 的 VM 原语。LAINIR lowering 先把自由局部变量物化为
临时根过程的物理参数；该入口随后创建独立 TCB、复用调用者的 VSpace，并以普通 `Value`
或 `Trap` 结束。它不接收源级 AST、类型值或模块值。

## Providers

The contract has three implementations; they are independent and share only
this file.

| Provider | Path | Status |
| --- | --- | --- |
| C seed | `seed/src/interpreter/` | working; runs in CI |
| bootstrap | `bootstrap/compiler/` | working; covered by the gate set |
| Lain | `docs/history/formal-implementations/interpreter.lain` | **paused and archived** |

The Lain-written interpreter compiled and passed the verifier but never ran:
the only reference to `execute_child` anywhere was a string match in a gate.
It was archived on 2026-09-12 to be rewritten in Lain once the language is
mature. See `docs/history/formal-implementations/README.md`.

The C seed therefore remains the reference implementation. A compiler or host
that needs execution composes a provider with its LAINIR model and
external-call policy; it does not reach into any provider's private state.
