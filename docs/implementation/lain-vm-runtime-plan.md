# LainVM runtime 实施计划

状态：当前主线计划。

本文描述如何把当前 `src/lainir/api/l1_interpreter.lain` 中的一次性结构化 evaluator
迁移为 LainVM 执行后端。LainVM 是架构根对象；`#eval` 是它的一个调用入口。本文不把
TCB、VSpace 或 Endpoint 编译成 LAINIR 数据，也不向 LAINIR 增加上下文控制指令。

## 1. 当前实现基线

当前执行路径是：

```text
default_provider
  -> Interpreter.execute_limited
      -> ExecutionState
      -> execute_procedure
      -> execute_region
      -> evaluate_expression
```

`ExecutionState` 目前同时保存执行限制、当前 activation、连续 storage、地址表和外部
调用开关。调用过程使用宿主递归，`#alloca` 追加到一块全局 storage，`#data` 直接从
artifact 读取，结果通过 `Result { status, value }` 返回。

当前实现已经有的检查：

- step、call-depth 和 allocation quota；
- 只读 `#data` 写入拒绝；
- 当前 procedure 返回 `#alloca` 地址拒绝；
- activation 内越界 load/store 拒绝；
- external call 默认关闭；
- scalar、type、module 和 AST result kind 的 API contract。

当前已推进的 runtime slice：地址句柄带有 activation 存活状态；procedure 返回时会失活
该 activation 派生的所有句柄；统一的地址解析会让后续 `#lea`、`#load`、`#store` 和
`#call_indirect` 拒绝失活句柄。这个 slice 已随 API baseline、physical-safety、formal
fixture 和 361-procedure source-closure gate 通过。

当前第二个 runtime slice 也已落地：`ExecutionState` 已拆为 `LainVm`、`VSpace` 和
`TCB` 三个内部对象；storage、address table 和 allocation quota 归 VSpace，执行步数、
调用深度、activation 和 external-call 权限归 TCB。root TCB 现在显式经过 READY、RUNNING
和 DEAD 状态，step 消耗只允许发生在 RUNNING 状态。Provider smoke 和完整 API baseline
已通过这次拆分。

当前 root execution entry 也已收敛：`execute` 与 `execute_limited` 通过同一个 `new_vm`
构造 root `LainVm`，随后都进入 `execute_procedure` 和 `finish_vm`。两条入口只在限制值
和 external-call policy 上不同，不再各自维护一份 VM 初始化路径。

owner/release 的 provider-neutral fixture 也已补齐：VSpace 和 TCB 只能由 owner 操作，
result/arena handle 只能转移给新 owner，release 后不能使用或重复释放。这个 contract
目前仍是边界规范，尚未把 evaluator 内部的 `Memory.Bytes` reset 接到返回路径；fixture
现在额外固定了 provider arena generation，VSpace reset/release 会使旧 arena handle 失效。

当前 evaluator 已把这条边界落到地址表：VSpace 带有 owner 和 released 标记，每个地址
句柄携带 owner；地址解析会拒绝 foreign owner 或已 released 的 VSpace。VM 返回时先关闭
逻辑地址生命周期；`release_vspace` 会统一失活地址句柄和 activation frame，并调用
`Memory.Bytes.release` 回收 backing arena，再替换为空 storage。物理回收仍遵循 provider
的 allocation policy，但已经由参考 VM 的返回路径触发。

root TCB 的状态转换也已显式化：`new_vm` 创建 READY TCB，`start_tcb` 将其置为 RUNNING，
执行完成后 `finish_vm` 置为 DEAD；step accounting 仍只接受 RUNNING 状态。

provider-neutral TCB fixture 现在也覆盖 suspend/resume：挂起只接受 RUNNING TCB，保存
execution position 并进入 BLOCKED；恢复只接受 BLOCKED TCB，并恢复保存的位置。真实
evaluator 尚未把宿主执行循环切成可挂起的 scheduler slice。

同一 fixture 已加入单 runnable scheduler：READY TCB 进入 runnable 集合，启动后占用
current 槽位，suspend 释放槽位，resume 重新选择该 TCB。它固定了 control-plane API，
不宣称递归 evaluator 已经能从任意 instruction offset 继续执行。

Endpoint fixture 现在覆盖单发送者/单接收者 rendezvous 和等待取消：无对端时保留等待，
交接后清空等待，发送方或接收方取消都会清除自己的等待状态。seed runtime 也提供了
opaque `LainirVmEndpoint`：等待项只保存 TCB control object、owner 和标量 payload，
发送/接收会驱动 TCB 的 `BLOCKED` 与 `RUNNING` 转换，并把 suspend reason 固定为
`LAINIR_VM_SUSPEND_ENDPOINT`，不保存 activation 地址。LAINIR
当前没有 Endpoint 指令；provider 可以通过 `lainir_vm_endpoint_bind` 将 send/receive
capability 注册到真实 evaluator。无对端调用返回 `LAINIR_RUN_BLOCKED`，root instruction
保持为待重试位置；对端到达后 pending result 唤醒 TCB，下一次 `lainir_run` 消费该结果。

scheduler fixture 已扩展到两个 TCB：同一时刻只允许一个 current，挂起第一个 TCB 后
才能启动第二个，第二个挂起后可以恢复第一个。这个 handoff 仍然是 control-plane
验证，当前已由 scheduler 的 `run` / `run_slice` 入口把一个真实 evaluator slice 接到 current TCB：
入口注入该 TCB 的 control object 和 owner，Endpoint 阻塞或终止后自动释放 current。
`lainir_vm_scheduler_select` 现在按 attachment 顺序轮转选择 READY 或已被唤醒的 RUNNING
TCB；阻塞和终止的 TCB 会被跳过。v1 的公平规则由严格 round-robin 和 attachment 数量
给出，priority/weight 扩展不属于当前 contract。
control fixture 还覆盖两个 Endpoint 上的两个发送方并行阻塞、对应接收方分别唤醒和
pending result 隔离；Endpoint 仍保持单 sender/单 receiver rendezvous。
fuel 耗尽时 `run_slice` 会让 RUNNING TCB yield，释放 current 但保留 continuation；
三个 TCB 的轮转 fixture 已确认单个 TCB 不会连续占用 current。
LAIN-VM v1 采用严格 round-robin，不提供 priority 或 weight；对一组持续 runnable 的
attached TCB，一个 TCB 在自己 yield 后最多等待其余 `N-1` 个 runnable TCB 各被选择一次。
BLOCKED/DEAD TCB 不计入 `N`，所有 TCB 都不可运行时 `select` 返回空，不进行忙等。
两个 Endpoint 的并行 fixture 现在让两个发送方分别经过 nested helper 后阻塞；恢复时
各自的 pending result 必须回到对应 helper frame，随后才完成各自 root procedure。
Endpoint 销毁会取消并唤醒仍在等待的 TCB；已 DEAD 的 TCB 也可以从 Endpoint wait 中移除，
避免 Endpoint 保留悬空 control object。

参考 evaluator 现在也暴露了最小 VM control API：`new_vm`、`start_tcb`、`suspend_tcb`
和 `resume_tcb`。它们只改变 TCB 的 READY/RUNNING/BLOCKED 状态并保留 TCB position；
执行入口仍会在完成后转为 DEAD，control API 的存在不等于递归 evaluator 已经具备可恢复
的 instruction continuation。

external call 现在经过 `external_capability_allowed` VM gate；它会同时检查 TCB 是否
处于 RUNNING 和当前 VM capability context。这一步先固定了 provider dispatcher 不能绕过
VM 状态的边界。

第一步结构迁移已经完成：布尔 policy 不再存放在 TCB，而由 `LainVm.cspace` 中的
capability objects 保存；TCB 只保留执行状态，gate 从 VM capability context 读取 owner、
active 和 `external` 权限。当前 CSpace 表已有两个 slot，其中一个用于 external 权限，
另一个作为非 external capability。

CSpace contract fixture 现在有带 name、owner、active 状态的 capability object，覆盖
transfer、revoke、foreign owner 拒绝和未绑定对象拒绝；参考 evaluator 已经接入最小
CSpace 表和 owner/active capability object。

VM control API 已增加按 slot 的 `capability_transfer` 与 `capability_revoke`；操作只在
当前 owner、有效 slot 和 active capability 条件满足时成功，gate 会立即反映 revoke 或
owner 转移后的状态。该 API 和最小 CSpace 表已经进入参考 evaluator。

external dispatcher 现在扫描 CSpace，要求至少一个有效 capability 的 `external` 权限
为真；slot 身份和权限语义已经分开。

当前第三个 runtime slice 已落地：VSpace 保存 activation frame 表；frame 记录父级、
storage 起点、存储长度和 active 状态。过程进入时创建 frame，返回时关闭 frame；地址
解析同时检查地址句柄和所属 frame 的存活状态。Provider smoke 已通过该迁移。

当前第四个 runtime slice 已落地：TCB 保存独立的 Trap 记录，包含 kind、status、
procedure、position 和 active 状态；root TCB 完成时将非零执行结果归档为 Trap。当前
LainVM 的 TCB 已在执行循环中记录当前 procedure 和 region instruction offset；Trap
position 使用这个物理 offset，procedure 仍沿用当前 root execution boundary，后续再接入
真实调用栈和 source span。position 归属已从 `LainVm` 根对象收敛到 TCB，但宿主递归调用
仍未变成可恢复 continuation。TCB 现在同时记录当前 region 和 suspend reason；默认
`suspend_tcb` 使用 yield reason，Endpoint 等控制面可以通过 `suspend_tcb_reason` 记录
具体阻塞原因，resume 时清除该原因。参考 VM 还保存了 `CallFrame` 向量；非 external
procedure 进入时压入 procedure、region、activation 和返回位置，region instruction
boundary 更新帧位置，返回时弹出并恢复调用者位置。这提供了 continuation 的调用栈来源，
尚未让 scheduler 从该栈恢复执行。
Eval result 现在提供只读的 Trap kind、procedure 和 position 访问器。Provider smoke
已通过该迁移。

当前执行入口是一次性 root VM；它在 `finish_vm` 中已经完成 VSpace release，关闭地址和
activation handle 并释放 backing arena。尚未接入的是长生命周期 session 的显式 reset API：
如果以后允许一个 VSpace 跨多个 root execution 复用，就必须把 generation、result handle
和 activation storage 的失效规则暴露为独立 contract，不能把一次性 `finish_vm` 当作 session
reset 的替代品。
seed control plane 现在提供最小的 `LainirVmSession` opaque 对象：它登记 attached TCB，
要求全部 attached TCB 进入 DEAD 后才能 reset/release，并提供单调 generation。它暂时不
拥有 TCB 或 provider storage；这一步只固定 session 生命周期与 stale-handle 检查的控制面
边界，`LainirRunRequest` 已能携带 session 并由 `lainir_run` 检查 control attachment；
实际 evaluator 的跨 root execution 复用和 result handle 物理绑定仍留在后续实现。

这些检查是迁移的起点，不代表 LainVM runtime 已经存在。

## 2. 目标运行模型

```text
LainVM
  ├── VSpace
  │     ├── readonly #data regions
  │     ├── activation regions
  │     └── bounds / ownership / lifetime
  ├── TCB table
  │     ├── procedure and instruction position
  │     ├── activation frame
  │     ├── quota state
  │     ├── CSpace reference
  │     └── Trap/result state
  ├── scheduler
  ├── Endpoint table
  └── CSpace
        └── external capability objects
```

这些是 LainVM 的运行时对象。解释器可以用宿主结构体保存它们，Lain-written runtime
可以用明确的内部对象表示，native/backend 可以将它们 lowering 到寄存器、栈、线程和
操作系统资源。它们对 LAINIR 保持 opaque。

### 2.1 continuation slice 的边界

真实 scheduler slice 需要保存 TCB 的执行 continuation，而不是只保存一个诊断 offset。
参考实现的下一步固定为以下 VM 内部状态：

- 当前 procedure、region 和下一条 instruction offset；
- 从 root 到当前 activation 的调用帧链，每帧包含返回位置和 activation id；
- 当前 TCB 的 RUNNING/BLOCKED 状态以及挂起原因；
- 仍然存活的 activation frame 和它们所属的 VSpace。

调度器只在 instruction boundary 或 Endpoint 等待点切换 TCB。`run_slice` 消耗本次
slice 的 fuel，返回 `RUNNABLE`、`BLOCKED`、`DONE` 或 `TRAPPED`；它不把 TCB、VSpace、
Endpoint 或 continuation 编译成 LAINIR 数据，也不增加 `#init_context`、`#swap_context`
之类的 LAINIR 指令。挂起时 activation 保持有效，TCB 终止时才统一失活其 activation
地址并释放 VSpace。seed interpreter 现在已经能把 root procedure 的 frame、locals、
下一条 instruction 和 activation storage 放进 VM control 的 opaque backend state，跨
两次 `lainir_run` 恢复并完成；当前切片在 root instruction boundary 生效，嵌套 procedure
调用在同一切片内完成，避免在表达式副作用中间重入。nested procedure 进入和返回时也会
同步 VM control 的 CallFrame 栈，活动帧的 procedure、region、position 和 activation
可被 control plane 观察。参考 evaluator 的 slice fuel 只在 instruction boundary 检查；
表达式内部只计全局 step quota。

### 2.2 nested continuation 的验收边界

把 nested procedure 纳入可恢复 continuation 需要同时满足以下条件：

1. 挂起点保存当前 frame、locals、参数、activation 归属和调用者返回位置；
2. 保存表达式游标以及当前外部调用的 pending 状态，恢复时从调用点继续；
3. pending 结果必须带有调用种类和结果值，恢复路径不能重新执行已经完成的参数表达式；
4. 一个 nested call 只能完成一次，重复 resume、错误 owner 和失活 activation 都进入 Trap；
5. Trap 或 TCB 销毁时，所有 nested frame 和 activation 一起释放，不能留下 Endpoint 等待项。

当前实现已经用 linked continuation stack 保存多层 nested frame 的 locals、alloca、当前
block/instruction，并逐层通过 pending result 把返回值交回 caller；表达式 cache 也会随
frame detach 重挂载。现有 fixture 包含双层 nested `send_outer -> send_helper`，以及
receiver 在 Endpoint wait 前完成的 sibling `make_value`，确认恢复后两处副作用都只执行
一次；同一 fixture 也覆盖 `#if` 分支内的 Endpoint wait。仍未完成的是多个并行 pending
call 的通用保存格式。新增 fixture 覆盖 nested Endpoint wait 的取消恢复、Endpoint 销毁
时的 CANCELLED 结果，以及 TCB abort 后清除已登记 wait。
VM control 的 backend state 现在带显式 destructor；TCB abort/free/finish 会释放仍挂起的
continuation，interpreter 正常完成路径会先清空 state 再自行收尾，避免重复析构。
显式将 backend state 设为 NULL 也会同时清除 destructor，后续 TCB 复用不会沿用旧 state
的析构函数。
Trap fixture 还覆盖了 nested Endpoint wait 取消后 caller 继续执行并触发 interpreter Trap；
结果是 DEAD/TRAPPED，Trap 记录保留，scheduler current 已释放。
顺序 sibling nested call fixture 还覆盖同一 root 先后经过两层 `wrapper -> leaf` 等待
Endpoint A/B；每个 child 返回后不会遗留 `should_return`，root 会从保存的 instruction
继续执行第二个 wrapper。另一个 fixture 让两个结果都回传后 caller 再触发 interpreter
Trap，确认 pending result、nested frame 和 backend state 会在同一次 Trap 收尾中一起释放。

当前 nested continuation 的覆盖边界如下：

| 路径 | 状态 |
| --- | --- |
| 单 TCB、单 Endpoint 的 nested wait/resume | 已验证 |
| 两个 TCB、两个 Endpoint 的 nested pending result 隔离 | 已验证 |
| 同一 root 的连续 sibling nested wait | 已验证 |
| 取消、Endpoint 销毁、TCB abort 后的 continuation 清理 | 已验证 |
| 多个结果已回传后再 Trap 的统一清理 | 已验证 |
| 多个未完成 call 的通用 fan-out/fan-in 语义 | 未定义，留待后续模型设计 |

因此当前阶段剩余工作是把已定义的恢复路径推广到更多表达式形状，并继续保持
fan-out/fan-in 不通过隐式扩展当前单 sender/单 receiver Endpoint contract。它不再包含
已经由上述 fixture 固定的清理和结果隔离语义。

### 2.3 多 TCB/Endpoint handoff 的边界

多 TCB scheduler 接入时必须保持以下约束：

1. scheduler 维护唯一 current TCB；Endpoint wait 让出 current，唤醒时只恢复登记的 TCB；
2. Endpoint wait 项只保存 TCB control object、owner 和 payload，不保存 activation 地址；
3. pending result 必须绑定目标 TCB/frame，不能由另一个 TCB 读取；
4. 当前 Endpoint 仍是单 sender/单 receiver rendezvous；fanout、广播和多 receiver 排队
   属于后续模型，不能通过隐式扩展当前 API 得到；
5. 验收 fixture 必须覆盖两个 TCB 的 handoff、Endpoint 阻塞、对端唤醒、结果消费和取消，
   并确认两个 TCB 的 activation/VSpace 生命周期互不越界。

seed runtime 现在提供 opaque `LainirVmScheduler`，维护 attached TCB 集合和唯一 current
槽位；`lainir-vm-control-test` 已覆盖 attach、start、suspend、resume、release、admit、
finish、重复启动和 current-slot 冲突，也覆盖 Endpoint 阻塞后释放 current、对端交接、
唤醒后重新 admit 和 pending result 消费。`lainir_vm_scheduler_select` 已提供按 attachment
顺序轮转的 runnable 选择，`lainir_vm_scheduler_run_slice` 已把单个 current TCB 的 fuel
slice 接到 `lainir_run`，并把 evaluator status 规范化为 RUNNABLE、BLOCKED_RESULT、DONE
或 TRAPPED；fuel 耗尽会通过 `yield` 释放 current，三个 TCB 的轮转已有验证。它仍不
定义 priority/weight 扩展；严格 round-robin 的等待上界由 attachment 数量确定。

## 3. 阶段一：建立 LainVM 核心对象

目标：在现有 provider/interpreter 旁边建立真正的 VM 状态，而不改变 LAINIR 指令集。

工作项：

1. 定义 `LainVm` 根对象，拥有 VSpace、TCB、Endpoint、Trap 和 CSpace 表。
2. 定义 `VSpace`：readonly data region、activation region、owner、bounds 和 reset 状态。
3. 定义 `Tcb`：procedure、region/instruction position、activation frame、quota、capability
   context、状态、pending Trap 和 result。
4. 定义 `Activation`：frame owner、存活状态、存储范围、调用者和返回边界。
5. 定义结构化 `Trap`：kind、tcb、procedure、instruction position、source location 和
   status。
6. 定义 VM 内部 scheduler 状态；先只允许一个 runnable TCB，但状态模型必须支持多个 TCB。

验收：创建、启动、暂停、恢复、终止一个 TCB；销毁 TCB 时其 activation 地址全部失效；
VSpace reset 后旧 region 不能再次访问；所有状态转换有单元 fixture。

## 4. 阶段二：迁移当前 evaluator

目标：让 `l1_interpreter` 成为 LainVM 的执行后端。

工作项：

1. 用 `LainVm` 持有的 TCB/VSpace 状态替代 `ExecutionState` 中的混合字段。
2. 将 `execute_procedure` 的宿主递归调用改为 VM activation frame 的进入和退出。
3. 将 procedure 与 region offset 记录为 TCB 的 execution position。
4. 让 evaluator 每次执行经过 VM 的 step accounting 和 Trap channel。
5. 保留当前结构化 L1 evaluator 的语义，先不引入新的 LAINIR control instruction。
6. 让 `#eval` provider 调用 LainVM 的 root-TCB 执行入口，而不是直接调用旧 evaluator。

验收：现有 scalar native/formal fixture 结果不变；step/depth/allocation 失败分类不变；
同一 artifact 的结果和 Trap 确定；不存在绕过 VM 状态的执行路径。

## 5. 阶段三：完成 VSpace 和 activation 生命周期

目标：解决当前 evaluator 最严重的内存模型问题。

工作项：

1. `#data` 建立只读 region，load 只接受属于当前 VSpace 的 data address。
2. `#alloca` 为当前 activation 建立独立 region 或 frame slice。
3. 每次 `#lea` 产生带 region、owner、offset、length 的内部 address handle。
4. 每次 `#load/#store` 检查 region、bounds、权限、owner 和 activation 存活状态。
5. activation 返回时标记 dead，所有相关地址立即失效。
6. session/runtime 终止时释放全部 activation-owned storage；结果只能通过 result channel 离开。

验收：覆盖只读写入、越界、跨 activation 使用、返回地址、保存后使用、VSpace reset 后
使用和 allocation quota；每个失败都生成稳定 Trap，不能产生部分 artifact。

## 6. 阶段四：引入 TCB 控制和 Endpoint

目标：让协程和未来并发建立在 LainVM 控制面上。

工作项：

1. 实现 TCB 状态转换：ready、running、blocked、dead。
2. 实现 VM 内部 scheduler 的单 runnable TCB 路径。
3. 实现 TCB suspend/resume，保存和恢复 execution position 与 activation 状态。
4. 实现 Endpoint rendezvous：发送方阻塞、接收方到达、结果交接和取消。
5. 将 Endpoint 交接纳入 owner、capability 和 activation lifetime 检查。
6. 单 TCB scheduler 和 Endpoint recording fixture 已覆盖状态转换、rendezvous 和取消；
   两个 TCB 的 handoff/resume fixture 也已完成。seed runtime 的 Endpoint control API
   已完成同一 TCB 状态转换、owner 检查和 evaluator capability dispatch；无对端阻塞、
   对端交接和下一次 `lainir_run` 恢复已有 seed integration test。

验收：一个 TCB 可以主动让出并恢复；Endpoint 不保存悬空 activation 地址；非法状态、
owner 和 capability 操作均进入 Trap。

## 7. 阶段五：把 capability 与结果纳入 VM

目标：让当前的外部调用开关和结果结构变成 VM 语义。

工作项：

1. 将 `external_calls_allowed: bool` 替换为 TCB 对 CSpace 的引用。
2. 为 capability 定义 owner、授予、拒绝、撤销和外部调用边界。
3. 外部调用先经过 CSpace 检查，再由 provider/backend dispatcher 执行。
4. Trap 统一保存 TCB、procedure、position、source location 和 status。
5. scalar 按值复制；type/module/AST 结果通过 owned handle 转移；裸地址拒绝离开 VM。
6. nested `#eval` 使用子 TCB/VSpace，或由 contract 明确拒绝，不能隐式重入旧 evaluator。

验收：capability allow/deny、owner transfer/release、Trap 字段和四类 Eval result 在真实
LainVM 路径中通过；Meta 不直接访问 evaluator 的 memory 或执行状态。

## 8. 阶段六：多 TCB 和平台 lowering

只有单 TCB runtime、Endpoint、Trap 和 CSpace 在参考后端通过后，才进入这一阶段：

- 多 TCB 调度和多个 VSpace；
- native backend 的寄存器/栈 lowering；
- Linux/Windows 用户态同步和地址空间 lowering；
- 裸机页表、硬件栈帧和中断 lowering；
- 软件 MMU、demand paging 和平台特化。

所有 backend 共享同一 LainVM contract。平台差异只能出现在 lowering 和 provider 实现，
不能反向改变 LAINIR 的物理语义。

## 9. 与 compiler 主线的关系

LainVM 是当前主线。compiler 的类型诊断、source span、剩余 backend 指令和 CI 验证作为
并行维护线推进，不等待 LainVM 完成，也不阻塞第一个真实 scalar `#eval` runtime。

每个 VM 阶段都必须有：

1. 对象或状态转换 contract；
2. 至少一个真实 provider/runtime 实现；
3. 正常、越界、owner、quota 和 Trap fixture；
4. 与现有 native/formal matrix 的回归结果。

## 10. 当前实施顺序

按当前代码和验证结果，已完成的部分不再作为待办，当前顺序如下：

1. **已完成：单 TCB 执行入口收敛。** `execute`、`execute_limited` 和错误返回都经过
   同一个 LainVM root-TCB 路径。
2. **基本完成：VSpace release contract。** owner transfer/release、arena generation、
   旧 handle 失效、带 generation 的 result handle 和 backing arena 回收已经接入；recording
   provider fixture 已验证每次 reset/release 只释放一次 backing arena。后续继续验证
   result、arena address 和 activation storage 的失效。
3. **已完成：root scheduler slice。** seed runtime 的 opaque control plane 已接入真实
   interpreter；root frame、locals、activation、下一条 instruction、fuel、suspend/resume
   和 CallFrame 观察均有 fixture。当前限制是 nested call 在一个 slice 内原子执行。
4. **已完成：Endpoint ownership 与 Trap control plane。** Endpoint 等待只保存 TCB、owner
   和 payload；pending result 可唤醒 TCB；Trap 可记录、abort、由 scheduler 消费，并带有
   instruction/expression span 和实际 nested procedure。
5. **已完成：多 TCB evaluator handoff。** scheduler 已能把一个 current TCB 的真实
   evaluator slice 运行到 Endpoint 阻塞或完成，并按 attachment 顺序选择另一个 READY 或
   被唤醒的 TCB 接入同一个 `lainir_run` 入口；两个 Endpoint 的并行等待和 pending result
   隔离已有 fixture；两个发送方经过 nested helper 的并行 Endpoint wait 也已验证结果
   回到各自 frame。slice 结果到 scheduler 状态的统一转换、fuel-yield 轮转和严格
   round-robin 等待上界已有验证；v1 不引入 priority/weight。
6. **已完成首轮：nested continuation 的并行 pending-call。** 两个 TCB 的 nested Endpoint
   result 已有隔离 fixture，同一 root 的两层 sibling nested Endpoint wait 也已验证；取消、
   Endpoint 销毁和 TCB abort 的 wait 清理也已有验证，backend state destructor 已接入并由
   nested abort fixture 验证；两个结果返回后再 Trap 的 fixture 也已验证统一清理边界。
   继续补充不同表达式形状的恢复覆盖；fan-out/fan-in 仍需单独的 Endpoint 模型决策。
7. **当前阶段：长生命周期 VM contract 与平台 lowering 前置。** 先定义可复用 VSpace 的
   generation/reset contract、带 VSpace identity 和 generation 的 result handle 归属以及
   多 TCB/多 VSpace 生命周期边界（一个 TCB 的 finish 不得 reset 另一个 VSpace），并固定
   可复用 session 的显式 reset/release API，再进入 native、
   线程、用户态地址空间和裸机 lowering。多 TCB 调度仍沿用已固定的严格 round-robin，
   不在本阶段引入 priority/weight。
8. **后续：平台 lowering。** 在 nested continuation、单 TCB VSpace、
   Endpoint、Trap 和 CSpace contract 稳定后，才进入 native、线程、用户态地址空间和
   裸机 lowering。

当前 seed runtime 的已验证能力包括：owner 检查、READY/RUNNING/BLOCKED/DEAD、fuel
exhaustion、root continuation、nested frame observation、Endpoint rendezvous、pending
result、Trap abort/acknowledgement、源码定位，以及 scheduler 驱动的双 TCB evaluator
handoff、按 attachment 顺序的 runnable 选择、fuel-yield 轮转和多个 Endpoint wait 隔离。
v1 严格 round-robin 的等待上界已固定；通用并行 pending continuation 仍未完成。

当前基线命令：

```text
python scripts/check_lainir_provider_smoke.py
python scripts/check_lain_vm_contract.py
python scripts/check_lainc_lainir_api_baseline.py
```

三者均通过时，才允许推进下一项；任一失败都先修复当前 VM slice，再扩展对象模型。
