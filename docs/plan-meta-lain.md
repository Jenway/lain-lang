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
- `std/syntax.lain` 已翻译：12 个实现过程名与签名、`meta_syntax_state`（rw，24 个 0 字节）与旧片一致；29 个跨片/宿主依赖
  以 extern 声明，三个 `meta_register_my_*` 在此为 extern。`lain0` 退出码 0、verify 通过（42 个过程、1 个 data），
  无需数据 stub。旧 `#lshr`/`#ptr2int`/`#int2ptr` 分别落成 `>>`、`as u64`、`as addr`。未运行 Meta。
  旧 `.l1` 将在翻译提交后单独删除。
- `std/handlers/struct.lain` 已翻译：3 个实现过程名与签名与旧片一致，无静态数据；5 个跨片/宿主依赖以 extern 声明
  （`meta_scope_add_decl` 在 scope.lain 定义，`meta_kw_struct` 在 emit.lain 定义）。与临时 `meta_kw_struct` 数据定义
  拼合后 `lain0` 退出码 0、verify 通过（9 个过程、1 个 data）。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/handlers/enum.lain` 已翻译：3 个实现过程名与签名与旧片一致，无静态数据；5 个跨片/宿主依赖以 extern 声明
  （`meta_kw_enum` 在 emit.lain 定义）。与临时 `meta_kw_enum` 数据定义拼合后 `lain0` 退出码 0、verify 通过。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/handlers/func.lain` 已翻译：8 个实现过程名与签名与旧片一致，无静态数据；17 个跨片/宿主依赖以 extern 声明。
  compile fixture 中临时定义 `meta_kw_func`、`meta_kw_extern`、`meta_expand_decl_phase` 三个数据符号；
  `./build/lain0/lain0.exe build/lain0/func-with-deps.lain` 退出码 0、verify 通过（28 个过程、3 个 data）。
  `meta_expand_children_func` 的变更标记为 bit 63，与低位常数 6 不重叠，故 `flag + 6` 等价于旧 IR 的 bitwise OR。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/handlers/if.lain` 已翻译：3 个实现过程名与签名与旧片一致，无静态数据；7 个跨片依赖以 extern 声明
  （`meta_kw_if`/`meta_kw_else` 均单点定义在 lex.lain）。与临时两关键字数据定义拼合后 `lain0` 退出码 0、verify 通过。
  旧 IR 的 `%else_hit` 值型 if（has_after 为 0 时不再求值）改写为等价守卫嵌套；高位变更标记同样用加法代替 OR。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/handlers/let.lain` 已翻译：8 个实现过程名与签名与旧片一致，无静态数据；30 个跨片依赖以 extern 声明。
  与临时 `meta_kw_let`/`meta_kw_ro` 数据定义拼合后 `lain0` 退出码 0、verify 通过。旧 IR 的四处「支里套值 if」
  （`%nxt`/`%v`/`%acc2`/`%one`）改写成守卫 + `continue`/`break`，逐条对照语义等价（值 if 结果直接喂给同一个
  continue/break，提升的表达式均为纯计算）；`meta_literal_fits` 的 `ref` 参数因保留字改名 `ref_v`，链接名不变。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/handlers/statements.lain` 已翻译：3 个实现过程名与签名与旧片一致，无静态数据；3 个跨片依赖以 extern 声明；
  `meta_syntax_ensure` 的 context/handler 号 (2,8)/(2,10)/(2,13) 与旧片一致。与临时 `meta_kw_return`/`meta_kw_for`/
  `meta_kw_loop` 数据定义拼合后 `lain0` 退出码 0、verify 通过。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- handler 目录至此全部迁完（struct、enum、func、if、let、statements）。
- `std/declarations.lain` 已翻译：6 个实现过程名与签名与旧片一致，无静态数据；17 个跨片依赖以 extern 声明。
  无需数据 stub，`lain0` 退出码 0、verify 通过。五个分派链（recognize/replacement/stmt_children/decl_children/
  declare/lower）的 handler 常量与首个命中顺序逐条对照一致，兜底 `lain_meta_fail(9412)` 与 9412 返回值保持。
  嵌套值 if 分派链改写成平坦守卫（命中即 return，既有写法）。`%slice` 因保留字改名 `slice_v`。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/expand.lain` 已翻译：14 个实现过程名与签名与旧片一致，38 个跨片依赖以 extern 声明，本文件内定义 4 个数据块
  （`meta_expand_defer_state`/`meta_expand_decl_phase`/`meta_expand_decl_state`/`meta_expand_id_state`，各 8 个 0 字节，rw）。
  无需数据 stub，`lain0` 退出码 0、verify 通过。三处嵌套值 if（`%answer`、`%new_root`、`%st`）改写为一次性选择循环，
  逐条核对：`pick` 在 `changed_bit == 0` 时不调用 `append_refs`（无副作用）且 `bump_release` 仍无条件执行；
  `pick_root` 仅在 `decl_changed == 0` 时清 `decl_phase`；`%st` 的遮蔽重读改名 `decl_changed2`。
  全文件调用序列对比：7 处差异全部是新增的 `symbol_addr` 与循环标签，真实调用顺序、标志常量（3×`0x8000…`、2×`0x7FFF…`）
  与失败码集合 {17,9408,9410,9411,9412} 一致。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/recognize.lain` 已翻译：15 个实现过程名与签名与旧片一致，25 个跨片依赖以 extern 声明，无静态数据，无需 stub，
  `lain0` 退出码 0、verify 通过。`meta_recognize_word` 的 12 层嵌套值 if 改写为一次性选择循环 `pick_form`，
  优先级链逐条核对与旧片相同（3/8/11/9/5/13/12/10/6/4/7/1），且所有条件均提前算好、无副作用；
  内层遮蔽的 `%stmt_context` 改名 `stmt_ctx` 并提前求值；其余按平坦守卫改写（命中即 return/break）。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/modules.lain` 已翻译：18 个实现过程名与签名与旧片一致，39 个跨片依赖以 extern 声明，本文件内定义 2 个数据块
  （`meta_kw_import` ro 6 字节、`meta_suffix_lain` ro 5 字节，字节与模式均与旧片一致）。6 个跨片数据符号在 fixture 中
  按定义单元原字节 stub（`lainir_text_buffer`/`lainir_zero`/`meta_dsep`/`meta_path_cap`/`meta_reg_count_off`/`meta_scalar_table`）；
  `lain0` 退出码 0、verify 通过。8 处调用序列差异经核对全部为 `load`/`store` 内建与守卫改写导致的字典序重排，
  运行顺序不变；重点复核 `%step`（`%also` 仅在 `is_colon` 路径求值）、`pick_end`（`has` 为真时不写后缀）、
  `pick_ready`（只有没扫过才调 `meta_scope_count`/`meta_scope_scan`）、`pick_body`、`%j3` 值 if（支内含 store，属允许形态）。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/eval.lain` 已翻译：3 个过程名与签名与旧片一致（其中 `lain_meta_apply_emitted_request` 在旧片中是无体的
  `#proc … #extern "…"` 宿主声明，落成一条 `extern`，链接名原样）；本文件内定义 2 个 ro 数据块
  （`meta_eval_align_module` 223 字节、`meta_eval_align_entry` 12 字节），无需跨单元数据 stub，`lain0` 退出码 0、verify 通过。
  `%result` 值 if（假支内含值 if 与语句 if）改写为一次性选择循环 `pick_result`：`valid` 为真只取 `%bits`，
  为假时按 `%code → %source → %known → 诊断` 原顺序执行后 yield `0xFFFFFFFFFFFFFFFF`，`meta_bump_release(mark)`
  仍在循环之后无条件执行；外层 `%release` 因遮蔽改名 `release_final`。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/scalars.lain` 已翻译：14 个实现过程名与签名与旧片一致，21 个跨片依赖以 extern 声明；本文件内定义 `meta_scalar_table`
  （ro，587 字节，sha256 `ae736489…`，与旧片逐字节相同）。fixture 只需 stub `meta_sym_i32`/`meta_sym_i64` 两个跨单元数据符号；
  `lain0` 退出码 0、verify 通过。原值 if 喂 `#return`/`if` 的位置全部改成平坦守卫，条件与原顺序不变，唯取中分支才执行
  （如 `meta_scalar_op` 仍只在 `sl == sym_len` 时调 `meta_bytes_equal`）；`meta_type_resolve` 经一次修正后限定名/非限定名两支
  顺序与旧片一致；`%rw`/`ref` 类保留字参数改名并保留链接名。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/records.lain` 已翻译：22 个实现过程名与签名与旧片一致，62 个跨片依赖以 extern 声明，无静态数据、过程顺序与旧片相同；
  fixture 需 stub 5 个来自 `lainir.l1` 的数据符号（`lainir_base_name`/`lainir_name_b`/`lainir_name_b2`/`lainir_name_v`/`lainir_text_buffer`）；
  `lain0` 退出码 0、verify 通过，本仓自有工具 `compare_fragment.py` 与 `compare_call_order.py` 均退出码 0。`meta_record_type_id`
  的 `%id` 值 if 之后还有无条件的 `restore_top`/`release`，若改平坦守卫会跳过清理，故用一次性选择循环 `pick_id` 保留原顺序；
  `%same`/u8 比较/循环参数遮蔽改名等改写逐条核对。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/funcs.lain` 已翻译（**最大一片**）：53 个实现过程名与签名与旧片一致，104 个跨片依赖以 extern 声明，无静态数据。
  因单片过大（3720 行），按 `scripts/split_unit.py` 切成 5 组并行翻译（各组 ~730 行 / 9–11 过程），共享头
  （104 条跨片 extern + 39 条数据 stub + 53 条同单元兄弟声明）由主代理写一次，再用 `scripts/merge_parts.py` 合并。
  各组均以 `lain0` 退出码 0 通过，合并后 fixture（clean unit + 39 个数据 stub）退出码 0，`compare_fragment.py` 与
  `compare_call_order.py` 由主代理亲自运行核对；5 处调用序列差异逐条审阅：三处失败码多重集完全相同（如 `meta_lower_body`
  为 {20:1,21:3,4:2}），另两处是互斥分支上的额外文本站点或与旧 IR 执行顺序一致的提升，均等价。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- 顺带修复 lain0 编译器缺陷：作为实参的嵌套调用拿不到结果临时量（`lowerCall` 用 `kInvalidType` 降级实参），
  `return inner(inner(y));` 报 verify 2020；改为传入实参在 Sema 里定下的类型。提交 `e151078`，
  `bash scripts/regress.sh --round-trip` PASS=35 FAIL=0。
- `std/sums.lain` 已翻译：18 个实现过程名与签名与旧片一致，84 个跨片依赖以 extern 声明，无静态数据；fixture 需 stub 6 个
  来自 `lainir.l1`/`emit.lain` 的数据符号（`lainir_eq_name`/`lainir_text_buffer`/`lainir_zero`/`meta_dsep`/`meta_eoff`/`meta_etag`）；
  `lain0` 退出码 0、verify 通过，`compare_fragment.py` OK。`compare_call_order.py` 报 7 处待审，逐条核对后全部为良性：
  四处是平坦守卫导致的 `lain_meta_fail` 文本位置前移（**失败码多重集完全相同**，如 ['10','9']、['5','9']），
  另三处是归档的 `#load`/`#store` 在表面语言里写作 `load`/`store` 内建（被脚本计为调用），其余 delta 为 `symbol_addr`/`lea`
  这类指令的表面写法差异。`meta_sum_type_id` 的 `%id` 后有无条件 `restore_top`/`release`，故用 `pick_id` 选择循环保留清理；
  `meta_lower_sum_construct_local` 的 `%t2` 用 `pick_t2`。未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `std/lainir.lain` 已翻译（**最后一片大件**）：152 个实现过程名与签名与旧片一致，9 个跨单元依赖以 extern 声明，
  本文件内定义 140 个静态数据块（138 ro + 2 rw，1086 字节）。切成 8 组并行翻译（各 18–20 个过程），共享头由主代理写，
  再用统一脚本按归档过程顺序合并 152 个过程体（提取时对注释做空白化再配平花括号，避免注释里的 `}` 截断函数）。
  fixture（clean unit + `meta_scalar_table` 数据 stub）退出码 0；`compare_call_order.py` 149/152 ok，3 处差异全是
  `#load`/`#store` 在表面语言里写作 `load`/`store` 内建。
- **四处数据块与同名过程冲突（必须解决的迁移差异）**：表面语言里顶层 `let NAME: [u8]` 会额外发布同名取址过程
  `#proc NAME() -> #addr`（`LowerDecl.cpp` 的 `buildAccessor`），而归档的 `lainir_data_ro_head`/`lainir_data_rw_head`/
  `lainir_data_rw_bare_head`/`lainir_return_base_close` 四个名字同时用作数据块与过程。过程名是对外契约
  （`records.lain`/`let.lain`/`sums.lain` 都调用它们），故**只改数据绑定名为 `NAME_text`**，过程名不动，并把这四个
  过程体里的取址改为 `symbol_addr("NAME_text_storage")`；已确认没有其他单元引用旧数据符号，且四个过程发出的
  `lainir_write` 长度（6/14/6/18）与归档一致。`compare_fragment.py` 因此报 4 处 data 改名（预期内、唯一的有意偏差）。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- `meta.lain` 已翻译（**最后一片，28/28 完成**）：42 个实现过程名与签名与旧片一致，128 个跨单元依赖以 extern 声明，无静态数据。
  切成 6 组并行翻译（part1 是单个 607 行的 `meta_emit_expr`，其余 7–9 个过程/组），主代理按归档过程顺序合并 42 个过程体。
  fixture（clean unit + 17 个他单元数据 stub）退出码 0、187 过程；`compare_fragment.py` OK（42/42 签名）；
  `compare_call_order.py` 39/42 ok，3 处差异逐条核对后全部良性：`meta_lower_struct`/`meta_lower_typed` 是平坦守卫
  把 `lain_meta_fail` 提到条件之后（**调用多重集完全相同**，10/10、11/11），`meta_operand_is_addr` 是把归档
  `%k` 值 if 的假支（类型解析路）提为提前返回——归档里该支文本上在后、执行上走同一路径，两支都以 `kind == 3` 返回。
  未运行 Meta。旧 `.l1` 将在翻译提交后单独删除。
- **28/28 片全部翻译完毕**，`bootstrapl1/SOURCE_ORDER` 对应单元已全部由 `bootstrap/` 下的 Lain 源替代。

## 组装与收尾（终局）

- **28/28 全部完成，整份 bootstrap 已可编译**：`scripts/assemble_bootstrap.py` 把 28 个单元拼成单文件
  `build/lain0/assemble.lain`（550578 字节；733 个顶层定义、丢弃 649 条内部 extern、保留 33 条宿主 extern），
  `./build/lain0/lain0.exe build/lain0/assemble.lain` **退出码 0**，发出 **869 个过程 / 215 个数据块**。
  未运行 Meta（本轮验收始终是 compile-only）。
- 迁移过程中修掉的两个 lain0 缺陷（同一类根因，均为「实参降级时丢掉了期望类型」）：
  1. 作为**调用实参**的嵌套调用不分配结果临时量（`lowerCall`）——`return inner(inner(y));` 报 verify 2020，提交 `e151078`；
  2. 作为 **`break`/`continue` 实参**的嵌套调用同样如此（`lowerJumpStmt`）——多路子代理在 `meta.l1` 翻译中被迫把
     `break pick(meta_tid_repr_kind(s_tid))` 拆成两个 `let` 才过闸。两处都改为传入该表达式在 Sema 里定下的类型
     （`sema_->exprTypes`）。均以 `bash scripts/regress.sh --round-trip` 作回归闸门。
- 若日后新增单元或改动 SOURCE_ORDER，重跑上面两条命令即可复核。

逐片 compile-only 闸门必须为跨片被调过程写 `extern` 声明（lain0 CLI 只接受单个输入文件）；而旧手写单元只声明**宿主**函数
（`#extern` 仅出现在 `lainir.l1` / `funcs.l1`），跨片调用靠 SOURCE_ORDER 拼接后在同一模块内解析。所以把已翻译单元
原样拼起来会撞 `verify code 2022 duplicate subroutine`：某片定义了另一片声明为 extern 的过程。

组装算法（脚本 `scripts/assemble_bootstrap.py`，收在 lain0-compiler 技能里）：

1. 按 `SOURCE_ORDER` 收集已存在的 `bootstrap/<unit>.lain`；
2. 汇总所有 `func` / `let` 顶层定义名；
3. 丢掉名字落在定义集合里的 `extern` 行，其余 `extern`（宿主函数）保留；
4. 写出单文件，并列出仍未解析的被调过程及其归属归档单元。

2026-10-14 演练结果：23 片参与组装、306 个顶层定义、259 条内部 extern 被丢弃、60 条宿主 extern 保留；
拼装后唯一的未解析被调过程是 `lainir_repr`（属尚未翻译的 `std/lainir.l1`）。这说明逐片 extern 是闸门脚手架，
不是最终程序的一部分；也说明组装一致性可以在剩下五片翻译完之前持续复查。

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
