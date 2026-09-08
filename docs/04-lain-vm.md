# LAIN-VM：虚拟控制面与执行环境

状态：当前架构设计。`#eval` 的临时 TCB 执行路径尚在实现中。

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

## 3. VSpace

VSpace 管理一组执行流可以看到的地址区域和权限。`#eval` 默认创建临时 TCB，并让它使用调用者当前的 VSpace；它不会仅因进入编译期计算就另建地址空间。

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

VM control API 提供以下操作：

| 操作 | 语义 |
| --- | --- |
| `create_tcb(entry, vspace, cspace)` | 创建 TCB，绑定入口 procedure、VSpace 和 CSpace |
| `suspend_tcb(tcb, reason)` | 保存执行状态并将 TCB 置为 blocked 或 suspended |
| `resume_tcb(tcb)` | 检查 capability 和状态后，将 TCB 放入 runnable 集合 |
| `terminate_tcb(tcb)` | 结束 TCB，交付过程返回值或 Trap，并释放其 activation |

协程通过 scheduler request 或 Endpoint 操作主动让出执行。解释器保存宿主状态，
native backend 保存寄存器和栈状态，系统级 backend 使用平台线程或硬件上下文；这些
实现共享同一组 TCB 状态转换和生命周期规则。

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
  -> VM 创建共享调用者 VSpace 的临时 TCB
  -> 为临时 TCB 建立根 activation 和执行预算
  -> 执行 LAINIR
  -> 正常完成时返回声明的 LAINIR 物理值，失败时产生 Trap
  -> 结束临时 TCB 并释放它的 activation
```

临时 TCB 结束时不会销毁共享的 VSpace。返回值遵守普通 LAINIR 过程返回规则；其中若包含 `#addr`，地址的有效性仍由它所指向区域的生命周期决定。嵌套 `#eval` 依次创建临时 TCB，并继续使用同一个 VSpace。

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

实施阶段与验收条件统一记录在 [`roadmaps/lain-roadmap.md`](roadmaps/lain-roadmap.md)。当前首先删除旧的求值结果包装协议；随后固定临时 TCB、共享 VSpace、过程返回和 Trap 的接口，再分别接入 seed 解释器、Lain 编写的解释器和 Meta。
