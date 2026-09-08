# LAINVM

This directory is the formal Lain implementation boundary for LAINVM.

LAINIR defines and verifies physical procedures, blocks, instructions and
values. LAINVM executes verified LAINIR and owns the execution model: TCB,
VSpace, Trap, scheduling, activation lifetime and `#eval` temporary TCBs.

The current Lain interpreter still combines some LAINIR and LAINVM code in
`src/lainir/api/l1_interpreter.lain`. C1 defines their interface; C3 moves the
execution implementation here. The C seed implementation remains under
`seed/` and must follow the same contract.
