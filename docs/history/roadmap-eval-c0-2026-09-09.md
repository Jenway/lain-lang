# C0：删除错误的求值结果协议

完成日期：2026-09-09。

C0 删除了 `EvalResult`、结果种类、结果资源归属、代际、sidecar、payload adapter
和 `LainirVmSession`。依赖这些接口的旧 LAINIR Meta 求值器、缓存、lowering、fixture
和独立 frontend 构建入口一并删除。

保留下来的 VM 基础包括 TCB、VSpace、Trap、执行预算、continuation、scheduler、
Endpoint 和 capability。TCB 结束不再重置 VSpace，以允许临时 TCB 与调用者共享同一
地址空间。

完成时通过以下检查：

```text
zig build
seed/zig-out/bin/lainir-vm-control-test
python scripts/check_lain_vm_contract.py
python scripts/check_compile_context.py
python scripts/check_lainir_api_behavior.py
```

旧 Meta 源码构建暂时不可用。冻结的 Lain 编译器 artifact 继续作为临时启动工具；
后续阶段从 C1 的 `#eval`、TCB 和 Trap 规范开始。
