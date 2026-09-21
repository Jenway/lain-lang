# TCB 与 VSpace 最终分工：运行报告（2026-09-21）

本报告对应 [VM 契约](../../docs/spec/vm.md) 里「TCB 与 VSpace 的最终分工」一节，
记录六个提交的实测结果。**没有推送**。

## 1 改动前状态与调用点清单

- 本轮开始前的 HEAD：`5529060`（文档同步：`#addr`/VSpace 边界校正）。
  工作区里另有文档一侧未提交的 `docs/implementation/README.md` 与未跟踪的
  `scripts/check_docs.py` —— **本轮一行没碰**（见第 10 节）。
- 迁移前用 `rg` 列出的全部调用点（数量按当时的工作树）：

| 类别 | 处数 | 处理 |
| --- | --- | --- |
| `lainvm_tcb_new` | 11（`fold.c` 1 + 测试驱动 10） | 全部改成"供给方先 `alloc_stack`、再传租约" |
| TCB 里的栈字段（`stack_space` / `stack_base` / `stack_size` / `stack_window` / `stack_window_size`） | 5 个字段、引用点 30+ | 换成 `stack_lease {space, region}` + `stack_used`；副本字段全删（漏改的调用点编译失败） |
| `lainvm_space_add` / `lainvm_space_remove` | `add` 11 处、`remove` 10 处（image 3 / meta_boot 3 / engine 2 / tcb 2 / 测试 11） | 拆成 `alloc`/`free` 与 `map_external`/`unmap_external`；两个旧名字**删除**，不留 wrapper |
| 栈 quota 扣账 | `tcb_new` 里 `quota_charge` + `tcb_free` 里 `quota_release` | 移到供给方 `lainvm_space_alloc` / `lainvm_space_free` |
| `set_accessible` 之前没有的窗口更新路径 | `stack_window_sync` 里 remove+add ×2 | 换成同句柄 `set_accessible` |

## 2 字段与所有权表

| 结构 | 字段 | 谁写 | 谁读 | 所有权/生命周期 |
| --- | --- | --- | --- | --- |
| `LainVmRegion` | `base` `capacity` `accessible` `rights` `owner` `generation` `alive` `backing_kind` `borrow_count` `quota` `charged` `raw` | VSpace | 全体（只读视图 `lainvm_space_slot`） | 槽位永不变；`OWNED` 由 VSpace 释放并归还 quota，`EXTERNAL` 只撤销映射 |
| `LainVmStackLease` | `space` `region` | 供给方给、TCB 存 | TCB / 供给方 | **只借**：借出 → `borrow_count++`；销毁 → 窗口 0 + `borrow_count--`；释放归供给方 |
| `LainVmTcb` | `stack_lease` `stack_used` + frame/slot/位置/回退点/`vspace`/`caps`/`quota` 引用/Trap/结果 | TCB | TCB | 不含任何栈字节；不申请、不释放、不扣账 |
| `LainVmQuota` | `limit` `used` `peak` `charges` `releases` `rejected` `underflow` | 账户（执行级） | 驱动/测试 | 一次最外层执行一个；换 VSpace 不变 |

## 3 TCB 源码检索证据

```text
$ rg -n "calloc|free\(|lainvm_quota_charge|lainvm_quota_release" seed/src/vm/tcb.c
49:  tcb = (LainVmTcb *)calloc(1, sizeof(LainVmTcb));
64:  tcb->frames = (LainVmFrame *)calloc(frame_cap, sizeof(LainVmFrame));
65:  tcb->slots  = (L1Value *)calloc(slot_cap, sizeof(L1Value));
92:  free(tcb->frames);
93:  free(tcb->slots);
94:  free(tcb->resolved);
95:  free(tcb);
110: tcb->resolved = ... calloc(...)
```

即：剩下的分配只有 **TCB 自己、帧栈、值槽、能力解析表**（都是执行状态，
admit 时一次给够）；**没有栈的 calloc/free，也没有任何 quota 调用**。
`rg -n "stack_base|stack_size|stack_window" seed/src/vm/tcb.c` 无命中。

## 4 每组数字（最终）

| 组 | 条数 | 通过 | 失败 | 未定 |
| --- | --- | --- | --- | --- |
| region | 13 | 13 | 0 | 0 |
| stack | 6 | 6 | 0 | 0 |
| host | 4 | 4 | 0 | 0 |
| lifetime | 3 | 3 | 0 | 0 |
| quota | 8 | 8 | 0 | 0 |
| lea | 6 | 6 | 0 | 0 |
| addr | 3 | 3 | 0 | 0 |
| checked-ref | 18 | 18 | 0 | 0 |
| activation | 7 | 7 | 0 | 0 |
| lease | 26 | 26 | 0 | 0 |
| **合计** | **94** | **94** | **0** | **0** |

`build/vspace-tcb-final.json` 里 `"total": 94, "fail": 0, "blocked": 0`。

## 5 activation 生命周期：六条契约的实测

| 契约项 | 用例 | 修正前 | 修正后 |
| --- | --- | --- | --- |
| `if` 内 alloca，出 `if` 后同一过程访问 | `activation_if_survives` | **拒 1004**（错） | 值 42 |
| loop 体内 alloca，`continue` 后用上一轮地址 | `activation_loop_continue` | **值 12**（水位回退→复用同一地址，静默错值） | 值 11 |
| 被调过程返回后访问 | `activation_callee_return` | 拒 1004（对） | 拒 1004 |
| 根过程结束后访问 | `activation_root_done` | 拒 1004（对） | 拒 1004 |
| Trap 后活窗口归零 | `activation_trap_clears_window` | 窗口还在 | `accessible = 0`、`borrow_count = 1` |
| 同址重新授权后不可区分 | `lifetime_reuse`（R07） | 值 9 | 值 9 |

循环增长（实测，不每轮回收）：`#alloca[#bits<64>](1)` 每轮推进 **16 字节**
（8 字节数据 + 16 字节对齐），第 n 轮结束水位 = 16n − 8；容量 4096 时
250 轮成功（值 250）、1000 轮在**水位 4088** 处拒 1007。

## 6 改动前后 64 条语料比较

```text
check_meta.py compare build/vspace-tcb-before.json build/vspace-tcb-after.json -> 64/64 逐字节一致
check_meta.py compare build/vspace-tcb-preround.json build/vspace-tcb-after.json -> 64/64 逐字节一致
```

第二条是与**本轮开始前的提交 `5529060`** 单独建的 worktree 快照对比：整个重构
对语言可见行为零影响（这 64 条语料本来也不覆盖 `#alloca` 之外的新通路）。

## 7 诊断码与接口状态

| 码/状态 | 含义 | 归属 |
| --- | --- | --- |
| 1006 | 没有栈租约、或租约所属空间 ≠ 当前执行空间 | 运行期 Trap |
| 1007 | `#alloca` 超出区段 `capacity` | 运行期 Trap |
| 1035 | `#alloca` 尺寸算术回绕 | 运行期 Trap（原有） |
| 1036 | 栈活窗口登记/撤销失败（句柄失效 / 上界不可表示） | 运行期 Trap（原有，本轮的窗口接口仍用它） |
| 1044 | allocation quota 用尽（余额不足 / 加法溢出） | 运行期 Trap（原有） |
| `LainVmLeaseStatus` 0..7 | OK / NOT_IN_SPACE / BAD_HANDLE / NEEDS_READ_WRITE / WINDOW_NOT_ZERO / ZERO_CAPACITY / ALREADY_BORROWED / BORROW_FAILED | **接口层**，不占 1xxx 号段 |
| fold 9318 | 编译期执行要不到栈（`alloc_stack` 失败） | 接口层诊断（fold 自己的号段） |

VSpace 管理 API 用 `bool` / 句柄返回，不伪装成运行期 Trap。

## 8 仍未迁移的存储所有权

| 存储 | 现在怎么登记 | 后续 |
| --- | --- | --- |
| 映像的 ro / rw / code arena（`image.c`） | `EXTERNAL`：映像自己 calloc、自己 free | 将来若由 VSpace 拥有，改成 `alloc`/`free`；现在先走 external mapping，**不算重复扣账** |
| 宿主源码文本 / 路径 / 暂存区（`meta_boot.c`、`host.c`） | `EXTERNAL`；暂存区与输出扩容**计入 quota**（`lainmeta_host_attach_quota`） | 同上 |
| TCB 的栈 | `OWNED`，供给方 `alloc_stack` → TCB 借用 → 供给方 `free` | 已完成 |
| TCB 自己 / 帧栈 / 值槽 | 直接 `calloc`（admit 的执行状态） | 不在 VSpace 计账范围内；若要计入，需先定"execution state 算不算承诺存储" |

## 9 每个提交与实测数字

| 提交 | 内容 | 实测 |
| --- | --- | --- |
| `53d6a35` vm: VSpace 区分容量与可访问窗口 | `capacity`/`accessible` 分开、`set_accessible`、修掉自己引入的窗口边界下溢 | build OK；68 条 66/2/0（2 条是当时故意留的 activation 复现）；lease 4/4 |
| `e44c3a5` vm: VSpace 管理 owned storage 与 external mapping | `alloc`/`free`/`map_external`/`unmap_external`/`borrow`，删掉 `add`/`remove` 并迁移 21 处调用点 | build OK；81 条 79/2/0；lease 17/17 |
| `0c0e5fb` vm: TCB 改为借用栈租约 | 租约类型、校验+借用、TCB 不再分配/释放/扣账、Trap 清窗 | build OK；81 条 79/2/0；Tcb 无栈 calloc/free 与 quota 调用 |
| `e9f5cc0` vm: 修正 procedure activation 的 alloca 生命周期 | 区域退出不再回收；根结束/Trap 清窗；循环线性增长 | build OK；84 条 84/0/0；activation 7/7 |
| `383d565` test: 补齐 VSpace 所有权、租约和 activation 验收 | 8 条租约用例 + 独借规则 | 92 条 92/0/0；lease 25/25 |
| `docs: 固化 TCB 与 VSpace 分工及实测结果` | 本报告 + `docs/spec/vm.md` 的权威小节 | 94 条 94/0/0；check_docs 0 error |

## 10 工作区里保留、未提交的改动

- `docs/implementation/README.md`（文档一侧改过，**未动**）
- `scripts/check_docs.py`（未跟踪，**未动**）
- `build/` 下的快照与报告 JSON（生成物，永不提交）

## 附：最终验收命令与结果

```text
python scripts/build.py                                   BUILD OK
python scripts/check_vspace.py --report build/vspace-tcb-final.json
                                                          94 条：94/0/0
python scripts/check_meta.py                              19/19
python scripts/check_meta.py snapshot build/vspace-tcb-after.json
python scripts/check_meta.py compare build/vspace-tcb-before.json build/vspace-tcb-after.json
                                                          64/64 逐字节一致
python scripts/check_docs.py                              0 errors
负对照 4 条（成功值 / Trap 码 / borrow_count / quota used 各改错一条）  全部 exit=1
```
