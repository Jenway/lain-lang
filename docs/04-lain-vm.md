# LAIN-VM：虚拟控制面与执行环境

状态：当前架构设计与 C3 实现。C seed 已通过临时 TCB 执行 `#eval`；Lain VM 的解释器、
根过程执行入口和子 TCB 执行入口已归入 `src/lainvm/`。Meta 到该入口的重新接线属于 C4。

职责划分（2026-09-12 校准）：`#eval` 是 LAINIR 的概念，它命名「这段已 lowering 的 IR 在
编译期执行」。LAIN-VM 不拥有 `Eval` effect，只提供实现它所需的执行原语。effect 的实现
策略（TCB 或 CPS）是编译期决策，见 [`stdlib/effect-system.md`](stdlib/effect-system.md)。

LAIN-VM 是 Lain 的执行控制面。它为编译期 `#eval`、解释器自举、运行期特权降级和未来的并发执行提供统一的物理环境模型。

LAIN-VM 与 LAINIR 分层：LAINIR 描述已经确定尺寸、调用约定和内存访问方式的物理程序；LAIN-VM 管理这些程序运行时的地址空间、执行上下文、同步、异常和能力。LAIN-VM 不替代 LAINIR，也不向 LAINIR 引入源语言类型或对象布局。

## 1. 控制面对象

LAIN-VM 由五类正交对象组成：

| 对象 | 职责 |
| --- | --- |
| VSpace | 地址空间、区域和访问权限 |
| TCB | 一个执行流的 procedure、instruction position、activation、限制和状态 |
| Endpoint | TCB 之间的同步和结果交接 |
| Trap | 非法物理操作、资源耗尽、断言和外部事件的统一出口 |
| CSpace | 当前执行流可使用的 capability 集合 |

VSpace 不负责执行；TCB 不拥有地址空间；Endpoint 不拥有执行流；Trap 不代替诊断系统；CSpace 不改变 LAINIR 指令的含义。对象之间通过 VM contract 组合。

这些对象是 VM 的逻辑对象。它们可以由解释器中的宿主结构体、native backend 中的寄存器和栈、或操作系统中的线程和地址空间实现。它们不是 LAINIR 的 struct、record 或 `#data` 声明。

这五类对象目前不在同一成熟度上：

| 对象 | 现状 |
| --- | --- |
| VSpace、TCB、Trap、CSpace | 已实现且被消费；C seed 的 `#eval` 路径与 `src/lainvm/interpreter.lain` 都在使用 |
| Endpoint、scheduler、TCB 挂起/恢复 | 已设计、未激活；Lain 实现中没有 Endpoint 与 scheduler，`suspend_tcb`/`resume_tcb`/`run_slice` 都没有调用者 |

原因是 `#eval` 是同步的（§8.1），当前没有任何 handler 需要挂起执行。挂起机制的消费者是
具有非平凡 `resume` 的 handler；在这类 handler 出现之前，Endpoint 与调度不属于契约的
活跃部分，不应据此认为 Lain 实现"缺了"它们。

## 2. 与 LAINIR 的边界

LAINIR 当前只表达物理值和物理操作：

```lain-ir
#bits<N>
#float<N>
#addr
#unit
#never
```

静态数据使用只读 `#data`，activation 局部存储使用 `#alloca`，地址计算使用 `#lea`，访问使用带宽度的 `#load/#store`。这些定义见 [`01-lain-ir.md`](01-lain-ir.md)。

LAIN-VM 为这些操作提供执行环境：

- 将 `#data` 映射为当前 VSpace 中的只读区域；
- 将 `#alloca` 绑定到当前 TCB 的 activation 生命周期；
- 检查地址是否属于当前 VSpace 和当前 activation；
- 将非法访问和资源耗尽转为 Trap；
- 通过 CSpace 决定 `#extern` 等外部能力是否可用。

TCB、VSpace、Endpoint、Trap 和 CSpace 不属于当前 LAINIR v1 指令集。需要向 Lain 或标准库公开 VM 控制操作时，必须先定义 VM API、权限、生命周期和 backend lowering。

`#eval` 属于 LAINIR 语义：LAINIR 定义「这段已 lowering 的代码在编译期执行」这一概念并
验证其静态类型。LAIN-VM 提供执行它所需的原语——过程入口、VSpace、预算与 Trap——但不
定义 `Eval` effect，也不参与 `#eval` 的语义判定。

编译期地址由 VM 的 VSpace、activation 和 capability 边界管理。Meta 需要引用
AST 与语义对象，但对象协议不要求这些引用必须是地址。若某实现以 `#addr`
承载对象 handle，VM 执行接口须按生命周期交付该值；调用必须有 LAINIR 可表述的
VM 边界，不能从高层语言绕过 LAINIR，也不能借 `#eval` 的产物结果协议完成。

## 3. VSpace

VSpace 管理地址区域和访问权限，独立于 TCB。TCB 本身没有 VSpace，也不因执行
`#eval` 自动取得调用者的地址空间。执行请求须显式提供允许使用的地址值和相应的
内存能力；没有这些输入时，临时 TCB 没有可用的外部地址。

### 3.1 区域

- `#data` 是静态只读区域。它只保存物理字节和对齐信息，没有源语言字符串、对象或模块语义。
- `#alloca` 属于当前 procedure activation。activation 结束后，其中的地址立即失效，不能通过返回值、全局状态、Endpoint 或 capability 逃逸。
- procedure 和外部 capability 通过 VM 的对象表或 provider 句柄关联，不使用伪造的 LAINIR 结构体地址。

### 3.2 地址检查

VSpace 在 `#lea`、`#load` 和 `#store` 的执行路径上提供边界和权限检查。至少需要拒绝：

- 写入只读 `#data`；
- 从 activation 外访问 `#alloca`；
- 越过区域边界的 load/store；
- 使用已经结束的 activation 或已经撤销的 VM 句柄。

具体实现可以使用连续 arena、保护页、软件边界表或宿主虚拟内存，但这些实现选择不改变 VM contract。

## 4. TCB

TCB 描述一段独立的执行历史。它至少保存以下逻辑状态：

| 状态 | 含义 |
| --- | --- |
| procedure / instruction position | 当前执行的过程和指令位置 |
| activation state | 当前调用 activation、局部存储和返回边界 |
| execution limits | step、call-depth、allocation quota 的预算和消耗 |
| capability context | 当前 TCB 使用的 CSpace |
| status | ready、running、blocked 或 dead |
| trap / return state | 挂起的 Trap 和过程返回状态 |

当前实现不规定这些字段的物理布局。解释器可以保存宿主结构，native backend 可以使用寄存器和栈，系统级 lowering 可以使用硬件线程现场。任何表示都必须保持相同的 TCB 生命周期、过程返回和 Trap 语义。

### 4.1 上下文切换

上下文切换由 VM scheduler 驱动。TCB 对 LAINIR 保持 opaque，切换过程不会把 TCB
地址作为物理值传入程序。

VM control API 的顶层操作草案如下；名称、参数表示和 LAINIR 调用 ABI 尚未固化：

| 操作 | 语义 |
| --- | --- |
| `create_tcb(entry)` | 创建 TCB 并指定入口 procedure；TCB 不自带 VSpace |
| `start_tcb(tcb, execution_request)` | 用独立的执行请求提供参数、内存能力和 capability 授权后开始执行 |
| `suspend_tcb(tcb, reason)` | 保存执行状态并将 TCB 置为 blocked 或 suspended |
| `resume_tcb(tcb)` | 检查 capability 和状态后，将 TCB 放入 runnable 集合 |
| `terminate_tcb(tcb)` | 结束 TCB，交付过程返回值或 Trap，并释放其 activation |

协程通过 scheduler request 或 Endpoint 操作主动让出执行。解释器保存宿主状态，
native backend 保存寄存器和栈状态，系统级 backend 使用平台线程或硬件上下文；这些
实现共享同一组 TCB 状态转换和生命周期规则。

### 4.2 编译器执行接口

编译器通过四类值请求执行：已验证的 `Artifact`、其中的 `Procedure`、物理参数序列和
过程声明的普通物理返回值。一次调用创建执行 TCB；失败产生 Trap。接口不公开 TCB、
解释器栈或 VSpace 的实现对象。

C seed 给手写 bootstrap 提供同一语义的句柄接口：解析并验证 artifact、按名称取得
procedure、追加 `#bits<N>` 或 `#addr` 参数、执行过程。正式 Lain 实现通过
`src/lainvm/api_contract.lain` 表达相同边界。模块、类型或 AST 若以 `#addr` 返回，地址
所指数据仍由 Meta 解释，LAINVM 不附加分类。

## 5. Endpoint

Endpoint 是 TCB 之间的同步会合点。它不改变 LAINIR 的值语义，也不承担通用消息队列。

基本语义如下：

1. 发送方到达而没有接收方时，发送方 TCB 进入 `blocked`；
2. 接收方到达时，VM 按 contract 完成交接，双方恢复为可运行状态；
3. 交接的数据必须经过 capability 和生命周期检查，不能把 activation 地址变成长期对象；
4. Endpoint 的等待、唤醒和取消都必须能产生稳定的状态转换或 Trap。

解释器可以使用内部等待表，native backend 可以降低到宿主同步原语，裸机实现可以使用原子指令和中断。它们共享同一 Endpoint contract。

## 6. Trap

Trap 是 VM 对非法物理操作和资源边界的统一响应。至少包括：

- 除零、未对齐访问和越界访问；
- 写入只读 `#data`；
- activation 地址逃逸和 use-after-return；
- step、call-depth 或 allocation quota 耗尽；
- capability 拒绝；
- Endpoint 或上下文切换的非法状态；
- provider 或宿主注入的外部事件。

Trap 至少保留 TCB、procedure、instruction position、错误分类和必要的 source location。编译器可以把 Trap 转换为诊断，native backend 可以把它转换为平台异常；Trap 的物理来源不能直接泄露为宿主崩溃。

## 7. CSpace 与外部能力

执行流不能直接访问宿主文件系统、时钟、网络或其他外部资源。`#extern` 的解析和调用必须经过当前 TCB 的 CSpace。

默认 `#eval` 临时 TCB 使用受限的 capability 集合。调用外部能力时，VM 检查：

- 当前 TCB 是否拥有相应 capability；
- capability 是否允许当前 TCB 使用；
- 参数中的地址是否仍在允许的 VSpace 范围内；
- 调用完成后是否产生合法的物理结果。

拒绝访问产生 Trap，不产生部分 artifact，也不允许通过宿主异常绕过 CSpace。

## 8. `#eval` 生命周期

一次 `#eval` 的最小生命周期是：

```text
调用者 TCB 执行 #eval
  -> VM 创建不自带 VSpace 的临时 TCB
  -> 显式传入捕获值及获授权的执行能力
  -> 为临时 TCB 建立根 activation 和执行预算
  -> 执行 LAINIR
  -> 正常完成时返回声明的 LAINIR 物理值，失败时产生 Trap
  -> 结束临时 TCB 并释放它的 activation
```

临时 TCB 结束时不会销毁外部提供的 VSpace。VM 内部执行结果若包含 `#addr`，
其有效性仍由所指区域的生命周期决定；这并不授权求值阶段把编译期地址写入产物。
嵌套 `#eval` 也只能使用显式传递且获授权的地址及能力。

### 8.1 捕获、根过程与返回

`#eval` 是同步操作。它的块可以引用外围 `%local`，但临时 TCB 不借用调用者的解释器
frame 或调用栈。compiler lowering 收集块的自由局部绑定，按词法绑定把它们作为值传给
临时根过程。根过程的静态返回类型就是 `#eval` 表达式的已验证类型。

传入 `#addr` 时仅复制地址值。父 TCB 在子 TCB 同步运行期间仍保持其 activation；子 TCB
结束时释放自己的 activation。VM 执行接口不能交付子 TCB 已释放的 `#alloca` 地址；
仍存活的地址遵守原有 VSpace 规则。地址的编译期交付和 `#eval` 的产物结果是两份契约。

### 8.2 预算与 capability

最外层 `#eval` 建立一个预算账户。它的临时 TCB 和全部嵌套 `#eval` TCB 共同消耗其中的
step 与 allocation 配额；嵌套计算不能重新获得完整限额。call-depth 从外层调用深度连续
计数，进入每个临时根过程增加一层。任一账户耗尽立刻产生 quota Trap。

临时 TCB 不隐式继承调用者 VSpace。编译请求须为执行显式提供内存能力与调用者
拥有的 capability 子集；嵌套 TCB 不能扩大这些授权。外部调用和地址访问按
显式传入的能力与地址有效性检查。

### 8.3 Trap 传播

子 TCB 正常结束时只交付普通 LAINIR 值。发生 Trap 时，VM 记录错误分类、procedure 和
instruction position，结束子 TCB 并释放其 activation；随后把 Trap 同步传播到执行
`#eval` 的调用点。调用点没有备用值，编译器把 Trap 转换为诊断。Trap 不作为 `#eval`
表达式的字段、`Result` 对象或 Meta handle 返回。

### 8.4 Lain VM 的执行入口

Lain 实现将 `interpreter.lain` 置于 `src/lainvm/`。它提供根过程执行入口，以及
`execute_child(state, procedure, arguments)`：后者要求调用者 TCB 正在运行，创建新的
子 TCB，保留原 VSpace、CSpace、step 和 allocation 账户，并把 `arguments` 作为临时根过程
的按值参数。子 TCB 结束后恢复父 TCB；其 activation 地址仍不能逃逸。

这组入口的公开结果只有物理 `Value` 或 `Trap` effect。解释器内部可以使用控制流记录来
组织 return、break、yield 与 Trap，但该记录不属于 LAINVM API，也不会进入 `#eval` 的
结果语义。

编译器中的 Meta 不直接调用带 `state` 参数的入口。

`Eval` 是 LAINIR 层的 effect，命名「这段已验证的 IR 在编译期执行」。LAINVM 不定义它，
只提供实现它所需的执行原语：`execute` / `execute_child`，外加 VSpace、预算与 Trap。
handler 由编译器边界安装——它取活动 TCB，调用 `execute_child`，并 `resume` 得到的普通
`Value`。因此 Meta 只能够 `perform` 该 operation，无法构造、保存或传递 TCB 与 VSpace。

`src/lainvm/interpreter.lain` 目前把 `Eval` effect 与 `eval_handler` 定义在 VM 模块内部，
这是待迁移的过渡状态：二者应随 LAINIR 契约移动，VM 只保留执行原语。`eval_handler` 签名
上的 `&mut LainVm` 参数是「本 handler 的恢复需要 TCB 机制」这一事实的未成型替身。

## 9. 确定性与时间

编译期执行需要确定的 step、call-depth 和 allocation 预算。相同 artifact、输入和 capability 集合应得到相同的结果或相同的 Trap 分类。

LAIN-VM 不内置业务时钟、定时器或事件循环。时间和异步 IO 通过显式 capability、Endpoint 和标准库 effect 接入；`#eval` 可以注入虚拟时间或完全拒绝相关能力，以保持可复现性。

## 10. 平台 lowering

同一 VM contract 可以有不同的执行后端：

| VM 对象 | 参考解释器 | 用户态系统 | 裸机 |
| --- | --- | --- | --- |
| VSpace | 受检的 arena/区域表 | 虚拟内存区域 | 页表和物理保护 |
| TCB | 宿主状态对象 | 线程或用户态上下文 | 硬件栈帧 |
| Endpoint | 等待表 | futex 或平台同步原语 | 原子指令和中断 |
| Trap | 解释器错误结果 | 信号/异常转换 | 中断向量 |
| CSpace | capability 表 | 句柄和权限表 | 特权级和物理保护 |

LAIN-VM 的对象在最终 lowering 中可以被消除，但对象之间的权限、生命周期和 Trap 关系必须保留。零开销是 lowering 的目标，不是省略 VM contract 的理由。

## 11. 实施顺序

实施阶段与验收条件统一记录在 [`roadmaps/lain-roadmap.md`](roadmaps/lain-roadmap.md)。

当前边界校准（2026-09-12）：

- `Eval` 与 `eval_handler` 归 LAINIR 契约，LAINVM 只保留执行原语；
- LAINVM 的活跃契约收窄为「执行已验证的 LAINIR，并守住 VSpace、Trap 与预算」；
- Endpoint、scheduler 与 TCB 挂起链，在出现非平凡 `resume` 的 handler 之前不激活；
- CPS 策略不涉及 VM 操作——变换之后没有 VM 参与，故 LAINVM 只实现 TCB 一条路径。
