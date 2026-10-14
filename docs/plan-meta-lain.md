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

**翻译编译边界**：`std/lex.lain` 依赖后续 `std/types.lain` 的 `meta_bytes_equal`。本片用合并 Lain 源
与依赖实现编译验证；今后遇到跨片调用，按最小依赖闭包组合编译，不添加与实际定义冲突的 extern shim。

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
  `--round-trip` 通过，提交 `18a9fb1`（已推送）。
- `std/lex.lain` 已按 compile-only gate 翻译并通过：27 个顶层静态表字节匹配、17 个过程名匹配；与
  `meta_bytes_equal` 依赖实现合并编译，结果 45 个过程、27 个 data，退出码 0。未运行 Meta。
- 修复 `lain0` 值型 loop 的重复合成 break；重建成功，examples `--round-trip` **35/35**。
- 翻译提交 `f3b2c1b`、删除 `bootstrapl1/std/lex.l1` 提交 `5fbe5d5`，均已推送至 `feat/meta`。
- `std/emit.lain` 已翻译：33 个过程名和签名、29 个静态数据符号/字节与旧片匹配。它依赖 `std/lainir.l1`
  中的 `lainir_write`、`lainir_repr`、`lainir_emit_uint`；用临时同签名 Lain 定义组成 compile-only 输入后，
  `lain0` 验证通过（65 个过程、29 个 data，退出码 0）。未运行 Meta；旧 `.l1` 将在翻译提交后单独删除。
- `std/types.lain` 已翻译：7 个实现过程名和签名与旧片匹配，没有静态数据；含 3 个 extern 依赖声明时直接编译，
  `lain0` 退出码 0、verify 通过。未运行 Meta；旧 `.l1` 后续单独删除。
- `std/regions.lain` 已翻译：3 个 Lain 实现过程、1 个 host extern；24 字节 scratch record 与旧数据一致。首次编译
  暴露 `symbol_addr` 需引用生成名 `_storage`，现已修正；与临时 `meta_cell_get` 定义合并编译后 verify 通过
  （6 个过程、1 个 data，退出码 0）。旧 `.l1` 保留至翻译提交后另行删除。
- `std/registry.lain` 已翻译：10 个过程名及签名、3 个静态数据符号及字节均与旧片一致；7 个跨片 helper 以原始
  名称声明为 extern，过程体也使用完整 helper 名称。`lain0 bootstrap/std/registry.lain` 退出码 0、verify 通过；
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/scope.lain` 已翻译：36 个过程名与签名、`meta_kw_colon_b` 数据字节（58）均与旧片一致；41 个跨片/宿主依赖
  以 extern 声明。与最小数据依赖拼合后 `lain0` 退出码 0、verify 通过（83 个过程、6 个 data）。本轮新增三条表面
  语法约束记录在《已知风险》。翻译提交为 `4d84724`，旧 `.l1` 已在 `5eec199` 单独删除。
- `std/parse.lain` 已翻译：35 个实现过程名和签名与归档旧片一致，无静态数据；依赖已在单文件输入中声明，无数据 stub。
  `./build/lain0/lain0.exe build/lain0/parse-with-deps.lain` 退出码 0、verify 通过（46 个过程）。未运行 Meta。
  旧 `.l1` 已在 `483aec4` 删除。

- `std/macros/my_if.lain` 已翻译：6 个实现过程名与签名、2 个静态数据字节与旧片一致；18 个跨片/宿主依赖以 extern 声明。
  与临时 `meta_kw_else` 数据定义拼合后 `lain0` 退出码 0、verify 通过（27 个过程、3 个 data）。未运行 Meta。
  旧 `.l1` 将在翻译提交后单独删除。
- `std/macros/my_block.lain` 已翻译：4 个实现过程名与签名、`meta_kw_my_block` 数据字节（109 121 95 98 108 111 99 107）
  与旧片一致；23 个跨片/宿主依赖以 extern 声明，`meta_expand_splice_group` 在文件内定义。`lain0` 退出码 0、
  verify 通过（28 个过程、1 个 data），无需数据 stub。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/macros/my_require.lain` 已翻译：2 个实现过程名与签名、`meta_kw_my_require` 数据字节
  （109 121 95 114 101 113 117 105 114 101）与旧片一致；13 个跨片/宿主依赖以 extern 声明。`lain0` 退出码 0、
  verify 通过（16 个过程、1 个 data），无需数据 stub。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- 下一片为 `SOURCE_ORDER` 第 11 项 `std/syntax.l1`。

## 已知风险

- **规模**：18,236 行、29 个源文件，是长跑；每片一提交保持可审阅。
- **Source loading/linking**：编译逐片进行；全量链接和 `meta_driver` 如何消费新模块尚未核实，执行中检查并按需补支持。
- **布局一致性**：Meta 操作 LAINIR 数据结构；逐片能编译不代表布局语义一致，运行时正确性不在本轮验收范围。
- **`symbol_addr` 只用生成名**：顶层 `let NAME: [T] = [...]` / `let NAME: addr = "…"` 在产物里落成 `NAME_storage`，
  所以跨片引用静态数据必须写 `symbol_addr("NAME_storage")`；写 `symbol_addr("NAME")` 会得到 code 2002
  undefined value。与旧 IR 的 `#data_addr NAME` 是同一条数据的两个拼写。
- **bool 值型 `if` 的假分支不能写裸 `0`**：`let v: bool = if c { … } else { 0 };` 在 verify 阶段报 2005
  （`#yield value 0 does not match the declared result`）；循环里的 `break label(0)` 没有这个问题。等价的
  写法是把它改成守卫式早退（`if c == 0 { … return …; }`），`scope.lain` 的 `meta_scope_decl_node` 即此形。
- **`ref` 是保留字**：参数名不能叫 `ref`（`extern ast_child(ref: u64, …)` 报「应为 参数名」），改名即可。

## 参考

- `docs/plan-lain0.md`（stage 1 / `lain0` 功能进度）
- `docs/04-bootstrap-dialect.md`（旧 bootstrap 方言说明）
- `bootstrapl1/SOURCE_ORDER`（旧文件拼装顺序）
- `bootstrapl1/`（手写 LAINIR 档案）
