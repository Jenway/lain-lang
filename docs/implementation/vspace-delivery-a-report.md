# 交付 A 报告：VSpace 边界证据与决策包

**性质**：对应 `docs/implementation/vspace-action-plan-astra.md` 的**交付 A**。
不改 `seed/src/vm/**`，只新增测试源码与运行脚本，并把最小反例、推荐与实际改动面交出来。

对应文件：简报 `vspace-review-brief-astra.md`、方案 `vspace-action-plan-astra.md`。

**证据分级**：下面每一行都标了是实测还是读代码。凡实测都给了可复跑的命令。
**本文没有把方案的预期输出抄成实测**——凡是与我原先预期不符的地方，第 5 节专门列出来。

---

## 1 基线（实测）

从仓库根执行（方案 §3.1 的命令）：

```text
git rev-parse HEAD            ba6f4484d733c072193fc3436bdeb4c612bff5c3
git status --short            ?? docs/implementation/vspace-action-plan-astra.md
                              ?? docs/implementation/vspace-review-brief-astra.md
zig version                   0.16.0
python scripts/build.py       exit 0   BUILD OK
python scripts/check_meta.py  exit 0   用例：19/19 通过
python scripts/check_meta.py snapshot build/vspace-before.json
                              64 个用例 -> build/vspace-before.json
```

工作区里**没有未提交的实现改动**（只有上面两份未跟踪的文档），所以基线二进制与 HEAD 相同。
日志留在 `build/vspace-baseline/`（`build/` 不进版本库）。

---

## 2 新增的入口

| 文件 | 作用 |
| --- | --- |
| `seed/tests/vspace_checks.c` | 边界用例驱动。调公开接口（`space.h`/`tcb.h`/`caps.h`/`lainmeta/host.h`），不改 VM |
| `scripts/check_vspace.py` | 运行器：每条用例**独立子进程 + 超时**；崩溃/超时/解析失败都算失败 |
| `scripts/build.py` | 扩成构建**两个**驱动（`build/meta_boot`、`build/vspace_checks`），仍是唯一构建入口 |

```text
python scripts/build.py                                    # 构建（唯一入口）
python scripts/check_vspace.py                             # 全部用例
python scripts/check_vspace.py --group region              # 只跑一组
python scripts/check_vspace.py --case region_valid_read
python scripts/check_vspace.py --report build/vspace-first-run.json

# 断言机制的负对照（两条都必须退出 1，已实测）
python scripts/check_vspace.py --case region_valid_read --expect-value 99
python scripts/check_vspace.py --case load_outside_region --expect-trap 9999
```

输出每条一行、机器可解析：
`CASE <id> group=<g> expect=<spec> actual=<spec> result=<PASS|FAIL|BLOCKED> detail=<text>`。

退出码：**0** 全部通过 / **1** 有失败或未定 / **2** 缺前置或用法错误。
`BLOCKED` 单列并在汇总里单独报；**含 BLOCKED 的全量运行不返回 0**（方案 §3.2）。

**实测（本次）**：

```text
python scripts/check_vspace.py --report build/vspace-first-run.json
  合计 23：通过 10，失败 9，未定 4          exit=1
  未定：host_source_index_bounds, lea_construct_only, lea_wraparound, budget_unimplemented

负对照 1（--expect-value 99）      exit=1   ✓ 断言确实在生效
负对照 2（--expect-trap 9999）     exit=1   ✓

python scripts/check_meta.py snapshot build/vspace-after.json
python scripts/check_meta.py compare build/vspace-before.json build/vspace-after.json
  逐条对比：64/64 逐字节一致          exit=0
```

最后那条是「这次改动没碰语言行为」的机器证据：新增的都是测试与脚本。

---

## 3 R01–R08：全部复现（实测）

| ID | 构造 | 实测观察 | 结论 |
| --- | --- | --- | --- |
| R01 | 先登记高地址段，保存引用，再登记低地址段 | 引用 0 从 `base=1594503992352` 变成 `base=1594503990304` | **复现**：引用是下标，插入把它挪了 |
| R02 | 三段依次登记，移除第一段后用后两段的旧引用 | 引用 1（`base=2850270875680`）现在指向 `base=2850270876704` | **复现**：`release_owner` 前移，后面的引用全错位 |
| R03 | TCB 借用的栈引用，经历一次低地址插入 | 栈引用 1 从 `base=1644803736896` 变成 `base=1644803732800`（指到别人）；撤销插入后恢复 | **复现**（见 §6：这条第一次直接把进程打死了） |
| R04 | 输出能力读合法区段末尾、长度超出一字节 | `status=0`，输出长度变成 2 | **复现**：宿主按裸地址读了区段外的字节 |
| R05 | 调能力时 host 参数换成另一个同样可读写的 host 对象 | `status=0`，**写进了那个诱饵对象的输出**（长度 4） | **复现**：没有身份核对 |
| R06 | 子过程把 `#alloca` 地址写进模块级可写数据后返回，调用方再读 | 读到 **7**（陈旧值），没有拒绝 | **复现**：activation 结束后地址仍可用 |
| R07 | R06 之后再来一次同样的调用 | 两次拿到的地址**完全相同**（返回 1 = 同址） | **复现**：旧引用与新引用无法区分 |
| R08 | `#alloca[#bits<64>](0x2000000000000000)`：`8 × 2^61 == 2^64` 回绕成 0 | 过程**正常返回 0**，`#store` 成功 | **复现**：溢出后得到缩小（0 字节）的"合法"分配 |

复跑：`python scripts/check_vspace.py --group region|stack|host|lifetime`。
R04/R05 的最小构造就是那两条用例本身（`host_past_region`、`host_wrong_identity`），
不需要制造进程崩溃：越界读发生在一块真实分配的 4096 字节缓冲上，只超一字节。

---

## 4 已经站得住的边界（实测通过，10 条）

`region_valid_read`（区段内读返回 42）、`load_outside_region`（未登记地址读 → trap **1004**）、
`region_full_64`（64 段都进得去；第 65 段被拒且前 64 段不变）、`region_overlap_reject`、
`region_adjacent_allow`（半开区间，首尾相邻不算重叠）、`region_bad_size_reject`（size=0 与上界回绕都拒）、
`region_range_tail`（整段可读、越界一字节不可读、尾后起点不可读）、
`stack_absent_trap`（没有栈却 `#alloca` → trap **1006**）、
`stack_exhaust_watermark`（第二次 `#alloca` 要不下 → trap **1007**，且**水位不变**）、
`stack_zero_count`（零 count 被**验证器**拒：**2024** `#alloca count must be positive`）。

`stack_zero_count` 顺带说明一件事：方案 §5-B2.3 问「零 count 的语义先核对规范并登记决定」——
**语义其实已经定了**（验证器要求正数），引擎里那句 `count ? count : 1`（`engine.c:484`）
是够不着的死分支。要不要留那句、还是删掉，是个小清理，不是决策。

---

## 5 我改掉的**我自己的**两处错误预期

方案 §0 要求区分实测与推导，所以这两处必须写明 —— 它们不是实现的问题，是我算错了：

1. **`stack_exhaust_watermark` 的期望水位**：我先写成 **16**（默认第一次分配会对齐到 16 字节）。
   实际是 **8**：`#alloca[#bits<64>](1)` 要 1×8 字节，起点水位是 0，对齐后仍是 0，所以水位 = 8。
   修完之后行为**是对的**（失败那次没有动水位），这条从失败变成通过。
   期间还试过"先跑一个成功的过程来标定水位"，**行不通**：过程正常返回时水位会被 frame 的
   `stack_mark` 回退成 0（`tcb.h:86-95`），量到的是**跑完**的状态，不是失败前那一刻。
2. **`stack_zero_count`**：我原以为零 count 会被当成 1（读到 `engine.c:484` 的写法），
   所以把它标成 BLOCKED。实测是验证器先拒（**2024**），根本没跑到引擎。改成按 2024 钉住。

---

## 6 一条比预期更重的发现：R03 的后果是**堆损坏**

第一次跑 R03 时进程直接死了，退出码 `-1073740940` = `0xC0000374`（**堆损坏**）。机制（读代码）：

```c
tcb.c:48-74   TCB 创建：自己 calloc 栈 → add_region(owner = id) → 再扫一遍表把下标找回来
tcb.c:79-91   TCB 销毁：base = vspace->regions[tcb->stack_region].base;   ← 按缓存下标取地址
              lainvm_space_release_owner(vspace, id);
              free((void *)base);                                          ← 然后 free 它
```

也就是说：**表被挪动之后，销毁会把别人的地址当自己的栈 free 掉**。而 `tcb.c:64-73` 在创建时
已经写过一段补偿（注释还写着「区段表是定长的，插入后下标可能变；按 owner 找回自己的那段」）——
**同一个不变量，创建处补偿了，销毁处忘了**。这是「下标当句柄」最直接的代价：
每个用到它的站点都必须自己知道这件事，漏一个就是堆损坏。

测试驱动为此按要求「安全退出」：检测到错位后先撤销自己那次插入，让下标重新指对，再销毁。

---

## 7 决策包：推荐、证据、实际改动面

方案 §4 已经给了推荐。这一节做的是**把每条推荐配上实测证据与实际改动面**，供作者确认；
推荐本身不改，也不视为已解除 `seed/src/vm/**` 停线。

### D1 谁分配与释放栈（推荐 A：供给方分配、VSpace 登记、TCB 借用租约）

- 证据：R03 + §6（今天 TCB 自己 `calloc`、自己 `add_region`、销毁时按缓存下标 `free`）。
- 改动面：`tcb.c:48-74`（创建）、`tcb.c:79-91`（销毁）、`tcb.h:118-122`
  （`stack_region` / `stack_used` 两个字段的语义）+ 两个调用点 `meta_boot.c:347`、`fold.c:350`
  （都写死 `id=1`）+ `engine.c:482-499`（`#alloca` 用缓存下标取区段、自己判上界）。
- 待确认：租约里要不要带「可用偏移/容量」（今天 `stack_used` 在 TCB 里），
  以及「有活借用时拒绝释放」要落在哪一层。

### D2 调用结束后的地址失效（推荐 A：保留裸地址，评估期内不复用 + 活动分配范围）

- 证据：R06（返回后仍读到 7）、R07（两次调用拿到同一地址）。
- 改动面：**A** = `LainVmFrame.stack_mark`（`tcb.h:86-95`）+ `engine.c` 里**所有**回退水位的点
  （要先把它们枚举齐，不能只改 return）+ 访问路径加「活动分配范围」检查；
  **B**（地址带身份/代数）= `LainIR` 的值表示（`lainir/value.h`）、`#load`/`#store` 的编码、
  `#ptr2int`/`#int2ptr`、宿主适配、C 后端与序列化 —— 这是全部条目里最大的一个。
- **安全目标必须先写明**：A 只能保证**一次评估内部**的失效访问被拒；跨评估、或宿主保存了
  地址以后再用，A 给不了完整时间安全。这条不定，"选 A 还是 B"就没有对错。

### D3 lea 的语义（推荐 A：lea 保持地址算术，实际访问统一受检）

- 证据：`lea_construct_only`（构造区段外 +1000000 地址**没有被拒**，过程正常返回）、
  `lea_wraparound`（`idx=0xFFFFFFFFFFFFFFFF, scale=8` 回绕成 `18446744073709551608`，也没被拒）。
- 今天的状态：`op_lea` 只算 `base + idx*scale + offset` 就写结果，**一处检查都没有**
  （`seed/src/vm` 里 `lainvm_space_check` 只有三个调用点：`engine.c:459`/`476`/`652`）。
- 待确认：规范 `04-lain-vm.md:88` 写的是「`#lea`、`#load`、`#store` 的执行路径上都提供检查」。
  选推荐 A 等于**调整这句话**——这是规范改动，必须作者批准，不能由实现单方面决定。
- 改动面：选 A 基本不用改 `op_lea`，但要把「访问统一受检」补齐（含宿主与未来的原子路径）；
  选 B 还要在 `space.h/c` 上加「结果仍落在合法区内」的查询，并定义尾后值、零长范围、回绕。

### D4 宿主边界（推荐 A：能力条目带受信任签名 + VM 调用适配器）

- 证据：R04（读越界，`status=0`）、R05（换 host 对象照样接受并写进它）。
- 改动面：`caps.h`/`caps.c`（条目要能承载受信任签名与绑定身份）、`engine.c` 的 extern 调用路径、
  `meta/host.c` 的**全部 11 个能力**（不能只修 `emit_write`）、注册驱动、后端对应入口。
- 待确认：适配器拿到的「调用上下文」里放什么（当前空间、预算、能力绑定）；
  以及「host 身份」怎么和注册时绑定（`args[0]` 今天是裸指针，没有可核对的凭据）。

### D5 配额（推荐 A：以实际承诺的内存字节为 live-memory 配额，释放归还）

- 证据：`budget_unimplemented` —— 规范要求（`04-lain-vm.md:105`/`:162`/`:212-214`），
  实现里 `grep quota|budget` 在 `seed/src/vm/**` 与 `seed/include/lainvm/**` **零命中**。
- 改动面：`tcb.h:105` 那组 `execution limits`（今天只有 `steps`/`fuel`）、`engine.c` 的分配点、
  `tcb.c:39-56`（帧/槽/栈的分配）、`host.c` 的 scratch 与输出增长点。
- 待确认：单位（字节/对象数/次数）与扣费点；以及「预装的只读 artifact/source」算不算账内。

---

## 8 需要作者确认的清单

1. **D2 的安全目标**：只保证一次评估内部，还是要跨评估也保证？（这一条决定 A/B 的取舍，而且是
   全部条目里改动面最大的一个。）
2. **D3**：是否批准把 `04-lain-vm.md:88` 的 lea 检查要求改成「算术不受检、访问受检」。
3. **D1/D4/D5 的推荐**是否确认（可分别确认、分别推进）。方案 §7 要求：确认之后要在计划与
   `AGENTS.md` 里写明**被解除的停线范围**，不要把整个目录限制一次性默认取消。
4. 新拒绝码的登记：`stack_count_overflow` 现在期望「必须拒绝」但**还没有码**（等实现时定，
   不预先占号）。`host_*` 两条的「拒绝」走的是 acp 返回非零，也没有 VM 侧分类。

---

## 9 未覆盖 / 未定（不许当成通过）

| 项 | 为什么现在测不了 |
| --- | --- |
| `host_source_index_bounds` | 越界索引的安全复现需要一块受控的"表尾哨兵"，当前 host 是 opaque 的，读一格就越界。**未实施** |
| 原子 / 读改写路径 | `xchg`/`cmpxchg`/`rmw_add`/`rmw_sub`/`rmw_and`/`rmw_or`/`rmw_xor` 在 `engine.c:1055-1061` 注册为 `LAINVM_OP_STUB(...)` → `op_unimplemented`。**这几条路径不存在**，不是漏检；实现时必须一并过 VSpace。向量算子（`engine.c:1039-1054`）同理 |
| C 后端 / 序列化 | 只测了解释器路径 |
| 多 TCB / 多 host 身份 | 只测了同一个 VSpace 里的单 TCB 与两个 host |
| ASan/UBSan | 没跑（方案 §6 要求：跑不了就明确标记未运行，不许写成通过） |
| 跨评估的地址安全（D2-B） | 依赖 D2 决策 |

---

## 10 下一步（交付 B 的顺序，等第 8 节的确认）

方案 §7 的顺序照抄：测试与决策包（**本次**）→ 稳定区段/租约（B1/B2）→ 宿主边界（B3）→
生命周期（B4）→ 预算（B5）→ lea/规范收口（B6）。每一阶段先跑对应专项组 + Meta 回归；
涉及共同访问 helper 时跑全部已启用的 VM 组。**不要把语言字面量策略或 Meta parser 重构混进这些提交。**
