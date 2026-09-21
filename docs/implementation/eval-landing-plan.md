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
| 块可读外围自由 `%local`；lowering 把自由变量作为临时根过程**按值**传入；块不持有调用者 frame（:158-159） | 折叠阶段把块 lowering 成临时根过程：自由变量 → 该过程的 `params`，进入时按值填初值 | 块引用外围局部变量并算出正确值；块结束后调用者 frame 不受影响 |
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

### S4 Meta 产出

- **改**：`bootstrap/` 的 v0 源语言接受 `#eval`（拼写见 D6），Meta lowering 生成块并置标记。
- **验收**：源文件 → LAINIR 文本里出现 `#eval`；实参非常量的拒例（2012）。
- **风险**：Meta 是手写 LAINIR，暂存区布局以 `bootstrap/std/scope.l1` 文件头为准；新增格子要同步容量检查。

### S5 主线接线

- **改**：驱动在 lowering 之后调用折叠，再产出后端输入；折叠后**重新验证**一遍（折叠产物仍须通过验证器）。
- **验收**：端到端正例（源 → 折叠 → 后端/执行结果正确）+ 反例（未折叠残留被拒）。
- **风险**：折叠引入的新阶段要有明确入口，不能在驱动里散着调。

### S6 预算与策略

- 每模块 eval 块数量：加计数与上限（0 = 不限，按设计原话）。
- quota 归属与 eval TCB 的空间来源按 D4/D5 的裁定接线。

## 3 待作者裁定

每条给两个方案 + 我的建议，**不自行选择**。

**已裁定（作者）**：D1 = **甲**（块是 IR 指令，验证器按匿名过程定型）、D2 = **甲**（块自己声明
结果类型）、D3 = **甲**（块的结果类型不得含 `#addr`）—— 都按甲实施。D4/D5/D6 分别到 S3/S6 才用得上。

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
- **D6 v0 源语言的拼写。** 甲：`#eval f(1)` 作为表达式。乙：单独的声明形式（如 `eval NAME = f(1);`）。
  **我建议甲**（与 LAINIR 侧同名，Meta 只需认前缀，不新增声明语法）。

## 4 风险与已知坑

1. **枚举加值是静默错值的高危点**：六处 dispatch 与名字表必须同时对齐。
2. **文本往返**必须成为用例，不能只验「能解析」：今天的缺陷正是打印出的写法解析器不认。
3. **折叠必须在主线**，否则一旦 Meta 能产出 eval，后端直接 9225 —— 缺阶段不是缺优化。
4. **块内的 `#alloca` 归哪个 activation**：已由本轮契约定（属该过程 activation，水位单调、受租约容量约束），实现时不要为块另开一套。
5. **不要为块新增 VM 接口**：VM 侧本轮暂停；如实施中发现确实缺原语，停下来给最小复现 + 两个方案。

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

- `seed/src/vm/**` 的任何改动（暂停）。
- `docs/spec/lainir.md` §7 与归档设计之间「块形式是否被放弃」的那句说明（等 D1 定了再写，避免文档先于裁定）。
- `docs/implementation/README.md` 的索引更新（该文件当前有他人未提交的改动，本计划不覆盖）。
