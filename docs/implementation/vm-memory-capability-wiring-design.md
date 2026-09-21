# 第 5 步接线设计：受检引用进入真实执行通路

依据 `docs/implementation/vm-memory-capability.md`（第 5 步）。作者已裁定：引用宽度 = **大卡**
（受检引用 16 B，`L1Value` 16→24 B）。其余三点（`#addr` 是否两字物理类型、宿主边界保留
受控裸地址通路、`fold` 第一期是否建表）由实施者决定——本文档就是这三点的裁定与做法。
运行报告：`docs/implementation/vm-memory-capability-report.md`。

## 1 勘察事实（都对着 HEAD 的树核过，file:line）

**值层**：`seed/include/lainir/value.h` 的 `L1Value {kind, bit_width, union{bits, void *addr}}` = 16 B，
三种 kind：`L1_VALUE_NONE` / `L1_VALUE_BITS` / `L1_VALUE_ADDR`。

**类型尺寸**：`seed/src/vm/engine.c:49-58` 里 `type_size(TY_ADDR) = sizeof(void *)`（8）、
`type_width(TY_ADDR) = 64`；`seed/src/core/infer.c:37` 与 `seed/src/backend/cbackend.c:316-320` 同样按 64。

**算子**（`seed/src/vm/engine.c`）：

| 算子 | 行 | 今天的检查 |
| --- | --- | --- |
| `op_lea` | 431 | **无**（`base + idx*scale + offset` 直接写结果） |
| `op_int2ptr` | 441 | 纯位重解释 |
| `op_ptr2int` | 447 | 纯位重解释 |
| `op_load` | 455 | `lainvm_space_check(READ)` → **1004** |
| `op_store` | 470 | `lainvm_space_check(WRITE)` → **1005** |
| `op_alloca` | 482 | 无栈 **1006**、尺寸回绕 **1035**、容量 **1007** |
| `op_data_addr` | 515 | 符号地址，失败 **1021** |

extern 调用：实参把 ADDR 摊成裸指针（`:556-557`），结果按返回类型 `#addr` 造 ADDR（`:566-568`）；
分支真值（`:694`）与 select（`:706-708`）都按位取。

**编译期执行**：`seed/src/fold/fold.c` **没有自己的算子实现**——它用 `lainvm_engine_run`（`:173`）
跑同一套引擎，所以改 `engine.c` 同时影响解释器与 `#eval`。`#eval` 结果若是 `#addr` 直接失败
**9308**（`:153-157`），值→位在 `:186-187`。

**bootstrap Meta 只走裸地址**：`bootstrap/std/emit.l1:21` 是
`#proc lain_meta_scratch_data(%host: #addr) -> #addr #extern "lain_meta_scratch_data"`；全树靠
`#lea(%scratch, N, 1, 0)` + `meta_cell_put/get`（`bootstrap/std/types.l1:36/41`，cell 是 `#bits<64>`）；
`bootstrap/std/parse.l1:384/389` 用 `#ptr2int[#bits<64>]` 比较地址。**bootstrap 里没有 `#int2ptr`、
没有 `#alloca`、没有 `#load[#addr]`/`#store[#addr]`**（grep 全树）。

**语料面**：64 条 `.lain` 里没有任何 `#alloca`/`#int2ptr`/`#load[#addr]`——动 ADDR 表示理论上不碰
语言语料，靠快照对比证明。用例面：`seed/tests/vspace_checks.c` 内嵌 LAINIR（`:211/268/313/321/331`
用 `#int2ptr[#addr]`，`:220/229/239/247/257/277/301` 用 `#alloca[#bits<64>]`）、
`seed/tests/programs.l1:67/74` 同。

## 2 裁定一：ADDR 分两种味道（"受控裸地址通路"的落地）

| 味道 | 谁产生 | 谁检查 |
| --- | --- | --- |
| **RAW**（今天的 ADDR，裸宿主指针） | 宿主能力/extern 结果、`#data_addr`、TCB 栈基址、base 是 RAW 的 `#lea` | 只过 VSpace（今天的行为） |
| **REF**（受检引用 `{句柄, 偏移}`） | `#alloca`、base 是 REF 的 `#lea`、`#int2ptr`、`#load[#addr]` 读到的 REF 记录 | 能力模型五步（第五步仍是 VSpace） |

`#lea` 保持"RAW 进 RAW 出、REF 进 REF 出"，于是 **bootstrap Meta 完全不受影响**（它的 base 永远
来自 extern 或 `#data_addr`）。程序内部**洗不出** RAW：`#ptr2int` 只给身份位、`#int2ptr` 只造 REF；
RAW 只能由宿主边界与装载器产生。这就是那条通路的边界。

## 3 裁定二：值表示加一种 kind，`#addr` 落内存按 16 B 自描述

- 新增 `L1_VALUE_REF`，载荷 `LainVmMemRef`（16 B）⇒ `L1Value` **24 B**（作者已批的代价）。
- 内存里的 `#addr` 槽 = **16 B 自描述记录**，不需要额外的 tag 字（代数 0 是保留值）：
  - REF：字 0 = `代数(32) | 槽号(32)`（代数 ≥ 1），字 1 = 偏移；
  - RAW：字 0 = 0，字 1 = 裸指针。
- `type_size(TY_ADDR) = 16`（`sizeof(LainVmMemRef)`），`type_width(TY_ADDR)` 仍是 64
  （`#ptr2int` 的整数位宽）。`#alloca[#addr](N)` 因此要 16×N 字节。
- `#store[#addr]`：RAW 存 `(0, 指针)`，REF 存 `(代数|槽号, 偏移)`；`#load[#addr]` 按字 0 还原味道。
  两种味道都能过内存，**没有"截断"这种静默错值**。

## 4 裁定三：C 后端第一期不跟随（记成缺口，并让它显式拒绝）

`seed/src/backend/cbackend.c` 的 `#addr` 就是 `uintptr_t`（`width_of_type:316`、load/store `:789-816`
按 8 B、`INST_ALLOCA:817` 指静态 C 数组）。第一期不改它，但**必须拒绝**而不是产出错的 C：
某过程一旦用到 REF，C 后端给稳定码 **9225**（"cbackend: checked reference needs the
capability-aware backend"）。理由：改 C 后端等于同时改多后端 ABI，那是 §1 点名要单独确认的一项；
先让解释器通路正确、后端显式拒绝（宁可拒绝，不要静默错值）。

## 5 各算子新语义

1. **`#alloca[#T](N)`**：`memcap.object_add(栈基址 + 对齐水位, N×size(T), owner=activation)` +
   `memcap.grant(对象, 0, 范围, READ|WRITE, owner=activation)` → 返回 **REF**。既有的三条拒绝
   （1006 无栈 / 1035 回绕 / 1007 容量）与水位推进不变；对象表满 → **9200**。
2. **`#lea(base, idx, scale, offset)`**：REF → 新 REF，**同一个句柄**、偏移 = 旧偏移 + `idx*scale+offset`
   （构造期不查、访问期查，与模型 §4.4 一致）；RAW → 今天的裸算术。
3. **`#load[#T](%p)`**：RAW → 今天（VSpace + 1004）；REF → `memcap.resolve(READ, size)`，失败时
   **直接抛能力模型的原码**（9200-9213，不再包一层，免得丢原因）。
4. **`#store[#T](%v, %p)`**：同理，失败抛原码；成功才 memcpy。
5. **`#ptr2int[#bits<W>](%v)`**：RAW → 裸指针（不变，Meta 依赖它）；REF → `代数|槽号`
   （**不带偏移**，偏移由来往双方各自提供，同 §4.2）。
6. **`#int2ptr[#addr](%bits)`**：只造 **REF**（`{代数, 槽号}`，偏移 0），使用时才判有效性。
   整数不能凭空产生 RAW。
7. **`#data_addr`**：保持 RAW（装载器给的）。

## 6 activation 生命周期

- `LainVmTcb` 内嵌一张 `LainVmMemTable`（每个执行上下文一张，4120 B）。
- `lainvm_tcb_new` 多一个 `generation_base` 参数：调用方（宿主/驱动）给单调递增的基数，
  满足模型"调用方保证基数大于同一块内存上以前发过的所有代数"的契约。
- activation 标签 = TCB 里单调计数器（每次起一次调用 +1），写进 frame 与 memcap 的 `owner`。
- 四条结束路径（正常返回 / Trap / 取消复位 / 销毁 TCB）都要 `revoke_owner(activation)` 然后
  逐个 `object_release`（对象 owner 也是 activation）：**撤销访问权与释放存储是两个动作，都要做**。

## 7 宿主边界（R04/R05 的判据）

宿主能力拿不到 REF（bootstrap 传的就是裸地址），所以宿主侧检查是：

1. 地址范围必须落在**调用者 VSpace** 的授权区段内（R04 判据）；
2. 传入的 host 对象必须是这次调用登记的**同一个**对象（R05 判据：身份，不是"能不能用"）；
3. **先检后写**：解析失败时目标内存与副作用计数都不变（模型里 `lainvm_memcap_host_write` 的形状）。

## 8 切片顺序与验收

1. 值层 + 解释器算子新语义（§3/§5）；
2. activation 生命周期（§6）；
3. 宿主边界（§7）；
4. C 后端显式拒绝（§4）。

每片都必须跑四条门禁：`python scripts/build.py`、`python scripts/check_vspace.py`（含
`--group capability`）、`python scripts/check_meta.py`、`worktree @ 改动前提交` 前后 64 条语料
快照对比；负对照至少一条。**失败/未定条数只能减少不能新增**；新能力必须进用例表。

## 9 明确不做（第一期）

CSpace 索引（槽号+代数仍可猜，见运行报告 §8）、能力派生树与通用委派、C 后端的受检引用、
`fold` 的受检引用（`#eval` 结果遇到 REF 直接 **9309**，不静默）、宿主字节写（`memcpy` 式）、
并发与重入。
