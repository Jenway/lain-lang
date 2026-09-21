# VM 契约

VM 执行已验证的 LAINIR。本文区分已明确的职责与尚待确认的地址、回收和预算规则。
实现缺口和复现方案见 [VSpace 实施方案](../implementation/vspace-action-plan-astra.md)。

## 对象与职责

| 对象 | 职责 |
| --- | --- |
| VSpace | 记录可访问区域、权限和区域有效性 |
| TCB | 保存一条执行流的指令位置、调用状态、结果和故障 |
| CSpace | 提供执行流可使用的能力；现有宿主调用能力与待接入的内存能力见下文 |
| Trap | 记录执行失败的分类、状态码和位置 |
| Endpoint | 执行流间通信的设计对象；其可用范围需单独验收 |

TCB 引用 VSpace，多个 TCB 可以共享一个地址空间。TCB 对 LAINIR 保持不透明，
不能作为程序可读写的控制结构交付。栈的供应、登记、借用与释放分工仍受下文决策约束。

## 执行与权限

执行请求显式提供物理参数、地址授权和宿主能力。没有授权的外部地址不可访问。
读、写和间接调用分别需要相应权限；检查覆盖整个访问范围。
宿主能力进行内存访问时同样需要验证参数范围、对象身份和生命周期，不能绕过执行环境的授权。
这一要求尚有实现缺口。

物理执行失败交付 Trap，编译器将编译期失败转为诊断。
切片耗尽表示本次调度额度结束，不等价于共享总预算已经耗尽。
执行终态和稳定诊断码是验收对象，宿主崩溃不能作为合法失败结果。

## 生命周期与预算要求

**2026-09-21 边界（作者确认，权威）**：

- `#addr` 是**无类型裸物理地址**：只表示地址数值，**不携带**对象身份、generation 或
  pointee type；单字宽（`sizeof(void*)`），落内存就是一个字。
- **VSpace 管物理内存**：区域、分配、回收、映射与 READ/WRITE/CALL 权限。
- **CSpace 管宿主服务能力**。不要把每一个 `#addr` 都改造成 capability。
- `ref(T)`、borrow、对象身份与**对象级**时间安全属于 Lain/Meta 层。需要动态检查时，
  Meta 负责把 `ref(T)` 降成 handle + generation + offset 等物理值；候选实现见
  [内存能力交接](../implementation/vm-memory-capability.md) 里的 **checked-ref 模型**，
  它**不是** `#addr` 的规范。
- `#ptr2int` / `#int2ptr` 只做**位模式转换**；`#int2ptr` **不授予**访问权限，
  实际访问继续查当前 VSpace。

所以「同址复用后旧裸地址必须失败」**不是** VM 对 `#addr` 的契约 —— 那条要求只适用于
高层 `ref(T)` 的物理表示。裸地址的行为是：

- 过程结束、且对应 VSpace 区域已撤销时，旧地址访问**拒绝**（实测拒 1004）；
- 同一数值地址后来被 VSpace 重新分配并授权时，旧裸 `#addr` 与新裸 `#addr`
  **不可区分**（实测：拿第一次留下的地址读到了新分配写进去的值）；
- 安全 Lain 程序靠 Meta 的 `ref(T)` 生命周期规则让这种旧引用**不存在**。

实现方式：栈按**租约**预留（记 base/size/所属空间，**不整块登记区段**——整块登记会让
返回后的旧地址仍在授权范围内），授权跟着**活窗口** `[base, base + 水位)` 走：
水位回退（返回 / `#break` / `#continue`）时窗口收回，`#alloca` 时重排，Trap 后由销毁回收。
窗口登记/撤销失败 → 拒 **1036**。

## 配额

**2026-09-21 确认按"实际承诺字节"落地**（§4/:105、§6/:162、§8.2/:212-214）：

- 单位**字节**；账户由**一次最外层编译期执行**创建，嵌套执行共享同一个账户；
- 扣费点在 VSpace **实际申请或承诺**一块底层存储时（本版为 TCB 栈整块预留），
  以及宿主暂存区与输出扩容；归还发生在**真的释放**该存储时；
- 栈整块预留只在预留那一刻扣一次，其中的 `#alloca` 子分配**不重复扣**；
  只回退水位**不归还**整块额度；
- 预扣是**原子**的：余额不足或加法溢出 → 拒 **1044**，账目一点不动；
  预扣成功而底层分配失败必须回滚；
- 切换 VSpace 不创建新账户、也不恢复额度。

## 稳定诊断码（本文件是权威登记）

| 码 | 含义 |
| --- | --- |
| 1004 | load 的目标范围不在当前 VSpace 的授权区域/权限内 |
| 1005 | store 的目标范围不在当前 VSpace 的授权区域/权限内 |
| 1035 | `#alloca` 尺寸算术回绕（count×element、水位对齐、水位相加） |
| 1036 | 栈活窗口登记/撤销失败（区段表满或上界不可表示） |
| 1044 | allocation quota 用尽（余额不足或加法溢出），账目不变 |
| 1045 | 执行期遇到 `#eval` 块（它必须在进入执行前被折叠掉；走到这里是管线漏了一步） |

验证器段 2xxx 的权威登记在 `seed/include/lainir/verify.h`（含 2030：`#eval` 块的结果
类型含 `#addr`——编译期地址不进产物）。

## 尚待确认的决策

| 项目 | 必须确定的内容 |
| --- | --- |
| 栈供应 | 已批准由供给方分配、登记和释放，TCB 借用；租约接口与结束借用规则待落实（本版供给方仍是 TCB 自己） |
| 撤销 | 区段身份与精确撤销已实现（句柄 = 空间 + 槽 + 代数）；能力派生树与通用委派仍待定 |
| 地址失效 | **已定**：`#addr` 是裸地址；失效靠 VSpace 撤销活窗口，同址重授权后不可区分；对象级失效归 Meta 的 `ref(T)` |
| lea | **已定**：纯地址算术，构造不查、访问查，按地址宽度取模；尾后地址可构造、访问时拒 |
| 能力调用 | 已批准统一受检边界（宿主先授权后访问）；可信签名、CSAPCE 索引与通用委派待落实 |
| 配额 | **已实现**：按实际承诺字节、账户跟执行走、拒 1044；宿主扩容与栈整块已在扣费点上 |

完整候选与验收见实施方案中的 D1–D5。
修改 `seed/src/vm/**` 前须取得相应决策确认；已有确认不重复请求。

## 后端约束

解释器与本机后端应分别说明如何实现同一权限、生命周期和失败契约。
仅在解释器中加入检查，不能证明生成的 C 获得相同保证。
若某后端仅支持受信任程序，应明确其范围，不以“零开销”省略契约说明。

**当前两端的 `#addr` 值语义一致**（2026-09-21 核实，选甲）：

- 解释器：`L1Value` 只有 `ADDR` 一种地址，载荷是单个指针；`type_size(#addr) = sizeof(void*)`；
  `#lea` 是无符号 64 位算术（回绕是定义好的）；`#ptr2int`/`#int2ptr` 是位模式转换。
- C 后端（`seed/src/backend/cbackend.c`）：`TY_ADDR` 宽度 64，load/store 按 `uintptr_t`
  单字读写，`#lea` 同样用 `uintptr_t` 算术（**不是**指针算术，避开 C 的 UB 面），
  `#int2ptr`/`#ptr2int` 同为一个 case。
- 差异**不在地址值语义**，而在**检查**：解释器每次访问查当前 VSpace、并在承诺存储时
  过配额；生成的 C 程序目前**没有**任何运行时边界层（`cbackend.c` 里对
  `lainvm_space_*` / `lainvm_quota_*` / `LAINVM_MEM_*` 的引用为 0），`#alloca` 发射成
  静态存储槽而不是运行时栈分配。
- 因此**不要**把两者描述成"解释器安全 / C 后端不安全"这种地址语义之别；准确的说法是
  "C 后端是**没有运行时边界层**的发射目标，其产物不实施 VSpace 与配额检查"。
  给生成程序加运行时边界层或改成运行期栈分配属于多后端 ABI 议题，需作者确认后再做。

## TCB 与 VSpace 的最终分工（2026-09-21 定；实测见 [专项报告](../implementation/vm-tcb-vspace-split-report.md)）

| 层 | 管什么 | 明确不管 |
| --- | --- | --- |
| **VSpace** | 存储的申请与释放；区域登记；当前可访问范围；READ / WRITE / CALL 权限；稳定区域句柄 | 执行状态、栈水位、谁在跑 |
| **TCB** | frame / slot / 指令位置；stack waterline 与各 frame 的回退点；当前执行用的 VSpace、CSpace 与 quota **引用**；Trap 与执行结果 | **不申请、不释放、不拥有栈字节**；不直接扣栈容量 |

### 区域记录：容量 ≠ 可访问窗口

```text
base          底层存储地址
capacity      存储字节数       —— 占用与重叠判断用 [base, base + capacity)
accessible    当前可访问前缀   —— 访问判定用    [base, base + accessible)
rights        READ / WRITE / CALL
owner         诊断用
backing_kind  OWNED（VSpace 申请/释放/计账）或 EXTERNAL（外部借入）
borrow_count  当前活租约数
quota/charged OWNED 存储的扣费来源与实际扣掉的字节数（释放时按它归还）
generation/alive  句柄身份
```

窗口变化只改 `accessible`（`lainvm_space_set_accessible`）：**句柄全程稳定**，不再
remove/add；缩小之后被收回的那一段**立刻**访问不了；失败时区段一点不变。

### 两类存储的接口（三件事分开，不再用一个模糊的 remove）

| 接口 | 做什么 | 不做什么 |
| --- | --- | --- |
| `lainvm_space_alloc(space, capacity, alignment, initial_accessible, rights, owner, quota)` | quota 原子预扣 → 分配 → 清零 → 登记 → 记下扣费来源与原始指针；任一步失败全部回滚 | —— |
| `lainvm_space_free(space, handle)` | 精确撤销 + 释放底层存储 + 按**原账户**归还 charged；`borrow_count != 0`、重复释放、跨空间、非 OWNED 一律拒 | 不撤销 external 映射 |
| `lainvm_space_map_external(space, base, capacity, accessible, rights, owner)` | 登记别人给的字节 | 不 `free(base)`、不扣账 |
| `lainvm_space_unmap_external(space, handle)` | 只撤销映射 | 不 free、不归还 quota |
| `lainvm_space_borrow` / `end_borrow` | 记"有几个活持有人"；借用期间不许释放 | 不改容量/窗口/权限，不动 quota |

### 栈租约

`LainVmStackLease {space, region}` —— 只有空间与**稳定句柄**；base / capacity / 权限
一律现读 VSpace 记录，TCB 里不留副本。协议：

1. 供给方 `lainvm_space_alloc_stack(space, bytes, owner, quota)`（初始 `accessible = 0`）；
2. `lainvm_tcb_new(..., lease, quota)` 校验后 `borrow_count++`；
3. 执行期间 TCB 用 `set_accessible` 决定窗口（水位）；
4. `lainvm_tcb_free`：窗口收回 0 → 结束借用；**不**撤销、**不** free、**不**归还额度；
5. 供给方随后 `lainvm_space_free` 才真正释放并归还额度；
6. 有活借用时 `space_free` 稳定拒绝；创建中途失败会还回已取得的借用，但不释放供给方的区段；
7. 一份**栈**租约只借给一条执行流（否则拒 `LAINVM_LEASE_ALREADY_BORROWED`）。

`lainvm_tcb_set_space` 只换当前执行空间：租约仍属原空间，`#alloca` 因空间不同拒
**1006**，销毁时仍去原空间结束借用。本轮不实现运行中迁移栈。

**接口层失败与运行期 Trap 分开**：租约校验返回 `LainVmLeaseStatus`（0..7：OK /
NOT_IN_SPACE / BAD_HANDLE / NEEDS_READ_WRITE / WINDOW_NOT_ZERO / ZERO_CAPACITY /
ALREADY_BORROWED / BORROW_FAILED），**不占** engine 的 1xxx 号段。

### alloca 的生命周期属于 procedure activation

`#alloca` 属于当前 procedure activation：进入/离开 `#if` / `#loop` / `#switch`
**不**创建、也**不**结束它的生命周期；只有 **procedure return、Trap、取消、TCB 销毁**
才结束。根过程结束时水位归零、窗口清空。

循环里反复 `#alloca` 的实测增长（**不**每轮回收）：`#alloca[#bits<64>](1)` 要 8 字节
数据，水位按 **16 字节对齐**推进，所以每轮推进 16 字节，第 n 轮结束水位 = 16n − 8；
容量 4096 时 256 轮之后水位 4088，第 257 轮拒 **1007**。配额在**供给方 alloc 那一刻**
按整块容量扣一次，与循环多少轮无关（水位涨落不改账）。
