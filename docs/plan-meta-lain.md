# 计划：用 Lain 重写 Meta，逐步替代手写 LAINIR bootstrap（stage 2）

> 本文件是 **可执行的任务序列**。stage 1（`lain0` 编译器 + 31 样例全绿）已完成，
> 见 `docs/plan-lain0.md`。本计划是 stage 2：**把 `bootstrap/` 里手写的 LAINIR Meta
> 用 Lain 重新写一遍，由 `lain0` 编译成 LAINIR，最终在 LainVM 里跑起来**，逐步替代
> 冻结的手写 IR。每翻完一片就删掉对应的旧 `.l1`，全程以 `bootstrapl1/` 当档案、git 留历史。

## 北极星

- **终点**：`bootstrap/` 里是 Lain 写的 Meta（`.lain`），`lain0` 能把它编译成合法
  LAINIR；手写 LAINIR（`bootstrapl1/`）被逐步删光；`lain0` 不再依赖手写 IR 自举。
- **本阶段不要求运行时正确**（用户明确：翻译时「不测试，只保证能编译」）；逐片只要求 `lain0`
  能编译。运行时验证不作为翻译闸门，也不在本计划本轮执行。
- **约束**：`bootstrap` 曾冻结（D-001）。本次重命名 + 替代 = 主动结束冻结，需在档案里注记。

## 现状盘点（已核清，作为本计划的依据）

- Meta 由 `bootstrapl1/meta.l1` + `bootstrapl1/std/*.l1`（含子目录）组成，按 `SOURCE_ORDER`
  有 **28 个清单文件 + meta.l1（共 29 个）**，合计 **18,236 行**。
- `bootstrapl1/SOURCE_ORDER` 列出 28 个 `.l1` 文件的拼装顺序；`meta.l1` 由驱动额外加载。
- 已确认 Meta 里**没有** `#call[tail]`、**没有**多结果过程。
- `bootstrap/lain/examples/`（35 个 Lain 文件）是 `lain0` 的编译回归测试，**不属于 Meta**。

### Step 0 完成（`feat/meta`）

- 原 `bootstrap/` 已归档到 `bootstrapl1/`；新 `bootstrap/lain/examples/` 保留 34 个样例。
- `bootstrapl1/FROZEN.md` 已注明 D-001 冻结结束、stage 2 改由 Lain 逐步替代。
- `bash scripts/regress.sh` 实测 **34/34**。
- 提交并推送：`d63b6c6`（分支 `feat/meta`）。

### 表达力审计：发现前置缺口，正在补齐

Meta 由 `SOURCE_ORDER` 中 **28 个文件 + `meta.l1`（共 29 个）**构成，精确统计 **18,236 行**。
对这些文件扫描真实代码行（忽略整行 `//` 注释）后，确认：**524 个有效 `#data_addr`**、
**10 个 `#ptr2int`**、**1 个 `#int2ptr`**；`#ptr` 命中是注释误报。共有 **33 个**宿主 `#extern`
链接符号。

`#data_addr` 经常作为内嵌操作数出现。已给 `lain0` 增加 `symbol_addr("symbol")`：字符串参数在编译期作为模块符号名，
生成 `#bitcast[#addr](%symbol)`；模块级 `#data` 符号由 verifier 绑定成 `#addr`，所以该形式对应旧 `#data_addr symbol`。
样例覆盖直接返回及作为 call 实参，已编译并通过 verify。

旧地址/整数转换共 11 处；`addr as u64` / `u64 as addr` 已实编译为相应 `#bitcast`。`docs/04-bootstrap-dialect.md`
记有对 11 处的 surface probe。33 个宿主 extern 有 Lain `extern` 语法，链接名在源文件中指定。

**数据地址与 cast 前置能力已补齐**：`symbol_addr("symbol")` 可生成 `#bitcast[#addr](%symbol)`，样例编译过 verify；
`addr as u64` / `u64 as addr` 发出 `#bitcast`，覆盖旧 11 处转换的表面形态。

**仍需决定翻译片段的编译边界**：`std/lex.l1` 调用 `meta_bytes_equal`，其实现位于 `std/types.l1`；
SOURCE_ORDER 顺序是 lex、emit、types。单独编译 lex 会缺 callee；加入同名 extern shim 再带上真实定义
会因 duplicate subroutine 被 verifier 拒绝。方案是把相依片段合并后用 `lain0` 编译，或给编译器加多源模块
输入能力。两者都没有实施；尚未翻译、也没有删除任何旧 `.l1`。

`meta_driver` 的旧入口把 SOURCE_ORDER 中的 LAINIR 文本拼为一个模块再 parse/verify。当前不需要为了
「逐片能编译」而先改它；等 Lain 源片和生成的 LAINIR 模块装配边界确定后，再做对应整合。
## 目录处置（关键：别弄丢 examples 闸门）

1. `git mv bootstrap bootstrapl1`（整目录改名，Windows 上 `bootstrap` 与 `bootstrapl1`
   末字符不同，不会撞名）。`bootstrapl1` 成为手写 LAINIR 的永久档案。
2. 新建 `bootstrap/`，并在其中**保留 `lain/examples/`**（从 `bootstrapl1/lain/examples`
   拷过来）——否则 `scripts/regress.sh`（glob `bootstrap/lain/examples/*.lain`）会空转，
   34/34 闸门消失。
- `bootstrapl1/FROZEN.md`、`bootstrapl1/lainir_f1_unit.txt` 位于归档目录；新 `bootstrap/` 只保留
  `lain/examples/` 回归用例并逐步加入 Lain Meta 源文件。
4. `regress.sh` 无需改路径（新 `bootstrap/lain/examples/` 仍存在）。

> **决策点 A（默认：新 bootstrap 内保留 lain/examples）**：若你更想让 regress 指向
> `bootstrapl1/lain/examples` 也可，但默认选「保留」，改动最小。

## 步骤

### 步骤 0 · 准备（一次性）
- `git mv bootstrap bootstrapl1`。
- 新建 `bootstrap/lain/examples/`，拷 **34** 个样例。
- `bootstrapl1/FROZEN.md` 追加结束冻结注记。
- 确认 `bash scripts/regress.sh` 仍以 **34/34** 通过（examples 没丢）。

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
按 `bootstrapl1/SOURCE_ORDER` 的顺序把 28 个清单文件逐个翻译；`meta.l1` 由驱动额外加载，是第 29 片。
第一个翻译对象是 `std/lex.l1`。
- 文件镜像：`std/lex.l1` → `bootstrap/std/lex.lain`，宏与 handler 保持子目录，最后
  `bootstrapl1/meta.l1` → `bootstrap/meta.lain`。
- 每片逐过程把同一 LAINIR 逻辑写成 Lain：过程→`func`、聚合→`struct`/`enum`、控制流→
  `if`/`loop`/`for`、load/store/lea→对应内建、符号地址→`symbol_addr("name")`、静态数据→
  顶层绑定。不要改变过程名、参数/结果和绑定到的宿主 extern 链接名。
- 每片达到「lain0 编译成功」后，才删 `bootstrapl1/` 对应 `.l1`；每片一个提交。

### 步骤 3 · 每片闸门（只编译）
- 对每片用 `lain0` 编译，要求编译器实际退出成功且诊断中没有 `error:`。
- **不运行 Meta、不做运行时测试、不要求 LAINIR diff**；Meta 的行为验证不在本轮闸门内。
- 编译失败时修到通过，或记录具体 blocker 后暂停后续翻译；不能删未通过的旧 `.l1`。

### 步骤 4 · 提交与删旧
- 每片：提交新的 `.lain`，然后在紧随的提交中删除 `bootstrapl1/` 对应的旧 `.l1`。
- 每片应完整满足编译闸门后再删除原件；git 历史保留旧内容。
- 每片翻译成功并编译后立即删掉对应的旧 `.l1`；只有元文件和测试夹具保留到你要求清理。

### 步骤 5 · 本轮停止点
29 片 Lain 文件全部按序完成且能编译后，更新本计划标记「Lain Meta 全量编译完成」。本轮按你的要求
**不启动 LainVM、不做运行时验证**。完整 Meta 的装载/VM 入口另列后续计划，不把运行成功作为本轮完成条件。

### 删除规则修订

用户要求每翻完一片就删对应旧 IR。每片的新 `.lain` 编译成功并提交后，删掉 `bootstrapl1/` 中
对应的 `.l1`（单独后续提交）。
- `bootstrapl1/` 的非翻译元文件和 tests 位置到最终清理由用户另行指示。

## 验收闸门

- 每片 `.lain` 用 `lain0` 编译成功；不执行 Meta、不做运行时验证。
- **编译器源码改动**（如 `symbol_addr`）走 `lain0` 项目的 examples + round-trip 回归；这不是 Meta 运行时测试。
- 本轮终点：29 个 Meta 源文件逐片翻译且均能编译；不要求 Meta 在 LainVM 跑通。

## 执行状态

- 目录重命名及 examples 保留：完成，提交 `d63b6c6`。
- `symbol_addr` 与字符串 token 去引号修复：实现完成，示例编译通过；34 个既有样例 + 新样例共 **35/35**
  `--round-trip` 通过。
- 尚未翻译任何 Meta 文件。
- 当前分支：`feat/meta`（从 `feat/lain0` 起）。

## 已知风险

- **规模**：18,236 行、29 个源文件，是长跑；每片一提交保持可审阅。
- **Source loading/linking**：编译逐片进行；全量链接和 `meta_driver` 如何消费新模块尚未核实，执行中检查并按需补支持。
- **布局一致性**：Meta 操作 LAINIR 数据结构；逐片能编译不代表布局语义一致，运行时正确性不在本轮验收范围。

## 参考

- `docs/plan-lain0.md`（stage 1 / `lain0` 功能进度）
- `docs/04-bootstrap-dialect.md`（旧 bootstrap 方言说明）
- `bootstrapl1/SOURCE_ORDER`（旧文件拼装顺序）
- `bootstrapl1/`（手写 LAINIR 档案）
