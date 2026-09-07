# 04-LAIN-VM：虚拟微内核与执行环境

状态：架构提案 / 编译期与虚拟执行环境规范。实施阶段见
[`roadmaps/lain-vm.md`](roadmaps/lain-vm.md)；本文不是当前 LAINIR 实现规范。

当前迁移基线只实现并验证了命名的只读静态 `#data`、当前 procedure activation
内的 `#alloca`、`#lea` 与 typed `#load/#store`、Eval 的 step/depth/allocation
限制，以及显式传入的 capability。下文的 VSpace、TCB、Endpoint、Trap、CSpace、
软件 MMU、`#swap_context`、demand paging 和 OS/bare-metal lowering 都是未来设计
方向；它们尚未成为 LAINIR 指令、provider API 或 seed 运行时对象。任何实现工作
必须先更新 LAINIR 规范和 API contract，再把这些提案纳入 provider 与测试。

### 审阅结论

这份文档适合作为长期架构提案，暂时不应作为 LAINIR 或标准库的实现承诺。它
把三个层次放在了一张图里：`#eval` 的受限执行、未来的运行时调度抽象、以及
操作系统或裸机的物理下沉。当前 roadmap 只接受第一层已经存在的约束；第二、
三层必须分别形成 API contract、可运行 provider 和验证用例后才能进入实现。

因此，本文中的对象名和操作名（例如 `VSpace`、`TCB`、`Endpoint`、`Trap`、
`CSpace`、`#swap_context`）均为保留的设计词汇，不是当前可编写的 Lain 语法。
尤其是 `#data` 的只读语义、`#alloca` 的 activation 生命周期、capability 的
显式传入和 Eval 配额，应继续以 LAINIR 与 API 文档为准；本文不重新定义它们。

LAIN-VM 是 Lain 系统中的**虚拟控制面与微内核抽象（Virtual Microkernel Control Plane）**。它不作为运行时的重量级虚拟机（如 JVM 或 BEAM）存在，而是为编译期计算（`#eval`）、解释器自举、以及运行期特权降级提供一套严格正交的物理环境模型。

LAIN-VM 继承 **seL4 / Mach 的极简微内核哲学**，坚决摒弃宏内核（Monolithic Kernel）将调度、内存、权限和文件强行绑定的混乱设计。LAIN-VM 将系统的物理控制权严格拆解为五个正交的物理基石：**空间（VSpace）**、**执行上下文（TCB）**、**同步端点（Endpoint）**、**异常控制（Trap）** 与 **能力（CSpace）**。

在最终的代码生成中，LAIN-VM 的抽象在编译期被完全擦除（Zero-Overhead Erasure），直接下沉为目标硬件指令或宿主操作系统的系统调用。

---

## 1. 架构总览与五大正交基石

LAIN-VM 拒绝类似 Linux `task_struct` 的“全能任务”设计，内核只管理五种正交物理对象：

```text
       ┌─────────────────────────────────────────────────────────────┐
       │                           LAIN-VM                           │
       └───┬─────────────┬─────────────┬─────────────┬─────────────┬─┘
           │             │             │             │             │
           ▼             ▼             ▼             ▼             ▼
     【 VSpace 】   【  TCB  】   【Endpoint】   【  Trap  】   【 CSpace 】
     - 连续无类型   - 寄存器现场  - 同步会合点   - 硬件级中断  - 不可伪造Token
     - 软件 MMU     - 栈空间与SP  - 阻塞与唤醒   - 断言违例    - 拦截 #extern
     - 瞬时 Arena   - 协同换栈    - 多核数据交接 - 诊断升格    - 编译期沙盒
```

1. **VSpace（虚拟内存空间）**：纯粹的资源容器。负责划定物理地址范围、访问权限及生命周期。VSpace 不具备执行能力。
2. **TCB（线程控制块）**：纯粹的动态执行现场。包含指令指针（IP）、栈指针（SP）和通用寄存器快照。TCB 不拥有内存，它只在一个指定的 VSpace 内运行。
3. **Endpoint（同步端点）**：TCB 之间进行**零拷贝同步与会合（Rendezvous）**的纯粹门牌号，解决执行流阻塞与多核交接。
4. **Trap（异常与中断分发）**：硬件级故障、只读越界、断言违例及外部物理 IRQ 的拦截中枢。
5. **CSpace（能力空间）**：基于权标（Capability）的权限控制表，决定当前执行流可以向宿主请求哪些特权操作。

---

## 2. 虚拟内存空间（VSpace）与软件 MMU

每个 `#eval` 任务或虚拟进程都在一个独立的 VSpace 内运行。VSpace 对标 seL4 的 Untyped Memory 与地址空间模型：

### 2.1 基于 Scoped Arena 的瞬时内存回收
- VSpace 向宿主申请一段连续的物理内存作为内部存储池；
- 空间内的静态 `#data` 与过程局部 `#alloca` 均通过线性指针推进（Bump Allocation）进行分配；
- **生命周期清算**：当一次 `#eval` 结束时，LAIN-VM 仅将最终的计算结果（字面量或紧凑对象）复制出沙盒，随后**直接重置 VSpace 的分配指针（Reset）**，实现 $O(1)$ 复杂度的瞬时物理内存回收，彻底杜绝编译期内存泄漏。

### 2.2 虚拟缺页与软件 MMU（Virtual PageFault）
LAINIR 的物理指令（`#load` / `#store`）保持对虚拟内存机制的无知，而 LAIN-VM 充当软件 MMU 守门人：
- **只读保护拦截**：当 `#store` 试图写入标记为只读的 `#data` 地址段时，LAIN-VM 触发写保护异常（Write-Protection Fault）；
- **哨兵页与防穿透（Guard Page）**：在 VSpace 分配的栈底设置不可访问的虚拟哨兵页，当栈指针 `%sp` 发生越界碰撞时立即拦截，防止宿主编译器崩溃（Segfault）；
- **按需分配（Demand Paging）**：对于编译期声明的大块稀疏内存，LAIN-VM 仅在首次写入触发缺页拦截时按需向宿主申请物理页，抑制编译期内存膨胀。

---

## 3. 虚拟执行流（TCB）与上下文切换

LAIN-VM 是上层代数效应（Algebraic Effects）、协程及并发模型在物理底层的承载者。

### 3.1 物理 TCB 结构
TCB 是一个极简的定长物理状态结构体，描述一段独立的计算历史：

```lain-ir
#data virtual_tcb {
  #bits<64> %virtual_ip,       // 当前程序计数器
  #addr     %virtual_sp,       // 当前栈指针
  #addr     %stack_limit,      // 栈溢出检查边界
  #bits<64> %registers[16],    // 虚拟通用寄存器现场
  #bits<32> %status            // READY, RUNNING, BLOCKED, DEAD
}
```

### 3.2 纯软件换栈（#swap_context）
在编译期执行 `#eval` 时，LAIN-VM 避免调用宿主机的物理汇编换栈指令：
- 当遇到 `#swap_context(old_tcb_addr, new_tcb_addr)` 时，LAIN-VM 拦截该物理操作；
- VM 将解释器当前的寄存器与状态写入 `old_tcb_addr`；
- VM 将内部的活跃 TCB 指针切换为 `new_tcb_addr`，并从该 TCB 恢复寄存器快照与 `%virtual_ip`；
- 解释器继续单线程推进，**无需触碰任何宿主机的物理调用栈**，从而在编译期安全原生跑通代数效应与纤程（Fiber）。

---

## 4. 同步与会合端点（Endpoint）

针对非协同式的多执行流交接、跨核并行或阻塞等待，LAIN-VM 提供类似 seL4 的原子同步原语：

1. **会合语义（Rendezvous IPC）**：
   - Endpoint 本身不持有消息队列，它只是一个**没有缓冲区的同步挂起点**；
   - 当 TCB-A 执行 `Endpoint.Send(ep, data)` 而无接收者时，TCB-A 状态置为 `BLOCKED`，进入等待队列；
   - 当 TCB-B 执行 `Endpoint.Recv(ep)` 到达时，两者瞬间完成寄存器级数据交接，双双激活。
2. **多核与并发支持**：
   - 彻底解耦“直接点名换栈（`#swap_context`）”带来的强依赖，为标准库实现 Mutex、Channel 和任务池调度器提供最底层的无锁阻塞支撑。

---

## 5. 故障、断言与编译器诊断（Trap & Diagnostics）

LAIN-VM 充当虚拟 CPU 的中断向量控制器，严禁将未定义行为（UB）或非法物理操作泄露给宿主机。

1. **硬件级 Trap 捕获**：
   - 算术除零（Division by Zero）；
   * 空间未对齐访问（Alignment Fault）；
   * 栈逃逸（Activation Escaping，即检查到 `#alloca` 地址被返回或脱离作用域）；
   * 外部物理硬件中断（IRQ，作为异步 Trap 注入）。
2. **断言消费与验证**：
   - LAINIR 中附着在 `#addr` 上的几何断言（如 `#assert_not_null`、`#assert_align`）由 VM 进行运行时断言；
   - 一旦断言失败，立即触发断言违例 Trap。
3. **诊断提升（Diagnostic Reflection）**：
   - 发生 Trap 时，LAIN-VM 封冻当前 TCB 的状态快照，解析 `%virtual_ip` 对应的源码调试信息映射（Source Map）；
   - 将底层段错误或断言失败，升格为优雅、精准的编译期报错：
     `"Comptime Trap: [Memory Access Fault] attempted to dereference null address at src/parser.lain:88:12"`。

---

## 6. 确定性与资源配额（Fuel & Quotas）

为了保证编译期计算图灵完备性下的**有界终止（Bounded Termination）**，LAIN-VM 内置虚拟时钟与资源度量机制：

- **指令燃料（Fuel Counter）**：
  每次 `#eval` 启动时被注入确定的 Step 预算（例如 $1,000,000$ 步）。解释器每执行一次分支、循环回跳或内存操作消耗对应燃料。燃料耗尽强制中断，杜绝编译期死循环；
- **调用栈深度（Call Depth Limit）**：
  限制嵌套 `#call` 的最大层级，提前拦截无限递归；
- **分配预算（Allocation Quota）**：
  限制单个 Task 内累积申请的内存上限，防范恶意宏引发的宿主 OOM。

---

## 7. 关于时间与异步事件的非侵入设计（Non-intrusive Time Architecture）

LAIN-VM **严禁内置任何时钟（Clock）、定时器或异步事件循环（Event Loop）机制**。所有时间流逝与异步 IO 必须在标准库层通过代数效应自愈：

1. **时间的物理本质**：
   - 获取时间戳被降级为读取硬件寄存器（如 x86 `RDTSC` / ARM `CNTVCT`），作为普通指令或外部符号处理；
2. **异步事件的组合消除**：
   - 外部硬件中断被统一视为异步 **Trap** 捕获；
   - 唤醒挂起任务统一通过 **Endpoint** 投递；
   - 标准库通过 `effect Clock` 暴露 `now()` 与 `sleep()` 接口。生产环境对接物理定时器中断；单元测试与 `#eval` 中可直接注入虚拟步进时钟（Virtual Time），实现确定性瞬间快进测试。

---

## 8. 能力访问控制（CSpace & Capability）

根据 Lain 核心设计准则，任何环境交互必须通过显式能力授权。

- **调用隔离**：解释器内部遇到 `#extern` 过程调用时，无权直接链接宿主符号；
- **调用网关（Gateway Dispatcher）**：
  外部调用被转换为向 LAIN-VM 提出的能力调用请求（Capability Invocation）。
  LAIN-VM 检索当前 Task 的 CSpace：
  - 若具有 `Cap::FsRead` 权标，转发给宿主执行文件读取；
  - 若缺乏对应权标，当场终止执行并抛出安全越权异常；
- **构建无菌性**：默认情况下，任何 `#eval` 任务拥有空的 CSpace，数学上保证宏展开与编译期特化完全纯粹、跨机器可复现、且免疫任何供应链恶意代码投毒。

---

## 9. `#eval` 的标准生命周期

当编译管线遇到一个 `#eval { ... }` 块时，LAIN-VM 驱动其完整的沙盒生命周期：

```text
[进入 #eval]
    │
    ▼
1. VSpace 实例化 ───► 向宿主借用连续空间，划定 Arena 与只读段
    │
    ▼
2. TCB 与 CSpace ───► 创建根 TCB，配置 %sp 指向栈顶，注入授权 Token
    │
    ▼
3. 驱动解释执行 ───► LAIN-IR 解释器接管取指，消耗 Fuel，处理换栈与 Endpoint
    │
    ▼
4. 结果萃取 ───────► 拦截正常退出，从栈顶或寄存器提取目标 #bits<N>
    │
    ▼
5. 资源清盘 ───────► 强制释放/重置整个 VSpace，销毁 TCB
    │
    ▼
[返回常量，折叠进外部静态 IR]
```

---

## 10. 运行时特化与物理下沉（Lowering）

LAIN-VM 虽是编译期的控制中枢，但它同时定义了**代码如何面向操作系统下沉**的抽象模型：

| LAIN-VM 概念 | 编译期执行 (#eval) 的实现 | 用户态系统 (Linux/OS) 的 Lowering | 裸机环境 (Bare-Metal) 的 Lowering |
| :--- | :--- | :--- | :--- |
| **VSpace** | 内存 Arena / 宿主堆池 | `mmap` / 虚拟内存段 (VMA) | 物理页表基址 (`CR3` / `SATP`) |
| **TCB** | 结构体快照 + 解释器循环 | 用户态汇编换栈 / OS 线程池 | 物理硬件栈帧 / `IRET` 现场 |
| **Endpoint** | 解释器内部挂起队列 / 寄存器换入 | Linux `futex` / Windows `WaitOnAddress` | 硬件原子指令 (CAS) + 自旋锁 / `WFI` |
| **Trap** | 解释器拦截升格为 Diagnostic | 注册 OS 信号处理 (`SIGSEGV` / `SIGFPE`) | 硬件中断向量表 (IDT / IVT) 处理函数 |
| **CSpace** | 虚拟 Token 查找表 | 文件句柄 (fd) / 操作权限鉴权 | 硬件特权级 (Ring 0 / PMP 物理内存保护) |

在将 LAINIR 降低为本机机器码时，**LAIN-VM 作为实体完全消解**，它在上层建立的控制面契约直接转化为硬件指令或最小系统调用，实现真正意义上的**零运行时抽象开销（Zero-Overhead Abstraction）**。
