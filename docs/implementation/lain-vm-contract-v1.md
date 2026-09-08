# LAIN-VM 单 TCB / 单 VSpace contract v1

状态：contract 草案，当前只覆盖 `#eval` 的单 session。它把已经存在的
`CompileContextV1` 和 `EvalResultV1` 约束整理成 VM 边界；它不引入新的 LAINIR
指令，也不宣称多 TCB、Trap 或软件 MMU 已经接入 LAINIR evaluator。Endpoint 已有 seed
provider control API，但仍通过 VM control plane 保持 opaque。

## 当前对象映射

| VM 术语 | 当前实现 | 生命周期 |
| --- | --- | --- |
| 单 TCB | 参考 evaluator 的 `LainVm.tcb` | 一次 root execution request |
| 单 VSpace | 参考 evaluator 的 `LainVm.vspace` | request 结束逻辑释放，物理回收由 provider 负责 |
| CSpace | `LainVm.cspace` 的 capability object table | slot 级 owner/active 授权 |
| Endpoint | seed `LainirVmEndpoint` control object | 单发送者/单接收者等待、交接和取消 |
| Trap | `LainirVmTrap` control record | kind/status/procedure/region/position/line/column/source range |
| Eval result | `EvalResultV1` | owner transfer/release 明确 |

参考 evaluator 的 TCB 当前字段为状态、step/depth limit、使用计数、current activation、
next activation 和 Trap；VSpace 保存 owner、release 状态、storage、地址/activation
表和 allocation quota。provider contract 仍以 `CompileContextV1` 的 owner、limit 和
计数规则作为边界语义。limit 为零表示该项不设上限；任何计数器超限都返回失败状态，
不能静默继续执行。

TCB 的控制面状态遵循 `READY -> RUNNING -> BLOCKED -> RUNNING -> DEAD`。suspend 只
接受 owner 持有的 RUNNING TCB，并保存 execution position；resume 只接受同一 owner
持有的 BLOCKED TCB，并恢复保存的位置。参考 evaluator 现在把当前 procedure 和 region
offset 保存在内部 TCB；这些字段属于 VM control state，不是 LAINIR 中的 TCB 数据字段。

当前 scheduler contract 只允许一个 current runnable TCB：READY TCB 可以加入 runnable
集合并被选中启动；当前 TCB suspend 后释放 current 槽位，BLOCKED TCB 才能被 resume
并重新成为 current。fixture 已覆盖两个 TCB 的 handoff 和 resume，仍不代表递归 evaluator
已经可以从任意 instruction offset 继续执行。

Endpoint contract 支持单发送者和单接收者 rendezvous。没有对端时发送或接收会留下等待
状态并将对应 TCB 置为 `BLOCKED`；对端到达后一次性交接、清空等待并恢复对应 TCB。
发送者或接收者可以取消自己的等待，取消会恢复该 TCB。seed `LainirVmEndpoint` 只保存
TCB control object、owner 和标量 payload，不保存 activation 地址；owner、重复等待和
非法状态都会被拒绝。`lainir_vm_endpoint_bind` 可将 send/receive 注册为 evaluator
capability；无对端返回 `LAINIR_RUN_BLOCKED`，恢复后消费 control-owned pending result。
`LainirVmControl` 暴露只读 suspend reason，Endpoint 等待固定为
`LAINIR_VM_SUSPEND_ENDPOINT`，scheduler 可以据此区分主动 yield 与资源等待。
解释器 Trap 和 capability rejection 会写入同一个 control-owned `LainirVmTrap`，调用者
可以在 `LAINIR_RUN_TRAP` 后读取记录；当前 seed 已记录 instruction line/column 和
source byte range，并通过 `lainir_vm_control_abort` 将 TCB 转为 `DEAD/TRAPPED`。parser
token 的精确结束边界仍待接入。

continuation 属于 TCB 的 VM 内部状态。参考 evaluator 当前已经保存 procedure、region、
instruction offset、suspend reason 和 `CallFrame` 调用帧链。可恢复的 slice 仍需要把该
调用帧链接入 scheduler；切换只发生在 instruction boundary 或
Endpoint 等待点。`run_slice` 的结果固定为 `RUNNABLE`、`BLOCKED`、`DONE` 或 `TRAPPED`；
参考 VM 内部已提供该入口，slice fuel 只在 instruction boundary 检查，表达式内部只计
全局 step quota。含有动态 arena 的 `LainVm` 不通过普通 Lain struct value 返回；持久
scheduler state 由 VM/provider 的 opaque control object 持有。`VmControl` fixture 已覆盖
跨 procedure 的 fuel yield、callee frame resume 和最终 VSpace release；现有 suspend/resume
API 仍同时验证状态转换和 position 保存。seed runtime 还提供 `LainirVmControl` opaque C
API，并由 `lainir-vm-control-test` 验证同一组 control-plane 不变量。`LainirRunRequest`
现在可以携带该对象，解释器在 instruction boundary 消耗 fuel，并以
`LAINIR_RUN_SLICE` 报告切片边界。root procedure 的 frame、locals、activation 和下一条
instruction 保存在 control object 的 opaque backend state 中，下一次 `lainir_run` 可以
恢复；slice 之间允许 TCB 经过 BLOCKED/RESUME 状态转换后继续该 continuation。nested
procedure 进入和返回时更新 control plane 的 CallFrame 栈，当前 nested
调用仍在一个 slice 内原子执行，nested frame/locals 的跨 slice 恢复留在后续阶段。

`EvalResultV1` 携带 status、kind、scalar value、object handle 和 owner。对象结果
必须带 owner；转移只允许从当前 owner 到目标 context，释放后不得再次使用。

## v1 不变量

1. 一次 session 只有一个执行上下文和一个资源空间。
2. `#alloca` 仍属于当前 procedure activation；它不是 VSpace 的长期对象。
3. `#data` 是只读静态数据；任何写入保护由 verifier/provider contract 负责。
4. `#eval` 只能通过显式 evaluator/provider 能力执行，Meta 不维护第二套物理求值器。
5. session 结束时，未转移的 request-owned object 由 owner 统一回收。
6. TCB、VSpace、Endpoint、Trap、CSpace 的名称不能直接变成新的 LAINIR 指令或
   模糊的 `#primitive` 入口。

参考 evaluator 的 external call 先经过 VM capability gate；gate 同时检查 TCB 状态和
VM capability context。当前 context 由 `LainVm.cspace` 的两个 slot
owner/active/external capability objects 表示。gate 扫描有效 slot，要求至少一个
external capability；slot 身份和权限语义分开。

provider-neutral CSpace contract 已定义 capability object：每个对象带 name、owner 和
active 状态，可以由当前 owner transfer 或 revoke；foreign owner、非 active capability
和未绑定到 CSpace 的对象都会被拒绝。参考 evaluator 已用两个 capability slot 替换旧的
布尔 policy，并提供按 slot 的 `capability_transfer` 和 `capability_revoke` VM control
API；external-call gate 使用更新后的 owner/active/external 状态。

## 已有验证

```text
python scripts/check_compile_context.py
python scripts/check_eval_object_matrix.py
python scripts/check_lain_vm_contract.py
python scripts/check_lainir_physical_safety.py
python scripts/check_lainc_lainir_api_baseline.py
```

这些检查覆盖 owner transfer/release、step/allocation/recursion quota、capability mask，
以及 scalar/type/module/AST result kind。owner handle 在 transfer 后只能由新 owner 使用，
release 后不能再次使用或重复释放。

`check_lain_vm_contract.py` 使用独立 recording provider 验证 VSpace owner/quota/reset、
arena generation 和 stale-handle 拒绝
和 TCB owner/state/step/suspend/resume/scheduler 规则，以及 Endpoint 的 rendezvous/cancel、CSpace
的 capability allow/deny、Trap 的 kind/source/status 字段。这个 fixture 只固定对象之间
的 API 语义，不伪装成 LAINIR 指令或真实运行时对象实现。

`check_lainir_physical_safety.py` 验证参考解释器拒绝写入只读 `#data`、拒绝返回当前
procedure activation 的 `#alloca` 地址，并拒绝 activation 内的越界 load。

当前 evaluator 还会在 procedure 返回时失活该 activation 派生的地址句柄；后续地址
解析拒绝使用失活句柄。这是向 LainVM activation lifetime 迁移的第一步，尚不等同于
完整的 VSpace region table 或 TCB runtime。当前参考 evaluator 为 VSpace 和地址句柄
记录 owner，并在 VM 返回时通过 `release_vspace` 关闭逻辑 VSpace、失活所有地址句柄和
activation frame，再调用 `Memory.Bytes.release` 释放 backing arena。具体的物理分配策略
仍由 provider 决定。

## 后续扩展顺序

多 TCB 扩展必须先增加独立的 `VSpaceV1`、`TCBV1`、`EndpointV1`、`TrapV1` 和
`CSpaceV1` contract，分别提供 provider 实现和 fixture。上下文创建、挂起、恢复和
销毁属于 VM control API。TCB 对 LAINIR 保持 opaque；上下文创建、挂起、恢复和销毁
通过 VM scheduler 与 Endpoint contract 完成。只有这些 contract 在至少一个 provider
上通过后，才进入软件 MMU、demand paging 或 Linux/bare-metal lowering。
