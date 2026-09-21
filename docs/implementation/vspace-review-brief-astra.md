# VSpace / LAINVM 设计评审简报（给 Astra）

**性质**：写给外部评审的简报，不是结论。目的是让你**只读代码**就能判断下面这些设计问题。
每一条都带 `file:line`；我给的是「现状 + 分叉 + 后果」，判断留给你。
最后一节列出我实际跑过什么，方便你区分**实测**和**推断**。

**设计血统**（作者说明）：LAINVM 的 idea 来自 **seL4 这类微内核**——能力、地址空间、TCB、
Trap 分离，机制与策略不混。看下面每一条时，请把 seL4 的形状放在旁边对照。

---

## 0 边界：这份简报没覆盖什么

**读过全文**：`docs/04-lain-vm.md`（278 行）、`seed/include/lainvm/space.h`、`seed/src/vm/space.c`、
`bootstrap/**`（手写 Meta 那一层）、`seed/tests/meta_boot.c` 的 `main()` 与几个 report 函数。

**只读过片段**：`seed/src/vm/engine.c`（450–499，即 `op_load`/`op_store`/`op_alloca`）、
`seed/src/meta/host.c`（能力表那一段）、`seed/include/lainvm/tcb.h`。

**没读**：`engine.c` 其余约 1000 行、`tcb.c` / `image.c` / `caps.c` 正文、`docs/01-lain-ir.md`、
`docs/02-lain-ast.md`、`docs/03-meta-system.md`。

所以：凡涉及「实现现状」我只写我读过的行；凡涉及整体结构的，请你自己核一遍。**本文没有一行是跑 VM 跑出来的**
（见 §5）。

**评审时要守的边界**（仓库自己的规则，任何结论都要落在里面）：

- **不要动 `seed/src/vm/**`**：VSpace 那条停线项还开着，作者明确要求「定了才动」（`AGENTS.md` §5）。
- 语言语义不得通过改 `seed/` 偷渡；只有指令语义或宿主能力确实缺失才动 C。
- 诊断码约定：**0 = 成功，非 0 = 稳定诊断码**。
- TCB 对 LAINIR 是 opaque（`04-lain-vm.md` §4.1 :114）。

---

## 1 作者已经定下的方向（不是评审意见，是前提）

1. **TCB 与栈分开**：**VSpace 管理区域（栈那块也在里面）**；**TCB 只引用，不自带、不登记**。
   依据是 seL4 的形状：用户栈只是映射进 VSpace 的内存，内核不记这笔账。→ 见 §3.2。
2. **不要隐式拓宽 / 窄化**：字面量**不按值升宽**（那是 C 的规则）；类型来自上下文，无上下文默认
   `i32`；**越界是编译错误**。方向参考 Rust / modern C++（`as` / `static_cast`、列表初始化禁止
   窄化）。→ 与本简报 §4 的第 3 个例子相关。
3. **能力/权限那套以 `docs/04-lain-vm.md` §7（:171-180）+ §10（:261）的 CSpace/capability 设计为准。**
4. 标量算子表（`i64`/`i8`/`u8`/`usize` 现在是空的，`bootstrap/lain/std/prelude.lain:17-21`）
   按需补充，不算设计问题。

---

## 2 规范已经答了、C seed 没跟上（我认为这不是开放问题，是实现缺口）

| # | 规范要求 | 实现现状 | 证据 |
| --- | --- | --- | --- |
| 1 | `#lea` / `#load` / `#store` **三条路径**都做边界与权限检查 | `op_lea` 只算 `base + idx*scale + offset` 就把地址写进结果，**没有任何检查**。整个 `seed/src/vm` 里 `lainvm_space_check` 只有三处调用：`#load`、`#store`、`#call_indirect` | `04-lain-vm.md:88`；`engine.c:459` / `:476` / `:652`（唯一三处）；`op_lea` 正文 |
| 2 | 调用外部能力时，检查「**参数中的地址是否仍在允许的 VSpace 范围内**」 | `cap_emit_write` 拿调用方给的地址直接 `memcpy`，不看 VSpace | `04-lain-vm.md:177`；`host.c:256` |
| 3 | `allocation quota` 属于 TCB 的 `execution limits`，耗尽产生 quota Trap；最外层 `#eval` 建账户、嵌套 TCB 共用 | `grep quota` / `budget` 在 `seed/src/vm/**` 与 `seed/include/lainvm/**` **零命中** | `04-lain-vm.md:105` / `:162` / `:212-214` |
| 4 | VSpace 的**表示**是实现自定（受检 arena / 虚拟内存区域 / 页表） | —— 所以「句柄用下标还是用 id」规范不必回答，这是实现内部的事 | `04-lain-vm.md:261` |

---

## 3 规范没答、我认为需要作者定的（我按自己的轻重排）

### 3.1 句柄的形状：稳定句柄，还是数组下标？

```c
/* space.h */
typedef struct { uintptr_t base; uint64_t size; uint32_t rights; uint64_t owner; } LainVmRegion;
typedef struct { LainVmRegion regions[64]; uint32_t region_count; } LainVmSpace;

int32_t lainvm_space_add_region(...);       /* 返回**下标**（space.c:46-51，插完再按 base+size 找回来） */

/* tcb.c:56 —— TCB 把这个下标记下来 */
tcb->stack_region = lainvm_space_add_region(space, (uintptr_t)stack, stack_bytes, ..., tcb->id);

/* engine.c:491 —— 每次 #alloca 拿这个数直接索引 */
stack = &tcb->vspace->regions[tcb->stack_region];
```

而表会被搬动：`insert_sorted` 按 base 升序插入（`space.c:12-20`），`release_owner` 把后面的
元素前移（`:57-66`）。

**后果**（同一个 VSpace 里有两个 TCB 时）：表里已有 TCB A，它的栈在下标 3；给 B 建栈时
`add_region` 发现新 base 更低 → 插到下标 1 → A 的 `stack_region` 还是 3，但 `regions[3]` 已经
不是它的栈了。**没有报错**，A 下次 `#alloca` 写到别人的内存里。

规范 §8.4（`:227-247`）描述的 `execute_child` 正是「保留原 VSpace、建子 TCB」，子 TCB 需要自己的
alloca 栈区段（`tcb.c:50`/`:56`）——也就是说这条路一旦走，就会动表。

### 3.2 TCB 与栈的记账（与 §1.1 直接对应）

按「VSpace 管区域、TCB 只引用」这个方向，下面三处是反的：

```c
tcb.c:50   void *stack = calloc(1, stack_bytes);                       /* TCB 自己分配栈 */
tcb.c:56   index = lainvm_space_add_region(space, ..., tcb->id);       /* TCB 自己登记，owner=自己 */
engine.c:491 stack = &tcb->vspace->regions[tcb->stack_region];         /* 拿缓存下标取区段 */
```

`#alloca` 的边界今天是 TCB 拿 `stack->size` 自己判的（`engine.c:489-494`，trap **1006** = 没有栈
区段 / **1007** = 越界）。

### 3.3 回收的粒度：按整数 `owner` 扫表，还是按能力派生关系撤销？

```c
/* space.c:54-67 —— 删掉 owner 匹配的**全部**条目，后面的往前移 */
void lainvm_space_release_owner(LainVmSpace *space, uint64_t owner);
/* 调用点 tcb.c:84 */
lainvm_space_release_owner(tcb->vspace, tcb->id);
```

`owner = 0` 是保留值（`space.h:42`：「0 = 模块 / 装载器的」），所以 `release_owner(0)` 会把镜像
那两段一起删掉——今天没人传 0，纯靠自觉。

整数 owner 表达不了「这块是我派生给你的那一块」：父把一块内存给子 TCB 用完想收回时，写父的 id
会在父结束时连自己的东西一起删，写子的 id 会在子结束时把父给它的别的区段也删掉。seL4 的做法是
能力派生树 + `Revoke`（撤销的是「从我这支派生出去的全部」），归属关系就是派生关系，内核不需要
owner 字段。

### 3.4 配额的单位与执行点

规范说 TCB 有 step / call-depth / **allocation quota** 的「预算和消耗」（`:105`），最外层 `#eval`
建账户、嵌套共用、耗尽报 quota Trap（`:162` / `:212-214`），但**没说单位，也没说在哪扣**。

而且今天**没有「要内存」这个动作**：区段都是 C 里 `calloc` 之后 `add_region` 登记的；宿主给 Meta
的 11 个能力里跟内存有关的只有 `lain_meta_scratch_data` / `lain_meta_scratch_size`
（`seed/src/meta/host.c:318-319`）。两个候选形状：

- **A（微内核式）**：宿主把一块 untyped 交给 TCB，TCB 通过一个操作把它派生成区段，配额扣在这个
  操作上（「还能派生 8 个区段 / 4 KiB」）。
- **B（计数器式）**：内存全是宿主启动时给的，Meta 自己切，配额就是一个字节上限。

今天 Meta 侧那个碰撞式分配器（宿主暂存区内 mark/release）是 **B 的形状**，但它只在驱动侧读出
`peak`，**VM 不知道这笔账**。

### 3.5 `#lea` 该查什么

`op_lea` 今天完全不查，而规范要求查（§2 第 1 条）。难点是 **`#lea` 没有宽度**——它只产生一个地址，
不知道将来读几个字节：

```c
%p = #lea(%base, 0, 0, 4096)      /* 跨出区段 4096 字节 */

/* 候选 A：查 base 落在某个区内            → 放行（%p 可能已经在区外）
   候选 B：查算出来的 %p 仍落在合法区内     → trap
   候选 C：不查（今天），只在 load/store 查 */
```

关键差别：如果 `%p` 之后被交给宿主能力（例如 `emit_write`），**A 和 C 都会让宿主去读区外的东西**，
因为检查只发生在 IR 的 load/store 上。

### 3.6 能力参数的检查发生在哪一层

规范 §7（`:177`）要求查，实现里没有（`host.c:246-256`：`memcpy(host->out + ..., bytes, length)`，
`bytes` 是 Meta 给的地址）。三种落法：

- **A**：VM 在 `#extern` 前统一检查 —— 需要一张「这个能力的第几个参数是地址、多长」的声明表。
- **B**：每个能力自己查 —— 等于没有统一边界（今天）。
- **C**：不回传裸地址，递**能力 + 对象内偏移** —— 要动 ABI。

---

## 4 三个具体反例（说明「检查缺位时，上面那层必须自己做到完美」）

这三个都是我在 Meta 层（手写 LAINIR 那一层）实测到的**静默错值**：Meta 不报、LAINIR 验证器也不
报、算出来的值是错的。它们和 VSpace 的关系是：这类错本该有一类被底座的检查接住，而 §2 的四条缺口
意味着接不住，于是上面那层必须自己完美——那不可能。

1. **嵌套实参被内层覆盖**：`add3(g(1), g(2), g(3))` 曾发成 `#call add3(3, %r4, %r5)`，算 **6**
   （该 9）。原因是实参表只有一份固定缓冲区，降级实参时又递归进内层调用。
2. **函数体算子链丢尾巴**：`func f(...) -> i32 { return a + b + c; }` 曾只发 `%r2 = #add(%a, %b)`，
   `+ c` 静默消失。原因是函数体那条老路只发一个算子。
3. **字面量越界静默截断**：`let a: i8 = 300;` 跑出来 **44**、`let a: bool = 2;` 跑出来 **0**。
   （与 §1.2 的方向有关：现在改成越界报错，码 23。）

三条都已在 `seed/tests/` 留下钉住的语料，`python scripts/check_meta.py` 一条命令可复跑。

---

## 5 我实际跑过什么（实测 vs 推断）

**跑过**：

- `python scripts/build.py` → 绿（编译 Meta 驱动 + 冒烟 `meta_for.lain` → `main = 10`）
- `python scripts/check_meta.py` → **19/19**（值 + 拒绝码 + 产物文本断言）
- 64 条 `.lain` 语料的改动前后逐条对比（`git worktree` + `snapshot`/`compare`）→ **62/64 一致**，
  两处差异都是预期且都写在提交信息里

**没跑**：任何 VM / VSpace 相关的东西。`space.c` / `engine.c` 的结论全部来自**读代码**，没有一条
是执行出来的；本简报也没有给 VSpace 写任何测试。

---

## 6 想请你回答的问题

1. **§3 的六条里，哪几条必须现在定**（因为它会进 ABI / 影响后续所有 lowering），哪几条可以留到
   有真实消费者时再定？我自己的排序是 3.1 → 3.2 → 3.3 → 3.4 → 3.5 → 3.6，但这只是"读代码的直觉"。
2. **「TCB 只引用 VSpace、栈只是区域」这个分法，在解释器（而非微内核）实现里，`#alloca` 的边界检查
   该落在谁身上**：VSpace 的 `check`，还是 TCB 自己的栈指针判断？规范 §3.1（`:82-84`）/ §3.2（`:88`）
   的措辞能不能推出唯一答案？还是说它有意留白？
3. **§2 那四条「规范答了、实现没跟」里，哪一条最可能被后续改动放大成设计债？** 我怀疑是第 3 条
   （配额），因为它决定「内存从哪来」这个还没成形的接口；但你说不定看到别的。
4. 如果 §3.1 选「稳定句柄」，**`regions[64]` 这个定长数组 + 按 base 升序二分查找**（`space.c:69-87`）
   还留得住吗？（二分依赖有序；稳定句柄通常意味着槽不动、可能无序。）
