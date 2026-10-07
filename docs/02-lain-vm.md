# Lain-VM

> [!QUOTE]
> Lain-IR 能够解释执行（嗨，多稀奇啊）。
> 
> 由 Lain-VM 来在编译期执行 LAINIR。

## 基本实体

| 对象 | 职责 |
| --- | --- |
| VSpace | 记录可访问区域、权限和区域有效性 |
| TCB | 保存一条执行流的指令位置、调用状态、结果和故障 |
| CSpace | 提供执行流可使用的宿主服务能力 |
| Trap | 记录执行失败的分类、状态码和位置 |
| Endpoint | 执行流间通信的设计对象；其可用范围需单独验收 |

TCB 引用 VSpace，多个 TCB 可以共享一个地址空间。TCB 对 LAINIR 保持不透明，
不能作为程序可读写的控制结构交付。栈由供给方通过 VSpace 分配，TCB 借用租约。
Endpoint 与调度属于设计对象，目前没有对应的端到端使用验收。

## 执行与权限

执行请求指定已验证的 LAINIR 产物、其中的过程、物理参数、地址授权、宿主能力和预算。
正常完成交付声明的物理结果；失败交付 Trap。没有授权的外部地址不可访问。
读、写和间接调用分别需要相应权限；检查覆盖整个访问范围。
宿主能力进行内存访问时同样需要验证参数范围、对象身份和生命周期，不能绕过执行环境的授权。

Trap 记录错误分类、过程与指令位置；编译器将编译期 Trap 转为诊断。
切片耗尽表示本次调度额度结束，不等价于共享总预算已经耗尽。
执行终态和稳定诊断码是验收对象，宿主崩溃不能作为合法失败结果。

## 能力空间（CSpace）：注册、冻结与宿主调用

能力项 = 键（外部符号名 `link_name`）+ 种类 + 回调 + **绑定 context**。context 是可信 C
注册路径在登记时给出的 `void *`（例如 `LainMetaHost *`），冻结后不再改变。

- `lainvm_caps_add` 复制并持有键名，把 context 绑到项上；同名重复登记拒 3，FUNCTION 项缺函数拒 4。
- `lainvm_caps_freeze` 之后禁止新增、扩容和修改绑定：冻结表上 `caps_add` 返回 **5**；再次
  freeze 幂等成功，且不可解冻。freeze 保证项地址稳定，**不**管理 context 的生命周期。
- 不提供修改 fn/context 的 setter：绑定只能来自可信注册路径，不从 LAINIR 参数恢复。
- `lainvm_tcb_set_caps(tcb, caps, diag)`：非 NULL 表必须已冻结，否则拒 **9112**；已有活动帧
  （运行中、暂停中或 Trap 后未结束的激活）时拒 **9113**；NULL 表表示无能力。解析在临时数组中
  完成后一次性替换，任何拒绝或 OOM 都保留旧绑定；未找到的符号保留 NULL 槽，调用时拒 1110。
- 宿主调用：`fn(context, raw, arg_count, result_out)`。context 不进入 `raw`、不从业务参数读取、
  不进入槽位或返回值；回调返回非零即转为 Trap，沿用其状态码。未解析的槽拒 1110，非 FUNCTION
  项拒 1111，缺函数拒 1112。
- 生命周期由任务所有者保证：能力表与绑定的宿主都必须活到所有引用它们的 TCB 销毁之后。
  `caps_free` 不销毁 context，也不做引用计数或撤销。释放顺序：TCB → 能力表 → 宿主。

`LainApplyLimits.caps` 非 NULL 时要求调用方已经冻结该表；apply 只借用，不修改也不冻结它，
未冻结时拒 9315。能力表可被多个 TCB 共享。

## 生命周期与预算要求

当前地址与能力边界：

- `#addr` 是**无类型裸物理地址**：只表示地址数值，**不携带**对象身份、generation 或
  pointee type；单字宽（`sizeof(void*)`），落内存就是一个字。
- **VSpace 管物理内存**：区域、分配、回收、映射与 READ/WRITE/CALL 权限。
- **CSpace 管宿主服务能力**。不要把每一个 `#addr` 都改造成 capability。
- `ref(T)`、borrow、对象身份与**对象级**时间安全属于 Lain/Meta 层。需要动态检查时，
  Meta 负责把 `ref(T)` 降成 handle + generation + offset 等物理值；具体表示仍待定义，
  它不改变 `#addr` 的规范。
- `#ptr2int` / `#int2ptr` 只做**位模式转换**；`#int2ptr` **不授予**访问权限，
  实际访问继续查当前 VSpace。

裸地址的失效按 VSpace 授权判断：

- 过程结束、且对应 VSpace 区域已撤销时，旧地址访问**拒绝**（实测拒 1004）；
- 同一数值地址后来被 VSpace 重新分配并授权时，旧裸 `#addr` 与新裸 `#addr`
  **不可区分**（实测：拿第一次留下的地址读到了新分配写进去的值）；
- 高层 `ref(T)` 的对象级时间安全需要 Meta 的生命周期规则，具体物理表示仍待定义。

栈按租约预留容量，初始可访问窗口为空。授权跟着**活窗口** `[base, base + 水位)` 走：
`#alloca` 推进水位，procedure activation 结束时回退。进入或离开 `#if`、`#loop`、
`#switch` 不结束 activation，因此 `#break`、`#continue` 本身不回收这次调用的 `#alloca`。
Trap、取消和 TCB 销毁也收回窗口。
窗口登记/撤销失败 → 拒 **1036**。

## 配额

配额按实际承诺字节计算：

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

验证器段 2xxx 的权威登记在 `seed/include/lainir/verify.h`。

## 后端约束

解释器与本机后端应分别说明如何实现同一权限、生命周期和失败契约。
仅在解释器中加入检查，不能证明生成的 C 获得相同保证。
若某后端仅支持受信任程序，应明确其范围，不以“零开销”省略契约说明。

当前解释器与 C 后端的 `#addr` 值语义：

- 解释器：`L1Value` 只有 `ADDR` 一种地址，载荷是单个指针；`type_size(#addr) = sizeof(void*)`；
  `#lea` 是无符号 64 位算术（回绕是定义好的）；`#ptr2int`/`#int2ptr` 是位模式转换。
- C 后端（`seed/src/backend/cbackend.c`）：`TY_ADDR` 宽度 64，load/store 按 `uintptr_t`
  单字读写，`#lea` 同样用 `uintptr_t` 算术（**不是**指针算术，避开 C 的 UB 面），
  `#int2ptr`/`#ptr2int` 同为一个 case。
- 解释器每次访问查当前 VSpace、并在承诺存储时
  过配额；生成的 C 程序目前**没有**任何运行时边界层（`cbackend.c` 里对
  `lainvm_space_*` / `lainvm_quota_*` / `LAINVM_MEM_*` 的引用为 0），`#alloca` 发射成
  静态存储槽而不是运行时栈分配。
- C 后端产物目前不实施 VSpace 与配额检查。给生成程序加入运行时边界层或改成运行期栈分配，
  需要先定义多后端 ABI 并完成相同约束的行为验收。

## TCB 与 VSpace 的分工

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

`#alloca` 的 count 是**元素个数**（独立的无符号常量，与元素类型无关）：执行时直接读
指令里的那个常量，**不**按指令类型实参的宽度解释；元素类型只参与 `count × 元素字节数`。
所以 `#alloca[#bits<8>](256)` = 256 字节、`#alloca[#bits<64>](256)` = 2048 字节，
C 后端的槽大小用的是同一个算式。

循环里反复执行 `#alloca` 会持续推进当前 procedure activation 的水位，直到该过程结束。
达到栈租约容量时拒 **1007**；配额在供给方分配整块存储时扣一次，水位涨落不改变账户。
