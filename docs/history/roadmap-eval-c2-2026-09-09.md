# C2：C seed 通过临时 TCB 执行 `#eval`

完成日期：2026-09-09。

C seed 的 `#eval` 已改为创建独立 VM control 和根 frame。外围参数及局部绑定按值复制到
根 frame；地址值仍引用调用者正在使用的地址空间。嵌套 `#eval` 创建独立控制对象，step
和 allocation 计数在嵌套链中连续消耗。

验证规则也已调整：`#eval` 块按表达式位置的预期类型验证，不再继承外围 procedure 的
返回类型。fold 将 `#bits`、稳定 `#data_addr`、过程地址、字符串和 `#unit` 结果改写为
普通 IR；backend 前不保留 `EXPR_EVAL`。

验证：

- `zig build`；
- `zig-out/bin/lainir-vm-control-test.exe`，其中包含真实 fold 后无 `#eval` 的检查；
- `python scripts/check_eval_tcb.py`，覆盖 capture、嵌套、地址逃逸、call-depth 和
  allocation quota；
- `lainir-seed run scripts/fixtures/eval_tcb_context_type.l1 main`。
