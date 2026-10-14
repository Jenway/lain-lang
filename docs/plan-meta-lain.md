# 计划：用 Lain 重写 Meta，逐步替代手写 LAINIR bootstrap（stage 2）

> 本文件是 **可执行的任务序列**。stage 1（`lain0` 编译器 + 31 样例全绿）已完成，
> 见 `docs/plan-lain0.md`。本计划是 stage 2：**把 `bootstrap/` 里手写的 LAINIR Meta
> 用 Lain 重新写一遍，由 `lain0` 编译成 LAINIR，最终在 LainVM 里跑起来**，逐步替代
> 冻结的手写 IR。每翻完一片就删掉对应的旧 `.l1`，全程以 `bootstrapl1/` 当档案、git 留历史。

## 北极星

- **终点**：`bootstrap/` 里是 Lain 写的 Meta（`.lain`），`lain0` 能把它编译成合法
  LAINIR；手写 LAINIR（`bootstrapl1/`）被逐步删光；`lain0` 不再依赖手写 IR 自举。
- **本阶段不要求运行时正确**（用户明确：翻译时「不测试，只保证能编译」）；运行时验证
  留给整份 Meta 在 LainVM 跑通时做。
- **约束**：`bootstrap` 曾冻结（D-001）。本次重命名 + 替代 = 主动结束冻结，需在档案里注记。

## 现状盘点（已核清，作为本计划的依据）

- Meta 由 `bootstrap/meta.l1` + `bootstrap/std/*.l1`（含 `std/macros/`、`std/handlers/`
  子目录）组成，共 **14,825 行**手写 LAINIR。
- `bootstrap/SOURCE_ORDER` 列出 28 个 `.l1` 文件的拼装顺序——**天然就是「一个一个」的清单**。
- 已确认 Meta 里**没有** `#call[tail]`（0 次）、**没有** 多结果过程（0 次）——两个已知阻塞点都不触发。
- `bootstrap/lain/examples/`（31 个 Lain 文件）是 `lain0` 的编译回归测试，**不属于 Meta**。

## 目录处置（关键：别弄丢 examples 闸门）

1. `git mv bootstrap bootstrapl1`（整目录改名，Windows 上 `bootstrap` 与 `bootstrapl1`
   末字符不同，不会撞名）。`bootstrapl1` 成为手写 LAINIR 的永久档案。
2. 新建 `bootstrap/`，并在其中**保留 `lain/examples/`**（从 `bootstrapl1/lain/examples`
   拷过来）——否则 `scripts/regress.sh`（glob `bootstrap/lain/examples/*.lain`）会空转，
   34/34 闸门消失。
3. 把 `bootstrap/FROZEN.md`、`bootstrap/lainir_f1_unit.txt` 移入 `bootstrapl1/`，并在
   `bootstrapl1/FROZEN.md` 末尾追加：*「stage 2 起，手写 LAINIR 由 Lain 版逐步替代；
   D-001 冻结在此结束。」*
4. `regress.sh` 无需改路径（新 `bootstrap/lain/examples/` 仍存在）。

> **决策点 A（默认：新 bootstrap 内保留 lain/examples）**：若你更想让 regress 指向
> `bootstrapl1/lain/examples` 也可，但默认选「保留」，改动最小。

## 步骤

### 步骤 0 · 准备（一次性）
- `git mv bootstrap bootstrapl1`。
- 新建 `bootstrap/lain/examples/`，拷 31 个样例。
- `bootstrapl1/FROZEN.md` 追加结束冻结注记。
- 确认 `bash scripts/regress.sh --round-trip` 仍以 34/34 通过（examples 没丢）。

### 步骤 1 · Lain 表达力审计（只读 grep，不动手）★ 建议先做
把 Meta 用到的 LAINIR 指令、`extern` 原语名、`struct`/`enum` 布局形态全列出来，
对照 `lain0` 现在能发出来的形态，确认没有表达不了的点。重点核：
- `extern` 原语（VM 提供的运行时入口：allocate / read / print / …）——Lain 需对应
  `extern` 声明，且名字要与 VM 一致。
- `#store` / `#lea` / `#load` / `#alloca` / 数据块 `#data` 的形态，Meta 用到哪些，
  Lain 的 `load`/`store`/`lea`/`addr_of`/表/串字面量能否等价发出。
- 聚合（struct/enum）的内存布局：Meta 手写的 offset 是否与 Lain 降级出的布局一致
  （不一致会导致运行时读错字段——这是「只编译不测试」最易漏的一类）。
- 若发现 Lain 表达不了的构造，先回 `lain0` 补能力，再继续。

> **决策点 C（默认：先做审计）**：强烈建议先做；若你判断 `lain0` 已够直接开工，可跳过。

### 步骤 2 · 逐片翻译（主体）
按 `SOURCE_ORDER` 的 28 个文件，**自底向上**逐个把 `.l1` 翻译成 `.lain`：
- 文件镜像：`std/lex.l1` → `bootstrap/std/lex.lain`，`std/macros/my_if.l1` →
  `bootstrap/std/macros/my_if.lain`，…，`meta.l1` → `bootstrap/meta.lain`。
- 翻译即「把同一条 LAINIR 逻辑用 Lain 表面语法重写」：过程→`func`、聚合→`struct`/`enum`、
  控制流→`if`/`loop`/`for`、`#store`/`#lea`/`#load`→`load`/`store`/`lea` 内建、
  地址→`addr`/`ref`/`mut ref`、`#data` 表/串→顶层 `let`。
- 每片一个小提交，便于审阅。

> 翻译顺序（= SOURCE_ORDER，28 片）：
> ```
> std/lex.lain          std/emit.lain         std/types.lain
> std/regions.lain      std/registry.lain     std/scope.lain
> std/parse.lain        std/macros/my_if.lain std/macros/my_block.lain
> std/macros/my_require.lain  std/syntax.lain
> std/handlers/struct.lain  std/handlers/enum.lain  std/handlers/func.lain
> std/handlers/if.lain    std/handlers/let.lain   std/handlers/statements.lain
> std/declarations.lain   std/expand.lain       std/recognize.lain
> std/modules.lain        std/eval.lain         std/scalars.lain
> std/records.lain        std/funcs.lain        std/sums.lain
> std/lainir.lain         meta.lain
> ```

### 步骤 3 · 每片闸门（默认两道）
- **硬闸门**：该 `.lain` 过 `lain0` 编译（`verify` 无 `error:`，本就是 `lain0` 的判据）。
- **推荐软闸门**：把 Lain 版过 `lain0` 生成的 LAINIR 与原 `.l1` 做**对照 diff**
  （脚本或人工）。两者不会逐字节相同（Lain→LAINIR 的形态差异），但过程名、指令、类型应一致；
  能挡掉绝大多数翻译错。**不依赖 VM、不跑执行**。

> **决策点 B（默认：编译过 + LAINIR 对照）**：若你坚持纯「编译过」，去掉软闸门即可。

### 步骤 4 · 提交与删旧
- 每片：提交新的 `.lain`（分支见决策点 E）。
- 该片过闸门后，**删 `bootstrapl1/` 里对应的那个 `.l1`**（git 历史仍保留，可回退）。
- `bootstrapl1/` 整体留到**整份 Meta 在 LainVM 跑通**后再一次性删掉（决策点 D）。

> **决策点 D（默认：整份跑通后删）**：边翻边删也可，但保留档案到整跑能降低最后联调风险。

### 步骤 5 · 收尾联调
- 全部 28 片翻译完 + 提交完，用 `SOURCE_ORDER`（改为 `.lain`）把 Lain Meta 拼成一份、
  过 `lain0` 编译成整份 LAINIR。
- 在 LainVM 里加载并**冒烟运行**，与 `bootstrapl1` 的已知正确行为对照（差异测试）。
- 跑通后：删 `bootstrapl1/` 目录，单发一个清理提交；更新本计划文档标注完成。

## 验收闸门

- **每片**：`lain0` 编译该 `.lain` 无 `error:`（硬闸门）；推荐过 LAINIR 对照（软闸门）。
- **全程**：`bash scripts/regress.sh --round-trip` 保持 34/34（examples 不被破坏）。
- **终点**：翻译后的 Lain Meta 整份编译成 LAINIR 并在 LainVM 跑通，行为等价于 `bootstrapl1`。

## 决策点汇总（默认已标，等你拍板）

| 决策 | 默认 | 备选 |
|---|---|---|
| A. examples 安置 | 新 `bootstrap/lain/examples` 保留 | regress 指向 `bootstrapl1/lain/examples` |
| B. 每片闸门 | 编译过 + LAINIR 对照 | 仅编译过 |
| C. 先做表达力审计 | 是（只读） | 跳过直接开工 |
| D. 删旧 `.l1` 时机 | 整份跑通后删 | 边翻边删 |
| E. 分支 | 新建 `feat/meta`（从 `feat/lain0` HEAD 起） | 继续在 `feat/lain0` |

## 已知风险

- **规模**：14,825 行、28 片，是长跑；每片一提交保持可审阅。
- **布局一致性**：Meta 是操作 LAINIR 数据结构的运行时，Lain 降级出的 struct/enum 布局必须
  与手写 IR 一致，否则运行时读错字段——「只编译不测试」最易漏，故步骤 1 审计 + 步骤 3 对照最关键。
- **运行时验证缺失**：翻译期只保编译；真正的语义正确要等步骤 5 在 LainVM 联调，可能一次性暴露多处，
  届时对照 `bootstrapl1` 与 git 历史逐片回查。

## 参考

- `docs/plan-lain0.md`（stage 1 完成）
- `docs/04-bootstrap-dialect.md`（Lain 各构造的降级形态 spec）
- `bootstrap/SOURCE_ORDER`（翻译顺序，本计划的权威片清单）
- `bootstrapl1/`（手写 LAINIR 档案，步骤 5 前不得删）
