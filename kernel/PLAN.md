# Lain-Lang Kernel 架构与实现计划

## 1. 定位与硬边界

`kernel/` 是 Lain-Lang 与源语言**完全解耦**的纯现代 Lain 内核，对应规范 [docs/01-lain-ir.md](../docs/01-lain-ir.md) 与 [docs/02-lain-vm.md](../docs/02-lain-vm.md)。

- **负责**：
  1. `LAINIR`（内存级物理 SSA IR、`IrBuilder`/`ProcBuilder` 构造边界、静态核验、基础优化）；
  2. `Lain-VM`（基于 seL4 微内核理念的确定性、能力隔离、原生 M:N 调度编译期沙箱）；
  3. `Backend`（AOT 原生代码生成，首期为标准 C99）。
- **明确排除（Non-Goals）**：
  - 不包含任何 Lain 源语言语法、`RawAst`、模块系统、类型推导、泛型或代数效应；
  - 不包含 `.l1` 文本解析与打印（文本序列化/调试输出属于外部 `tools/`，主编译链路全内存直传 `IrModule`）；
  - 不重复造标准库轮子（容器、错误效应、位运算、格式化目标一律复用 `std`）。

---

## 2. 目录结构与严格单向依赖（DAG）

```text
kernel/
├── PLAN.md
├── PREREQS.md             # 支撑纯现代 Lain 写法的前置编译器/标准库改动清单
├── ir/                    # LAINIR 内核（仅依赖 std）
│   ├── model.lain         # 物理类型（tagged union Ty）、Op、Inst、DataSeg、Proc、IrModule
│   ├── builder.lain       # 面向 frontend/lower 的唯一构造边界（IrBuilder / ProcBuilder）
│   ├── verify.lain        # 静态核验（!{ VerifyErr }，Fail(VerifyFault)）
│   └── opt.lain           # IR 级优化（常量折叠、DCE、#unreachable 剪枝）
├── vm/                    # Lain-VM 微内核与 M:N 调度器（仅依赖 std + ir）
│   ├── addr.lain          # 强类型句柄/地址（VmAddr / SegId / TcbId / EpId）
│   ├── trap.lain          # Trap tagged union + VmTrap = Fail(Trap)（按需增量添加变体）
│   ├── vspace.lain        # 隔离地址空间（OWNED/EXTERNAL MutSlice、accessible/capacity 窗口、borrow）
│   ├── cspace.lain        # 宿主能力表（注册、freeze()、隐式 context 分派）
│   ├── endpoint.lain      # 无缓冲同步会合端点（Rendezvous IPC，从 Day 1 纳入 M:N 设计）
│   ├── image.lain         # 纯只读程序镜像（固定确定性基址预烤，可跨多 VSpace/TCB 共享）
│   ├── tcb.lain           # 纯执行流控制块（不拥有/不分配虚拟内存，仅持有外部供给的 SegId 栈租约）
│   ├── engine.lain        # 无状态切片执行引擎（run_slice -> SliceOutcome）
│   └── sched.lain         # M:N 协作式调度器（就绪队列、Endpoint 唤醒、对外 apply 沙箱入口）
└── backend/               # AOT 后端（仅依赖 std + ir，与 vm 互相隔离）
    └── c99.lain           # IrModule -> 标准 C99（面向 std::io::Sink，与 VM 语义逐位等价）
```

### 依赖红线
- `ir` 只依赖 `std`；
- `vm` 只依赖 `std + ir`；
- `backend` 只依赖 `std + ir`，**禁止依赖 `vm`**；
- `frontend/lower` 只通过 `kernel::ir::builder` 构造 IR，不触碰 `IrModule` 内部存储细节；
- 全 `kernel/` **禁止依赖 `src/` 或未来 `frontend/`**。

---

## 3. 核心架构契约

### 3.1 内存直传与 `Builder` 封装边界
- `frontend/lower` 通过 `IrBuilder` / `ProcBuilder` 产出内存中的 `IrModule`，由 `Builder` 负责 SSA `ValId` 分配、结构化控制流配对回填与内部编码；
- `IrModule` 经 `verify::verify_module` 校验后，直接进入 `vm::sched::apply`（编译期执行）或 `backend::c99::emit`（AOT 代码生成），全程零文本往返。

### 3.2 微内核实体分离（`Image` / `VSpace` / `TCB`）
- **`Image`（纯只读、可共享）**：装载 `IrModule`，按固定确定性基址预烤 `.rodata`/`.data` 虚拟地址与过程槽位；一次装载后可被任意多个隔离 `VSpace` 与并发 `TCB` 共享。
- **`VSpace`（按隔离域独占）**：唯一的物理内存仲裁者；负责挂载 `Image` 数据段、开辟栈区段（`SegId`）、映射外部 `MutSlice(u8)`、仲裁 `[base, base + accessible)` 窗口与读写权限。
- **`TCB`（纯执行流、零内存所有权）**：只保存 `pc`、调用帧栈、SSA 寄存器槽位与外部传入的栈租约 `SegId`，**绝不负责向 `VSpace` 申请或释放内存**。

### 3.3 原生 M:N 协作式调度（`Engine` + `Endpoint` + `Sched`）
- `Engine::run_slice(img, vs, cs, tcb, fuel_slice)` 每次执行一个 Fuel 时间片并返回强类型终态：
  ```lain
  let SliceOutcome = enum {
    Done(u64)
    FuelSlice
    Blocked(EpId)
    Trapped(Trap)
  };
  ```
- 无需汇编上下文切换：切换 `TCB` 等价于一次普通函数返回与状态回写；
- `Sched` 管理就绪队列与 `Endpoint` 会合唤醒：`M = 1` 为单线程确定性执行（覆盖普通 `comptime` `apply`），`M > 1` 接入 `std::thread` 即为多核 M:N 并发编译。

---

## 4. 现代 Lain 工程铁律

1. **统一现代语法**：`pub { ... };`、`use import("...")::{ ... };`、静态路径一律 `::`、运行期字段与 UFCS 一律 `.`、核心实体全部组织为 `module { pub { let Self = ...; ... }; }`。
2. **错误传播走 `std::err::Fail(E)`**：`VmTrap = Fail(Trap)`、`VerifyErr = Fail(VerifyFault)`、`OomErr` 全部通过中止性代数效应 fail fast and clear，零 `TrapReport` 返回值与 `out_val` 输出参数。
3. **强类型与零重复**：使用 `VmAddr` / `SegId` / `TcbId` / `EpId` / `ProcId` / `DataId` / `ValId` 隔离不同语义的整数；公共底层原语（`align_up`、`is_pow2`、`Vec`、`Option`、`algo`、`Sink`）统一归入 `std`。
4. **全自动 RAII、零注释、零伪造代码**：依赖结构体字段自动递归 `drop`（不手写 `*_free`），所有 `Trap::Reason` / `VerifyError` 变体随实际实现按需添加，不预先编造。

---

## 5. 推进里程碑

- **Phase K0（前置闭环）**：完成 [PREREQS.md](PREREQS.md) 并刷新 `bootstrap/lainc_boot.c`，确保现有 `kernel/` 文件零告警编译通过。
- **Phase K1（IR 闭环）**：完成 `ir/model.lain`、`ir/builder.lain`、`ir/verify.lain`。
- **Phase K2（VM 微内核与 M:N 调度）**：修正 `vm/tcb.lain` 栈租约归属，补齐 `vm/cspace.lain`、`vm/endpoint.lain`、`vm/image.lain`、`vm/engine.lain`、`vm/sched.lain`（含 `apply` 入口）。
- **Phase K3（AOT 后端）**：实现 `backend/c99.lain`（面向 `std::io::Sink`），打通与 `vm` 的语义等价验证。
- **Phase K4（优化与外部工具）**：实现 `ir/opt.lain`，并将 `.l1` 文本打印/解析放到 `tools/` 供调试使用。
