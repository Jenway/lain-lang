# 最小内存能力模型：运行报告

日期：2026-09-21。对应交接：`docs/implementation/vm-memory-capability.md`（§6 的推进顺序 1–4）。

**一句话**：模型层 18 条用例（§5 验收表 17 条 + §4.1 第二个原型的探针 1 条）全部通过；
**实际 VM 通路还没有接线**，两者分开报告（§6.5）。过程中实测出并修掉一个真缺陷：
切换 VSpace 之后销毁 TCB，会漏掉栈区段的撤销与释放。

## 0 基线

- `HEAD = 2b6462c`（`update docs`）。工作区里另有文档一侧未提交的改动
  （`docs/implementation/README.md`、`docs/implementation/vspace-action-plan-astra.md`、
  `docs/spec/vm.md`、`docs/implementation/vm-memory-capability.md`、`scripts/check_docs.py`），
  本交付**没有碰**它们，提交里也不含。
- 本交付改动：`seed/include/lainvm/memcap.h`（新）、`seed/src/vm/memcap.c`（新）、
  `seed/src/vm/tcb.c`（销毁路径）、`seed/tests/vspace_checks.c`（capability 组 +
  `lifetime_reuse` 断言）、`scripts/check_vspace.py`（组名）。**没有改 bootstrap/**。

## 1 复现命令与实际结果

```text
python scripts/build.py                                            BUILD OK（严格告警）
python scripts/check_vspace.py --group capability                  18 条：通过 18，失败 0，未定 0
python scripts/check_vspace.py --report build/vspace-capability.json
                                                                   46 条：通过 38，失败 4，未定 4，exit=1
python scripts/check_vspace.py --case cap_revoke --expect-trap 9999      exit=1（负对照）
python scripts/check_vspace.py --case cap_live_rw --expect-value 99      exit=1（负对照）
python scripts/check_meta.py                                       19/19 通过
python scripts/check_docs.py                                       docs: 35 files, 109 local links, 0 errors
```

改动前后的 Meta 全语料对比（`tcb_free` 改过，所以要证它没动语言行为）：
按 `AGENTS.md` §6 的办法在 `build/oldwt` 挂 `HEAD` 的 worktree、各自 `build.py` +
`check_meta.py snapshot`，再 `compare` —— **64/64 逐字节一致**（worktree 已删除）。

## 2 模型的决定与理由

- **引用形状**：`{能力句柄, 对象内偏移}`。对象身份、范围、权限、代数**都在表里**，不在引用里。
  所以改引用的位不能扩大范围、不能提权，最多把它改坏（改坏就被拒）——这一条让 §4.3 的
  "普通字节复制 / 部分覆盖"有了规则：字节复制保留关联，部分覆盖只会**丢**权限，不会**加**权限。
- **表身份 `serial`**：句柄里带表身份。销毁上下文后在**同一块内存**重建表（新 `serial`），
  旧引用与旧整数都不能复活。用计数器而不是"空间对象地址"，因为同址重建时地址会一样。
- **代数**：槽重用时递增；到 `0xFFFFFF` 就**不再重用**那个槽（宁可少一个槽，也不让旧引用复活）。
- **另开一张表**，不塞 `LainVmSpace` 的 64 个区段槽：一次调用的对象数与区段数增长速度不同
  （每次 `alloca` 一个对象，整段栈只是一个区段）。
- **撤销与释放是两个动作**：`revoke_owner` 只让能力失效；`committed` 只在
  `object_release` 时归还。释放存储**不**顺手删能力记录——持有者再去用会被判 9211
  "对象已不存活"，那正是要留下的证据。
- **访问检查顺序**（§3 五条）：表身份 → 所属上下文 → 能力存活 → 引用代数 → 对象代数 →
  对象存活 → 权限 → 区间（先验偏移，再减法比剩余）→ 当前 VSpace。
- **诊断码**（本层稳定码，全树此前未占用）：
  9200 对象表满、9201 参数非法、9202 能力表满、9203 授权越出对象、9204 权限为空、
  9205 **已废弃**（句柄不再带表身份；同址重建改由代数基数检出 → 9208）、9206 当前上下文无权使用、9207 能力已撤销 / 槽无效、9208 引用代数不符、
  9209 权限不足、9210 越出授权范围、9211 对象已不存活、9212 当前 VSpace 未授权、
  9213 撤销/释放句柄无效（含 `owner=0`、重复释放）。

## 3 §5 验收表逐条结果（模型层）

| 用例 | 期望 | 实测 | 说明 |
| --- | --- | --- | --- |
| `cap_live_rw` | 值 1 | 值 1 | 写进去、读回来、宿主缓冲确实变了 |
| `cap_bounds` | 拒 9210 | 拒 9210 | 越界一字节 / 越过授权子范围 / 偏移 `UINT64_MAX`，目标内存未改 |
| `cap_rights` | 拒 9209 | 拒 9209 | 只读能力写入被拒且原值不变，读仍成功 |
| `cap_revoke` | 拒 9207 | 拒 9207 | 撤销后读、写都拒 |
| `cap_same_address_reuse` | 值 1 | 值 1 | **主动安排**同址重建：新引用可用，旧引用 9208 |
| `cap_copy_revoke` | 拒 9207 | 拒 9207 | 三个副本（赋值 / 内存往返 / 再取回）全部失效 |
| `cap_memory_roundtrip` | 值 1 | 值 1 | 存活时可用；部分覆盖只能变坏；撤销+同址复用后老引用 9208 |
| `cap_forged_reference` | 拒 9206 | 拒 9206 | 别人的能力 9206、越界 9210、改代数 9208、假槽号 9207、假整数 9208、跨上下文 9208；内存都没变 |
| `cap_int_roundtrip` | 值 1 | 值 1 | 存活时整数往返有确定结果，句柄字段逐位一致 |
| `cap_int_after_revoke` | 拒 9208 | 拒 9208 | 旧整数不能恢复权限、也拿不到新对象 |
| `cap_context_reuse` | 拒 9208 | 拒 9208 | 同址重建（代数基数更大）后：槽未占用时旧引用 9207、槽被复用后旧引用与旧整数都 9208；新表自己可用 |
| `cap_child_borrow` | 值 1 | 值 1 | 子期间可写，撤销子上下文后父对象仍可用、账不变 |
| `cap_space_switch` | 拒 9212 | 拒 9212 | 能力有效但新空间没授权 → 拒；原空间仍可用 |
| `cap_host_access` | 拒 9207 | 拒 9207 | 先解析后副作用：失效与越界都没产生任何副作用 |
| `cap_failure_cleanup` | 值 1 | 值 1 | 空大小 / 回绕 / 越界 / 无权限 / 表满都不留残留、账不动 |
| `cap_quota_two_actions` | 值 1 | 值 1 | 撤销后 `committed` 仍 64；释放后归 0；存储没了的能力 9211 |
| `cap_tcb_destroy_after_switch` | 值 1 | 值 1 | 真实 VM：换空间后销毁 TCB，栈在自己的空间里撤销并释放 |
| `cap_protoB_stale_address_probe` | 值 1 | 值 1 | 见 §4.1：**PASS = 缺口按预期复现**，不是"原型 B 正确" |

`lifetime_reuse` 的断言按 §5 改了：不再要求两次的物理地址不同，改成"同址复用时旧引用不能
访问"。它现在实测输出 `同址复用后旧引用仍读到 7（按契约必须被拒）` —— 说明真实通路上
**还没有**能力层，与模型层的结果是两件事。

## 4 §4 接口实验的结论

### 4.1 引用怎样到 load/store（两个原型比较）

- **原型 A（采用）**：值里携带"能力句柄 + 偏移"，宿主地址只在受检访问时解析。
  关联**就在值里**，所以参数传递、返回、复制、写进内存再读出来都自然保留；改坏只会失去权限。
  18 条用例里的 `cap_memory_roundtrip`、`cap_copy_revoke`、`cap_int_*`、`cap_forged_reference`
  都是对它的实测。
- **原型 B（探针，不采用）**：保留地址值，另用 VM 元数据跟踪它对应哪条能力。探针用一张
  `{地址 -> 能力}` 的旁表，按地址唯一（新登记覆盖旧的 —— 这正是"按数值地址查询**现在**的能力"）。
  实测结果是 fail open：撤销 + 同址复用之后**地址这个数字没变**，逃逸出去的旧地址再回来时旁表
  把**新对象**的能力给了它，于是旧引用拿到了新对象的权限（读到新对象写的 `0x4242`）。
  旁表没有第二个键可用——两条地址相同的引用在 B 里无法区分。
  要救 B 就得拦截**所有**字节写（宿主 `memcpy`、能力输出、解释器的值槽），这正是 §4.3 的活。

### 4.2 整数转换

位布局：`serial(32) | 代数(24) | 槽号(8)`，**不含偏移**（往返要显式带上偏移）。
规则是"转换保留能力身份的位，但身份是否还有效一律由表判"：对象存活时往返成功
（`cap_int_roundtrip`）；撤销并复用后旧整数被判 9208（`cap_int_after_revoke`）；
伪造整数因代数不符被判 9208（句柄位布局 = 代数(32) | 槽号(32)，不含偏移）。没有采用"一律拒绝 `int2ptr`"这种糊法。

### 4.3 内存里的地址

受检引用写进内存再取回**保留关联**（关联在值里）；普通字节复制是允许的；
部分覆盖的规则是"只能丢权限"：改槽号（`bytes[0]`）→ 9207 越界，改代数（`bytes[4]`）→ 9208，改偏移 → 9210。
**尚未覆盖**：宿主的字节写（`memcpy` 路径）与直接返回裸地址——它们必须纳入同一问题，
而目前模型层还没有拦它们（接线时一并做）。

### 4.4 lea

模型的答案：`lea` 只能产出"某个能力 + 更大的偏移"，**不能**产出可以重新授权的裸地址——
地址算术不提升权限、不换对象；越界与回绕由访问检查（9210）兜住，构造本身允许。
这样 §4.4 的问题就落回 D3（偏移是否允许暂时越界、尾后值怎样表示）——D3 仍未定，
`lea_construct_only` / `lea_wraparound` 两条仍是 BLOCKED。

## 5 §7 同时可推进项的结果

- **配额的两个动作**：已落（`cap_quota_two_actions`）。撤销访问权不归还额度，释放存储才归还；
  `committed` / `released` 两个账分别记。单位按"实际承诺的存储字节"，这正是 §7 要的那条。
- **换空间后直接销毁 TCB**：**实测出缺陷**。改之前，销毁时按 `tcb->vspace` 去撤销栈——换过空间
  之后那个空间里没有这段租约，于是**一段都撤不掉、栈内存再也没人 free**（实测：租约所在空间
  2→2，静默泄漏）。修法是按**句柄里记的那个空间**撤销与释放（句柄本来就带着空间身份）。
  修后：租约空间 2→1（精确撤销并 free），换到的空间 0 段不受影响。
- **统一受检宿主适配器**：模型层已落（`lainvm_memcap_host_write`，先解析后副作用，
  `cap_host_access` 证明失效/越界时副作用为 0）。接进 `cap_emit_write` 属第 5 步。

## 6 内存开销（实测）

`build/sizeof-probe.c`（一次性探针，落在 `build/`，未提交）：

```text
table=4120  object=32  cap=48  handle=8  ref=16
```

即每个执行上下文一张表 4120 B（32 个对象槽 + 64 个能力槽，定长、无动态分配），
受检引用 **16 B**（句柄 8 B + 偏移 8 B）——这就是作者裁定的"大卡"宽度。
**当前运行时开销为 0**：还没有任何代码 new 这张表——接线后按"每个执行上下文一张"计；
接线后 `L1Value` 由 16 B 变 **24 B**（8 B 头 + 16 B 引用载荷）。

## 7 已覆盖 / 未覆盖

**已覆盖**：对象登记与释放的参数边界、授权范围边界、权限、撤销（含副本）、同址复用、
代数与代数基数失效（同址重建）、跨上下文与跨空间、伪造（改位 / 借别人的 / 假整数）、字段级部分覆盖、
整数往返、子树借用与上下文结束、额度的两个动作、宿主适配器的先检后副作用、
以及"换空间后销毁 TCB"这条真实 VM 路径。

**未覆盖**（明确不做）：真实 `#alloca` / `load` / `store` 通路（第 5 步）、宿主能力接线、
`CALL` 权限（本层只收 READ/WRITE；间接调用仍单独判）、能力派生树与通用委派、
跨进程序列化、`memcpy` 式宿主字节写、并发与重入（模型假定检查与访问之间不发生撤销或释放）、
host 越界索引、原子/读改写与向量算子（仍 `LAINVM_OP_STUB`）、sanitizer、C 后端。

## 8 残留缺口（模型保证到哪一层就报告到哪一层）

1. **槽号 + 代数是可猜的**。程序若能把任意整数当引用用，理论上可能猜中一条**属于自己上下文**
   的有效能力。真正要堵，得把能力记录放进程序寻址不到的表（CSpace），程序只拿得到索引。
   当前模型挡住的是"改位扩大授权"和"借别人的引用"（都实测到了）。
2. **`owner` 是整数标签**，不是不可伪造的身份；它挡的是"位一模一样的别人的引用"。
3. **单线程 / 同步**假设（§3 允许的原型前提）。

## 9 仍然失败的 4 条（都是尚未实现的两件）

```text
FAIL host_past_region   R04：宿主按裸地址读区段外（status=0，输出长度=2）
FAIL host_wrong_identity R05：换诱饵 host 照样受理（长度=4）
FAIL lifetime_escape    R06：逃逸出去的地址仍读到陈旧值 7
FAIL lifetime_reuse     R07：同址复用后旧引用仍读到 7（按契约必须被拒）
```

前两条属 D4（宿主边界），后两条属 D2（地址模型）。**现在模型层已经给出答案**，
缺的是把它接进真实通路——所以这 4 条从"要不要做能力"变成了"什么时候接线"。

## 10 正式接口建议（第 5 步的设计，按 §1 先给设计再确认）

**作者裁定（2026-09-21）：选"大卡"= 收紧句柄，受检引用 16 B（`L1Value` 16→24 B）。**
其余三点（`#addr` 是否两字物理类型、宿主边界保留受控裸地址通路、`fold` 第一期是否建表）
由实施者决定。模型层**已经按这个宽度落地**（见 §11）；下面 10.2 的三个选项保留作记录。

### 10.1 现状（实测）

```text
seed/include/lainir/value.h:  L1Value = {kind, bit_width, union{bits, void *addr}} = 16 B
```

即今天 `#addr` 就是一个裸宿主指针，`#ptr2int` / `#int2ptr` 是纯位重解释。走原型 A 之后
`ADDR` 要携带"能力 + 偏移"，于是**每个值的宽度都要变**，这是本层唯一有全局影响的一处。

### 10.2 引用宽度：三个选项（实测 `sizeof`）

```text
宽引用 {table, slot, generation} + offset          ref=24 B   L1Value 16->32 B（+100%）
收紧句柄 {slot, generation} + offset               ref=16 B   L1Value 16->24 B（+50%）
窄引用 {index(32), offset(32)}                     ref= 8 B   L1Value 16->16 B（不变）
```

- **收紧句柄**：把表身份从句柄里拿掉，改成 `lainvm_memcap_init(table, generation_base)` ——
  代数由调用方给的**单调基数**起算，于是"同址重建的表"拿到的是更高的代数，旧引用照样失效
  （`cap_context_reuse` 依然过），句柄从 16 B 降到 8 B。代价：调用方要把这个单调计数器传下来
  （仍然是"能力注入、无全局可变状态"）。
- **窄引用**：值里只有一个 32 位**不透明下标**（每次求值一个引用表），`L1Value` 完全不涨。
  代价：偏移限 4 GiB 且引用要经一层表间接访问——是"CSpace 索引"那个形状，和 §8 残留缺口的
  修法方向一致，但要多一张每求值的表。
- **推荐**：先按**收紧句柄（16 B）**接线——它不需要额外表、语义最直白；窄引用留作
  "值宽度不能涨"时的退路。这一条要作者拍。

### 10.3 各处改动面

1. **`#alloca` 对象化**：一次 alloca = 一个存储对象（现在只是一个水位）；返回**受检引用**而不是裸地址。
   `owner` = 当前 activation 的标签。
2. **load / store**：助记符与编码不变，**操作数语义**变（`ADDR` 操作数就是受检引用）；
   检查顺序用模型里那五步；`space_check` 仍是第 5 步。
3. **`#lea`**：产出 `{同一个能力, 偏移 + idx*scale + offset}`；不换对象、不提权。
   "偏移能不能暂时越界 / 回绕怎么表示"仍属 D3。
4. **`#ptr2int` / `#int2ptr` 契约**：整数承载引用身份的位（模型里是 `serial|代数|槽号`，
   不收偏移）；**有效性一律由表在每次访问时重判**，整数本身不构成权限。
5. **过程结束的编排**：返回 / Trap / 取消 / 销毁四条路径都要 `revoke_owner(activation)` +
   `object_release(该调用的对象)`；子调用借用父对象不改归属（模型已测）。
6. **宿主边界**：`cap_emit_write` 等能力改成"先解析、后副作用"（模型里已有
   `lainvm_memcap_host_write` 这一形状）。**注意**：`bootstrap/` 的 Meta 现在是把 scratch
   的地址当 `bits<64>` 传给宿主能力的——能力引用不能是唯一的地址表示，宿主边界要保留一条
   **受控的**裸地址通路（否则 Meta 立刻编不过）。
7. **`fold` 与 C 后端**：`fold` 在编译期求值，要么建同样的对象表，要么明确它只处理无对象的值；
   C 后端要不要跟着换成受检引用，属 §1 说的"多后端 ABI"范围。

### 10.4 请作者确认四点

1. ~~引用宽度选**收紧句柄（16 B，`L1Value` 24 B）**还是**窄引用（8 B，值不涨）**？~~
   **已定：收紧句柄（16 B，`L1Value` 24 B），并已按此改完模型层（§11）。**
2. `#addr` 是否接受变成**两字物理类型**（影响 LAINIR 类型系统、规范文本与序列化形态）？
3. 宿主边界保留一条受控裸地址通路（`bootstrap/` Meta 现在依赖它）——可以吗？
4. `fold` 要不要一并建对象表，还是第一期只接解释器通路？

确认之前**不动** `L1Value`、load/store 编码与后端——按 §1，这一层变更要先有设计。

## 11 作者的裁定与按此落地的改动（2026-09-21）

**裁定**：引用宽度选**大卡 = 收紧句柄**，受检引用 16 B（`L1Value` 16→24 B）。作者选的是
"大卡（16 字节）"；其余三点（`#addr` 是否两字物理类型、宿主边界保留受控裸地址通路、
`fold` 第一期是否建表）由实施者决定。

**按这个宽度改了什么**（`seed/include/lainvm/memcap.h`、`seed/src/vm/memcap.c`）：

1. 句柄 `LainVmMemHandle` 从 `{table, slot, generation}`(16 B) 变成 `{slot, generation}`(8 B)：
   表身份不再进句柄。
2. `lainvm_memcap_init(table, generation_base)`：调用方保证基数**大于同一块宿主内存上
   以前发过的所有代数**；同址重建的上下文因此拿到更高的代数，旧引用照样被拒。
   `handle_none` 的判据改成 `generation == 0`（0 是保留值，槽号不再兼作哨兵）。
3. `LAINVM_MEMCAP_MAX_GENERATION` 从 `0xFFFFFF` 提到 `0xFFFFFFFF`。
4. 诊断码 **9205（表身份不符）废弃**，号码保留不重用；同址重建由代数检出 → 9208。
5. 整数位布局改成 `代数(32) | 槽号(32)`（不含偏移；偏移由来往双方各自提供）。
6. 用例跟上：`cap_rig_open` 的参数语义变 `generation_base`；`cap_context_reuse` 改成
   "重建 + 槽位被重新占用"（槽未占用时旧引用 9207、复用后旧引用与旧整数 9208）；
   `cap_forged_reference` 的"假表号"换成"假槽号 9207"，跨上下文与假整数变 9208；
   `cap_int_roundtrip` 去掉表身份比较。

**实测**：

```text
python scripts/build.py                              BUILD OK
python scripts/check_vspace.py --group capability     18 条：通过 18
python scripts/check_vspace.py                        46 条：通过 38，失败 4，未定 4
python scripts/check_meta.py                          19/19
负对照 cap_revoke --expect-trap 9999 / cap_live_rw --expect-value 99   都 exit=1
sizeof: table=4120  object=32  cap=48  handle=8  ref=16
```

**行为没有变宽**：失败仍是那 4 条（R04/R05 宿主边界、R06/R07 地址模型），未定仍是那 4 条。

---

## §11 第 5 步接线（受检引用进真实通路）实测

设计见 `docs/implementation/vm-memory-capability-wiring-design.md`。分两片落地：`6b4c19a`
（值层 + `#alloca`/`#lea`/load/store/`#ptr2int`/`#int2ptr` + activation 生命周期）、`8971755`
（宿主边界先检后写）。

**接上了什么**

- `L1Value` 新增 `L1_VALUE_REF`（载荷 16 B）→ `L1Value` **24 B**（作者已批的"大卡"代价）。
  `L1Value.as.ref` 与 `LainVmMemRef` 同布局（`_Static_assert` 钉住）。
- ADDR 两种味道：**RAW**（宿主能力结果 / `#data_addr` / TCB 栈基址，只过 VSpace）与 **REF**
  （`#alloca`、base 是 REF 的 `#lea`、`#int2ptr`、`#load[#addr]` 读到的记录，过能力模型五步）。
  `#ptr2int` 对 REF 只给身份位、`#int2ptr` 只造 REF ⇒ **程序内部洗不出 RAW**。
- `type_size(TY_ADDR) = 16`：`#addr` 落内存是 16 字节自描述记录（字 0 = 0 → RAW、字 1 是裸指针；
  否则字 0 = 代数(32)|槽号(32)、字 1 = 偏移）。两种味道都过得去，不会静默截断。
- `#alloca` 一次发放一笔对象 + 一条覆盖它的能力（owner = 本次 activation），返回 REF。
- 过程结束（正常返回 / `leave_region` / Trap / 销毁 TCB）四条路径都 `revoke_owner` + 释放对象：
  **撤销访问权与释放存储是两个动作，都做**。
- 宿主边界：宿主服务由驱动 `lainmeta_host_attach_space` 显式授权；`lain_meta_emit_write` 在任何
  副作用之前判"有没有授权"与"区间在不在该空间授权的区段里"，拒时输出一个字节都不动
  （新码 `LAINMETA_ERR_DENIED = 5`）。
- `fold` 遇到 REF 直接 **9309**，不把引用静默变成编译期常量。

**实测**

```text
python scripts/build.py                                BUILD OK
python scripts/check_vspace.py                         50 条：通过 46，失败 0，未定 4
   --group capability                                  21 条：通过 21
   --group host                                         4 条：通过 3，未定 1
python scripts/check_meta.py                           19/19
负对照（host_in_region_ok 值改 99 / host_past_region 码改 9999）  都 exit=1
worktree @ 6b4c19a 与 @ 0d6b82c 前后语料快照            64/64 逐字节一致（两次）
```

**交接 §5 点名的那几条，现在的结果**

| 项 | 交付 A 时 | 现在 |
| --- | --- | --- |
| R04 宿主按裸地址读区段外 | 复现（status=0，输出长度 2） | **拒 5**，正对照区内 4 字节仍成功 |
| R05 换一个没授权的宿主对象 | 复现（写进诱饵输出，长度 4） | **拒 5**，诱饵输出仍是 0 |
| R06 引用逃逸后仍可读 | 复现（读到陈旧值 7） | **拒 9207** |
| R07 同址复用后旧引用仍可进 | 复现（读到 7） | **拒 9207** |
| 失败 / 未定 | 4 / 4（46 条） | **0 / 4（50 条）** |

**没做的（明确记着）**

- **C 后端是"未受检后端"**：同帧内用法两边一致（`alloca_store` 用例），缺口只有**逃逸**这一处
  —— 解释器拒、C 后端没有对应检查。改它属于多后端 ABI，需要作者确认（设计 §4）。
- 4 条 BLOCKED 不变：`host_source_index_bounds`（未实施）、`lea_construct_only` / `lea_wraparound`
  （等 D3）、`budget_unimplemented`（等 D5）。
- 槽号 + 代数仍可猜（需要 CSpace 索引）；单线程假设；`lea` 的宽度语义（D3）未定。
