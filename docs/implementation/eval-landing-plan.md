# `#eval` 落地计划

目标：把「编译期执行」从 `#call` 上的一个布尔标记，做成设计里的 `#eval { ... }` 块 ——
有静态物理类型、能读外围自由局部、在独立 TCB 里带预算执行、backend 前消失。

边界：本计划**不新增 `seed/src/vm/**` 的接口**，`seed/src/vm/**` 保持暂停状态。
块在执行前由折叠阶段 lowering 成临时根过程，走既有的 `lainvm_tcb_start` + `lainvm_engine_run`。

---

## 0 现状（实测）

| 事实 | 出处 |
| --- | --- |
| `#eval` 现在是 `L1Inst.is_eval`（bool），**不是操作码**；注释写明「Meta 发出 -> folding 消掉；backend 永远看不到」 | `seed/include/lainir/core.h:227-229` |
| LAINIR 文本解析把 `#` 后 `[a-z0-9_]*` 当操作码查表，`eval` 不在表里 → **3002** | `seed/src/core/parse.c:644-651` |
| 打印器把带标记的调用打印成 ` #eval`，解析器不认 → **带 eval 的模块 print→parse 必失败**（今天无 fixture 撞到） | `seed/src/core/print.c:292` |
| 验证器只查一件事：实参必须编译期已知，否则 **2012** | `seed/src/core/verify.c:417-423` |
| fold 有调用形式的完整实现：9301 无被调、9302 不在模块、9303 实参过多、9304 实参非常量、9305 起不来/宿主拒绝、9306 没跑完、9307 无结果、**9308 结果是地址** | `seed/src/fold/fold.c:147-207` |
| fold 的预算参数只有 `max_call_depth`/`stack_bytes`/`fuel`（**无「每模块块数」**） | `seed/include/lainfold/fold.h:33-34` |
| fold **不在主线**：`lainfold_module` 的唯一调用者是 `seed/tests/fold_eval.c:234/299`；`seed/tests/meta_boot.c` 里没有 fold | grep 实测 |
| 后端见到 eval 残留直接拒 **9225** | `seed/src/backend/cbackend.c:1241` |
| `execute_child`（归档里 `#eval` 的 VM 原语）在当前 seed **零命中** | grep 实测 |
| 块要用的载体已经存在：`L1Region{params,param_count,results,result_count,insts,inst_count}`、`L1Inst.body`、`L1Inst.ty/has_ty` | `core.h:167-174`、`:233`、`:209-210` |
| 「有结果类型的块」定型机制已经存在：`yields_values()`；被 `#if`/`#loop`/`#switch` 使用，错误码 `L1V_MISSING_YIELD` | `verify.c:257-268`、`:581`、`:629`、`:693` |

## 1 设计要求 → 落地形式 → 验收

设计要求出自归档设计 `docs/history/2026-09-21/docs/01-lain-ir.md:136-163`（原样保留，不改）。

| 设计要求 | 落地形式 | 验收 |
| --- | --- | --- |
| `#eval { ... }` 是有静态物理类型的块；验证器把块视为返回该预期类型的匿名过程（:138、:154-156） | 新指令种类 `INST_EVAL`，`body` 指向一个区域，区域的 `results[]` 声明块的类型；验证器对块跑 `yields_values`，并检查每条可达 `#return` 的类型 | 正例 1 条；反例 3 条：缺 return、返回类型不符、块类型未知 |
| 块可读外围自由 `%local`；lowering 把自由变量作为临时根过程**按值**传入；块不持有调用者 frame（:158-159） | 折叠阶段按值**捕获**：块被 lowering 前，块体里对外围名字的引用换成第一遍折出来的字面量（块内自己绑的同名名字优先）；换成不掉的（运行期才知道的）拒 **9323** | 正例两条：块读已折叠的常量 → 产物执行得 7；块内同名绑定压过外围 → 得 3。反例一条：块读过程参数 → 拒 9323 |
| 捕获 `#addr` 只复制地址值，仍受原区域与 activation 生命周期约束（:159-160） | 不需要新机制：这一条已由本轮的 activation 生命周期修正保证（逃逸/复用实测拒 **9207**） | 块捕获一个地址 → 块内可用；调用者返回后再用 → 拒 |
| 执行 `#eval` 的 TCB **不自带 VSpace，也不隐式继承调用者地址空间**；用捕获地址需显式授权（:160-161） | 折叠阶段为块单独建一个 TCB（独立 space + 栈租约 + quota），不把调用者 space 传进去 | 块用未授权地址 → 拒；显式授权后 → 通过（见 D5） |
| 预算：step count / call depth / 累计 `#alloca` 字节数 / 每模块 eval 块数量（:163） | 前两项已有（`fuel`、`max_call_depth`）；第三项现在由**栈租约容量**真实约束；第四项待建（见 D4） | 各自的拒绝码：超 fuel、超深度、超容量（1007）、超块数 |
| 正常结束产生已验证的普通 LAINIR 值，失败产生 Trap，Trap 不属于表达式值（:139-140） | 折叠把结果值写回产物；Trap 转编译诊断，不写值 | 成功值断言；Trap 用例断言诊断码而不是值 |
| `#eval` 必须在 backend 前执行并消失（:142） | 主线在 lowering 之后、backend 之前调用折叠 | 产物文本里 `#eval` 计数为 0；后端 9225 不可达 |
| 块内可用 `#addr`，但不得把仅编译期有效的地址当作产物结果（:151-152） | 见 D3 | 逃逸用例必须被拒（结构化禁止或运行期拒绝码） |

## 2 切片

每片独立可验收；顺序即依赖顺序。S1 不涉及任何待定项。

### S1 文本表示与往返（调用形式）

- **改**：`parse.c` 的操作码表加 `eval`（等价 `#call` + `is_eval`）；`print.c:292` 的写法与解析器对齐；两者对同一份文本必须互为逆。
- **不改**：IR 结构、验证器、fold、VM。
- **验收**：`%s = #eval f(1)` 能解析并验证；打印后再解析得到**逐字节相同**的规范文本（今天这条不成立）；把 `#eval` 写成未知拼写仍是 3002（负对照）；`check_meta.py` 19/19 与 64 条语料快照逐字节一致。
- **风险**：打印位置（` #eval` 在名字前还是调用后）必须只有一个权威写法，parse 与 print 用同一张名字表。

- **状态：已完成。** 落地与原文的差异：`eval` **没有**进 `k_ops[]`——那张表是「拼写 → 种类」，
  而 `call`/`eval` 是同一个种类，放进去会让 `lainir_opcode_name(INST_CALL)` 的语义含糊；改为在
  `opname.h/.c` 给出权威常量 `LAINIR_OPCODE_EVAL`，parse 与 print 共用它。拼写落在**名字位**：
  `%v = #eval one(1)`；行尾标记 ` #eval` 只留给非直接调用的种类（`#call_indirect`）。旧打印形式
  `#call f(1) #eval` 从此不再被接受——它本来就不能往返（打印器输出、解析器不认）。
  实测：`build/text_roundtrip.exe` **28 项 ALL PASS**（解析 + 验证、规范文本是 `#eval one(1)`、
  固定点、带名字实参拒 **2012**、`#evall` 拒 **3002**）；负对照：同一份测试拿到 HEAD 的 parser 上
  编跑 → `FAIL：#eval one(1) 解析成功`、`diag: 3002:6 unknown opcode '#eval'`、exit=1。
  回归：build BUILD OK、`check_meta` 19/19、`check_vspace` 94/94、64 条语料快照 vs HEAD
  `64/64 逐字节一致`、`check_docs` 39 files/115 links/0 errors。

### S2 块的 IR 与验证器

- **改**：`core.h` 在 `INST_COUNT`（`:140`）前加 `INST_EVAL`；`print.c`/`parse.c` 的名字表同步；`verify.c` 加分支——块按匿名过程定型（复用 `yields_values`，`#return` 的类型必须是区域声明的结果类型）；`engine.c` 与 `cbackend.c` 对 `INST_EVAL` 一律**拒绝**（引擎不该见到它，后端 9225 一类的稳定码）。
- **不改**：fold、VM 结构。
- **验收**：块正例 + 三个反例；`#eval` 块在文本里 print→parse 往返一致；引擎/后端遇到块是**稳定拒绝**而不是静默错值。
- **风险**：**枚举加值必须六处对齐**（名字表、parse、print、verify、fold、backend/engine）。名字表若按枚举下标索引，插在中间会平移全部下标——实施时先确认表是名字查找还是下标索引，再决定插位置。漏一处的表现是静默错值，不是崩溃。

- **状态：已完成。** `INST_EVAL` 加在枚举**最后**（`INST_RETURN` 之后、`INST_COUNT` 之前），
  不下移任何下标；名字表是名字线性查找（不是下标索引）。对齐到的八处：`core.h`（枚举）、
  `build.h/.c`（`lainir_inst_eval`）、`opname.c`（名字表）、`parse.c`（`#eval` 后看第一个字符
  分流：标识符 → 调用形态、`->`/`{` → 块）、`print.c`（`#eval -> (T) { ... }`）、`infer.c`
  （结果类型取自块的声明）、`verify.c`（块按匿名过程定型）、`engine.c`（`op_eval_block` → trap
  **1045**）、`cbackend.c`（**9225**）。新增诊断码 **2030**（块结果含 `#addr`）。
  验收入口：`seed/tests/eval_checks.c` + `scripts/check_eval.py`（已接进 `scripts/build.py`），
  **9/9 通过**；负对照四条（错码 / 错文本期望）全部 exit=1。回归：build BUILD OK、
  `check_meta` 19/19、`check_vspace` 94/94、64 条语料快照 vs S1 提交 `64/64 逐字节一致`、
  `check_docs` 0 errors。
  **未做（属 S3）**：折叠阶段还不认 `INST_EVAL`——`fold.c` 只处理 `inst->is_eval` 的调用，
  所以今天块只能"进得来、验得过、跑不了"（引擎 1045、后端 9225 都是拒绝）。

### S3 折叠执行块

- **改**：`fold.c` 加块路径——把块 lowering 成临时根过程（自由变量 → `params`，按值传入），在**独立的 TCB**（自己的 space + 栈租约 + quota + `max_call_depth`）里 `lainvm_tcb_start` + `lainvm_engine_run(fuel)`，结果写回产物；Trap 转诊断。
- **不改**：`seed/src/vm/**`。
- **验收**：块算出正确值并消失（产物里 0 个 `#eval`）；块内 `#alloca` 超出租约容量 → 1007；Trap → 稳定诊断码；地址逃逸按 D3 的裁定被拒。
- **风险**：块 lowering 需要一份「临时根过程」——实施时确认 `LainFold` 是否独占 image（今天 `run_eval` 用 `f->image` + `f->tcb`），若是则要么在 load 前把合成过程放进模块副本，要么每次执行重建 image。这一步不要顺手改 VM API。

- **状态：已完成。** 落地：`fold.c` 在**装载之前**扫出所有 `INST_EVAL` 块（`collect_module_blocks`），把每块 lowering 成
  `__eval_block_<i>` 临时根过程（`params` 空、`results` 取块声明），并造一份**含这些过程**的模块给 image 用
  （image 持有子过程指针，所以这份模块与子过程数组活到 image 释放）；输出模块仍只含原模块的子过程。
  执行走**每次 eval 一份账户 + 一个自己的 TCB**（D4 = 乙、D5 = 甲）：块拿不到调用者的地址空间，
  只看得见 fold 自己的空间（映像 + fold 的栈），并受 `fuel`/`max_call_depth`/租约容量约束。
  Trap 把**引擎那枚稳定码原样报上来**（1005/1007/1044…）而不是压成 9306——调用形式今天仍是 9306，
  这处不对称是有意的，统一放到 S6。新码：**9319** 块太多/合成失败、**9320** 块嵌块（引擎执行外块时会撞见
  内块 → trap 1045，所以直接拒）、**9321** 块带参数（自由变量按值传入今天没有来源，拒绝而不是猜值）、
  **9322** 块没被 lowering（内部不一致）。
  实测：`eval_checks` 从 9 条到 **12/12 通过**（新增 `block_folds`、`block_alloca_over`、`block_nested`）：
  `block_folds` 折叠后**执行产物**得到 7（只验"折掉了"不够，静默错值正是这么活的）、产物文本里 0 个 `#eval`、
  折叠产物**重新通过验证器**；`block_alloca_over` 租约 64 字节、块内要 800 → **1007**；
  `block_nested` → **9320**。负对照 3 条（错码 1006 / 错文本 `#eval` / 错码 9321）都 exit=1。
  回归：build BUILD OK、`check_meta` 19/19、`check_vspace` 94/94、64 条语料快照 vs S2 提交
  `64/64 逐字节一致`、`text_roundtrip` 28 项 ALL PASS。
  **未做**：块内嵌套块、C 后端（仍 9225）、主线接线（S5）。

### S3b 块的按值捕获（自由变量）

- **改**：`fold.c` 分**两遍**——第一遍只折调用形态（`#eval f(...)`），第二遍才 lowering 并执行块。
  顺序是语义要求：块要按值捕获外围**编译期已知**的值，而那些值来自第一遍
  （`%k = #eval one(7)` 之后块里读 `%k`）。lowering 前对块体做一次捕获替换：外围名字有已知值
  → 换成字面量；块**自己绑**的同名名字不动（块内优先）；两者都不是 → 拒 **9323**。
  两遍各起一次执行环境（image 不可变，"让临时过程进 image"只能再装载一次：第一遍装载输入模块、
  第二遍装载含临时过程的模块）。这条顺序是**必须**的：第一遍就把块体当普通区域重写过一次，
  如果那时不认"块根"，块内的局部会被外围的同名折叠值顶掉（实测过：块内 `%k = 1+2` 被换成
  外围的 7，产物算出 7）。
- **不改**：`seed/src/vm/**`、解析器、验证器。
- **验收**：`block_captures_folded`（块读已折叠常量 → 折掉 2 次、产物 0 个 `#eval`、
  产物执行得 **7**）、`block_capture_shadow`（块内 `%k = 1+2` 压过外围 `%k = 7` → 产物执行得 **3**）、
  `block_captures_unknown`（块读过程参数 → 拒 **9323**，诊断里带那个名字）。
- **状态：已完成。** 实测 `eval_checks` **15/15**；负对照 4 条 exit=1，其中
  `block_capture_shadow --expect-text "#return 7"` 正好卡住块内优先这条规则（替换若串了，
  产物就是 7、这条对照反而会通过）。回归：build BUILD OK、`check_meta` 19/19、
  `check_vspace` 94/94、64 条语料快照 vs S3 提交 `64/64 逐字节一致`。
- **未做**：块的**声明式参数**仍拒 **9321**（块区域今天解析出来就是零参数，`parse.c` 传的是
  `parse_block(p, NULL, 0, ...)`）；真把自由变量做成临时根过程的 `params` 要等源语言拼写（D6）。

### S4 Meta 产出

- **改**：`bootstrap/` 的 v0 源语言接受 **`eval g(1)`**（表达式前缀，拼写与理由见 D6），
  Meta lowering 把它发成 LAINIR 的 `#eval g(1)`（**不是** `#call`）；块形式（`#eval -> (T) { ... }`）由
  Meta 生成留给后续，本期只做调用形态——它已经能折叠、能执行、能过后端（S2/S3/S5）。
- **验收**：源文件 → LAINIR 文本里出现 `#eval`；实参非常量的拒例（2012）。
- **风险**：Meta 是手写 LAINIR，暂存区布局以 `bootstrap/std/scope.l1` 文件头为准；新增格子要同步容量检查。

- **状态：已完成（只做绑定位置）。** 实测（2026-09-21）：
  - `bootstrap/std/lex.l1` 加词数据 `meta_kw_eval`（`"eval"`，4 字节，与 `meta_kw_func` 同一段）；
    `bootstrap/std/emit.l1` 加 `meta_hash_eval`（`"#eval "`，与 `"#call "` **等长** 6 字节）。
  - `bootstrap/std/funcs.l1`：`meta_emit_binding_call` 多一个 `%is_eval: #bits<1>`，前缀用一次
    `#if` 在 `#call ` / `#eval ` 之间选（发射器其余部分一个字没动，两趟顺序没碰）；
    `meta_lower_stmt` 在形状分派**之前**判标记——`%v` 是内容为 `eval` 的词、且下一个兄弟是
    种类 5（一次调用）→ 拿那个兄弟当被调者、它的下一个兄弟当实参表，传 `%is_eval = 1`。
  - **没有新增暂存区格子**（只多两份数据串），所以 `scope.l1` 文件头的布局与容量检查不用动。
  - 产物（`build/meta_boot.exe seed/tests/meta_eval_call.lain main 2 '' canonical
    std/prelude.lain=bootstrap/lain/std/prelude.lain`）里是 `%y = #eval g(1)`；
    默认模式折叠后 `main() = 2` → `ALL PASS`。
  - 反例一（实参是参数）：`%y = #eval g(%a)` → 验证器
    `2012 line 5 #eval argument 0 is not compile-time known`。
  - 反例二（返回位置 `return eval g(1);`）：Meta 干净报 **4**。
  - 只支持绑定位置是**有意的**：顶层 `let main: i32 = eval g(1);` 走的是「合成过程 +
    `#return #call …`」那条路，`#eval` 会被当成内联操作数发出去，而折叠阶段认的是**指令**——
    要支持得先改 `meta_emit_call` 的收尾形状（先落临时值再 `#return`）。留作后续。
  - 验收：`check_meta` **22/22**（新增 `eval_call`、`reject_eval_unknown_arg`、
    `reject_eval_return_pos`）、`check_vspace` 94/94、`eval_checks` 16/16、语料快照 vs S5 提交
    **64/65 逐字节一致**（唯一差异是新增的 `seed/tests/meta_eval_call.lain`）、`check_docs` 0 errors。

### S5 主线接线

- **改**：驱动在 lowering 之后调用折叠，再产出后端输入；折叠后**重新验证**一遍（折叠产物仍须通过验证器）。
- **验收**：端到端正例（源 → 折叠 → 后端/执行结果正确）+ 反例（未折叠残留被拒）。
- **风险**：折叠引入的新阶段要有明确入口，不能在驱动里散着调。

- **状态：接线已完成（端到端正例见 S4）。** 入口是 `seed/tests/meta_boot.c` 的 `fold_evals`
  （在解析 + 验证之后、装载/后端之前）：
  - **执行路径**与 **`-c`（出 C）路径**都走它；`canonical` 模式**不折**——它打印的是 Meta 的
    产物，不是后端输入。
  - `module_has_eval` 先扫一遍：**没有 `#eval` 就原样返回**，不起执行环境（装载 + 栈租约 + TCB），
    所以语料零代价、零新失败面。
  - 折叠产物**重新验证**一遍再往下走；失败写 `fold:` / `folded verify:` 前缀的诊断。
  - 宿主能力用同一份 `caps`（`caps` 的创建因此挪到 `prepare(meta)` 之前）。
  - 预算从环境读（S6 已完成）：`LAIN_FOLD_DEPTH`=64 / `LAIN_FOLD_STACK`=4096 /
    `LAIN_FOLD_FUEL`=1000000 / `LAIN_FOLD_QUOTA`=0（不限额）/ `LAIN_FOLD_BLOCKS`=0（不限块数）；
    值不是十进制数就退回缺省（不静默当 0，那等于把预算悄悄设成最小）。
- **已验**：`check_meta` 19/19、`check_vspace` 94/94、`eval_checks` **16/16**、
  64 条语料快照 vs S3b 提交 `64/64 逐字节一致`（无 `#eval` 时折叠阶段原样返回）。
  新增 IR 级组合用例 `folded_backend_ok`：折叠后的产物能进 C 后端，而**同一份模块不折叠时**
  后端拒 9225（`block_backend_rejects`）——这就是"折叠必须在后端之前"的验收。
- **未做**：端到端（源自 v0 源语言）要等 **S4**：Meta 今天还产不出 `#eval`（拼写见 D6）。

### S6 预算与策略

- **状态：已完成。**
- 每模块 eval 块数量：`lainfold_set_block_limit(fold, limit)`，**0 = 不限**（设计原话，也是默认）；
  数组容量 `FOLD_MAX_BLOCKS`（64）是硬底——上限高于容量时按容量算，超了报 **9319**。
- 预算不再写死在库里：`seed/tests/meta_boot.c` 的 `fold_evals` 从环境读
  `LAIN_FOLD_DEPTH` / `LAIN_FOLD_STACK` / `LAIN_FOLD_FUEL` / `LAIN_FOLD_QUOTA` / `LAIN_FOLD_BLOCKS`，
  缺省 64 / 4096 / 1000000 / 0（不限额）/ 0（不限块数）。
- **调用形态的 trap 码与块形态统一**：`run_eval` 里引擎 trap 时把引擎那枚稳定码**原样**报成诊断码
  （与 `run_eval_block` 一致）；**9306** 只留给「跑完了但不是 DONE、也不是 trap」的情形（今天就是 fuel 耗尽）。
- **已验（2026-09-21）**：`eval_checks` **19/19**（新增 `eval_call_traps` 拒 1007、`block_limit_off` 通过、
  `block_limit_reached` 拒 9319；负对照 `--expect-verify 9306` 与 `--expect-verify 9318` 都 exit=1）；
  `check_meta` 24/24、`check_vspace` 94/94、语料快照 vs S4 提交 `65/65 逐字节一致`、
  `text_roundtrip` 28 项 ALL PASS、`check_docs` 0 errors。驱动层实测：
  缺省 → `main() = 2` ALL PASS exit=0；`LAIN_FOLD_FUEL=1` → `fold: 9306` exit=1；
  `LAIN_FOLD_QUOTA=1` → `fold: 9318` exit=1。

## 3 待作者裁定

每条给两个方案 + 我的建议，**不自行选择**。

**已裁定（作者）**：D1 = **甲**（块是 IR 指令，验证器按匿名过程定型）、D2 = **甲**（块自己声明
结果类型）、D3 = **甲**（块的结果类型不得含 `#addr`）—— 都按甲实施。**D4 = 乙**（每次 eval 一份账户）、
**D5 = 甲 + 乙并存**（执行用独立临时空间，捕获地址要能用必须显式授权）—— S3 按这两条落地。

- **D1 块进不进 IR。** 甲：块是 IR 指令（`INST_EVAL` + 区域），验证器按匿名过程定型——设计原样。乙：块只作前端构造，lowering 阶段就变成「临时根过程 + 调用」，IR 不加新种类——改动最小，但块在 IR 层不可见、验证器不能单独验块。
  **我建议甲**（设计原文就是「验证器把块视为匿名过程」，且 S2 的载体已存在），但甲需要动 LAINIR 物理语义，属要先讨论的一类。
- **D2 块的类型来源。** 甲：区域自声明结果类型 + 使用点检查（沿用 `#if`/`#loop`/`#switch` 的现有机制）。乙：按设计原话做使用点推断（块的类型由消费它的表达式位置决定）。
  **我建议甲**，理由：机制已存在、诊断更强；乙要求验证器对每条指令做期望类型传播，今天没有任何指令是这么定型的。
- **D3 `#addr` 逃逸。** 甲：结构化禁止——块结果类型不得含 `#addr`（块内随便用）。乙：允许结果带地址，靠运行期检查兜（编译期空间的地址在产物空间未授权 → 首次使用拒）。
  **我建议甲**，理由：静态可判；乙对经 `#ptr2int` 洗成整数的值不成立，只能算「用的时候会炸」。
- **D4 预算归属。** 甲：整次编译一份 quota 总额。乙：每次 eval 一份。
  **我建议乙**（一次编译期执行一个账户，超支的诊断能指到那一次）；「每模块块数」的上限建议由**验证器**数（静态量），折叠阶段只消费它。
- **D5 eval TCB 的空间来源。** 甲：独立临时空间（fold 今天就是这么跑调用形式的）。乙：要求调用方显式授权捕获地址所在的空间。
  **我建议甲 + 乙并存**：执行用独立空间（满足「不自带、不隐式继承」），捕获地址要能用就必须走显式授权（满足「还需显式授权相应的内存能力」）。两条不冲突。
- **D6 v0 源语言的拼写。已定：`eval g(1)` 作为表达式前缀**（2026-09-21 作者裁定，见下）。
  - 原甲 `#eval f(1)` **在 v0 源码里写不出来**：`#` 到行尾是注释。
    证据：`bootstrap/std/lex.l1:73-101` 的 `bs_skip_space` —— 字节 ≤ 32 是空白（:85-89），
    字节 **35 = `#`** 就调 `bs_skip_line` 跳到行尾（:90-94）。这段注释本身也写明注释是必须的
    （没有它，注释里出现的 `scalar` 字样会被当成声明扫进来）。
  - 最小复现（探针不进版本库）：把
    `func g(x: i32) -> i32 { return x + 1; }` /
    `func f() -> i32 { let y: i32 = #eval g(1); return y; }` /
    `let main: i32 = f();` 存成 `build/probe_eval.lain`，跑
    `$env:LAIN_META_MANIFEST='bootstrap/SOURCE_ORDER'` +
    `build/meta_boot.exe build/probe_eval.lain main 2 '' canonical std/prelude.lain=bootstrap/lain/std/prelude.lain`
    → 实测 `meta: 122058 steps, host status 4`、exit 1：Meta 在 `let y: i32 = ` 处停住，
    后半行（含 `return y;`）被当注释吃掉。LAINIR 侧不受影响——它有自己的 parser，`#eval` 照用。
  - 因此拼法定为 **`let y: i32 = eval g(1);`**：与 LAINIR 的 `#eval g(1)` 同形状、只去掉 `#`，
    能嵌在表达式里（`1 + eval g(1)`）。备选是 `eval y: i32 = g(1);`（带名字的语句形式，
    不能嵌在表达式里）——未采用。
- **D7 `#alloca` 的 count 按元素类型宽度读（S3 实测发现，需要裁定，尚未实施）。**
  - 最小复现：`%p = #alloca[#bits<8>](256)`，接着 `#store[#bits<64>](42, %p)` → 拒 **1005**；实测这一次
    `#alloca` 只把水位推了 **1 字节**（`tcb->stack_used == 1`）。机制在 `seed/src/vm/engine.c:120-130`：
    字面量走 `lainvm_operand_read` → `read_ref(tcb, ref, op->bits, inst->has_ty ? inst->ty : NULL)`，于是
    count 被按**指令的类型实参**（这里是元素类型 `#bits<8>`）读：256 & 0xFF = 0，再被 `op_alloca`
    （`engine.c:524`）的 `count ? count : 1` 夹成 1。元素类型 `#bits<64>` 时 count 正常——S3 的 1007
    用例就是这么测的。
  - 影响：`#alloca[#bits<8>](N)` 只能表达 N < 256；写大了**不报错、分配变小** = 静默错值。
  - 甲：count 按**地址宽度**读（与元素类型无关），验证器要求 count 是地址宽度的整数 →
    `#alloca[#bits<8>](256)` 就是 256 字节。乙：保持"count 是元素类型宽度的整数"，但验证器**拒绝
    装不下的字面量**（像码 23 那样），且运行期不再把 0 夹成 1（0 直接拒，码 **2024** 已存在）。
  - 这条要动 `seed/src/vm/engine.c`（VM 暂停区），所以我停在这里给复现，没有自行改。

## 4 风险与已知坑

1. **枚举加值是静默错值的高危点**：六处 dispatch 与名字表必须同时对齐。
2. **文本往返**必须成为用例，不能只验「能解析」：今天的缺陷正是打印出的写法解析器不认。
3. **折叠必须在主线**，否则一旦 Meta 能产出 eval，后端直接 9225 —— 缺阶段不是缺优化。
4. **块内的 `#alloca` 归哪个 activation**：已由本轮契约定（属该过程 activation，水位单调、受租约容量约束），实现时不要为块另开一套。
5. **不要为块新增 VM 接口**：VM 侧本轮暂停；如实施中发现确实缺原语，停下来给最小复现 + 两个方案。
6. **`#alloca` 的 count 宽度陷阱**（S3 实测，见 D7）：`#alloca[#bits<8>](256)` 静默变成 1 字节。
   写验收用例时元素类型别选比 count 窄的（`#bits<64>` 安全），但**不要**把它当正常语义。

## 5 验收入口与基线

每次改动至少跑：

```text
python scripts/build.py                 # BUILD OK（严格告警）
python scripts/check_meta.py            # 基线 19/19
python scripts/check_vspace.py          # 基线 94 条 94/0/0
python scripts/check_meta.py snapshot build/before.json
# ... 改动 ...
python scripts/check_meta.py snapshot build/after.json
python scripts/check_meta.py compare build/before.json build/after.json   # 基线 64/64
python scripts/check_docs.py            # 改动文档时
```

数值用例必须再做一次**错期望对照**（同一条改成 99 应退出 1）；拒绝用例断言具体诊断码。

IR 层的用例（S1/S2/S3）今天没有专用入口：建议照 VSpace 的先例加
`seed/tests/eval_checks.c` + `scripts/check_eval.py`（每用例独立进程、超时、崩溃与解析失败都算失败、
负对照单列），并把它接进 `scripts/build.py`。源语言层的用例（S4/S5）进 `scripts/check_meta.py` 的用例表。

**当前基线**：`check_vspace` 94/94、`check_meta` 19/19、语料快照 64/64、`check_docs` 0 errors。

## 6 不在本计划内

- `seed/src/vm/**` 的任何改动（暂停）。D7 的修法要动 `engine.c`，等裁定。
- `docs/spec/lainir.md` §7 与归档设计之间「块形式是否被放弃」的那句说明（等 D1 定了再写，避免文档先于裁定）。
- `docs/implementation/README.md` 的索引更新（该文件当前有他人未提交的改动，本计划不覆盖）。
