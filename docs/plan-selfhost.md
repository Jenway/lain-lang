# 计划：推进 bootstrap 自举（让 Meta 编译它自己）

面向 HEAD `4896da2`（分支 `master`）。依据是实测报告 `docs/selfhost-gap.md`（53 行三态表、
77 条探针原始记录、11 的因果链）。本计划只做一件事：**让跑在 LainVM 上的 Meta 能降级
`bootstrap/` 自己的 28 个单元，产出能被 seed parser+verifier 装回去的 LAINIR。**

## 一、现状（全部实测）

**两条编译路径要分清：**

| 路径 | 谁编译谁 | 状态 |
| --- | --- | --- |
| lain0（C++）编 bootstrap | 28 个 Lain 模块 → **每模块一份** LAINIR（整批指纹 `ef2f198c…`） | **已通**：零诊断、两次跑整批指纹相同、在 VM 上 `host_status=0` |
| Meta（Lain）编源码 | 样例 → LAINIR | 26/38 绿，12 红 |
| **Meta 编 bootstrap 自己** | 28 个单元 → LAINIR | **未通**，基线 **0/28** |

**自举的等式**：Meta 必须实现它自己源码所用的那部分语言。第 4 步（`docs/plan-modules.md`）
把 bootstrap 源码搬到了 `import` 上（180 条 import、2482 处跨单元调用、167 处跨模块取值），
于是 Meta 被要求实现一个它的源码现在离不开的特性。

**两道墙，性质不同：**

| 墙 | 现象 | 规模 |
| --- | --- | --- |
| 降级期 | 单元降级中途返回非零 | 53 行差集表：能 24 / 半 4（status=0 但产物装不回去）/ 不能 25 |
| 执行期 | `LAINVM_TRAP_LOAD_FAULT 1004`，`host_status` 仍 0、产物截断 | 19 个含 `loop` 的单元、131 处 `loop` |

**`host_status=11` 的真相（纠正一个流行误解）**：11 出在 `bootstrap/std/scope.lain` 的
`meta_scope_scan_root` import 分支 —— `meta_source_find` 返回 NOT_FOUND 就 `lain_meta_fail(11)`，
位置在**降级之前的预扫描**。它**不是**能力缺口：Meta 的 import 匹配是「spec 规范化后与登记名
**逐字节相等**」，而 bootstrap 源码写的是相对导入方目录的 `import("types")`。同一份源码换登记名：
清单路径形态的登记名（当时的 `SOURCE_ORDER` 路径写法，那份清单已在步 3 删除）→ **11**；规范化 spec → **6**（1803 字节）；少 `.lain` → **0 + trap 1004**。
**这是测试口径问题**，单元级实测一律用「登记名 = 源码里的 spec」绕过（第 8 步才真正解决）。

**一条很值钱的好消息**：bootstrap 源码**完全没有**用到 `struct` / `enum` / `ref` / `slice` /
`for` / 赋值语句 / 本单元 `t[i]` / 函数体字符串字面量。所以 `u8math` / `strinline` / `refs` 等
红样例**大多不阻塞自举** —— 它们属于「语言覆盖面」，而自举只要求覆盖**自己源码用到的**那部分。

**驱动侧限制**：`LAINVM_SPACE_MAX_REGIONS=64`，每份源码占 3 个区段 → 一次最多登记 ~20 份；
28 个模块的闭包要 **62 条登记 / 189 个区段**。

## 二、目标

1. **单元级**：Meta 能降级叶子单元（先 `std/regions`）并产出 `PARSE+VERIFY OK` 的 LAINIR。
2. **整份**：28 个模块一次跑通（登记 62 → 28，区段 189 → ≤87）。
3. **判据统一**：每一步的验收都是 **`host_status=0` 且 `./build/check_unit.exe <产物>` = `PARSE+VERIFY OK`**。
   只看 `host_status` 会把 trap 出来的坏产物当通过 —— 这正是 trap 能"假装成功"的原因。

## 三、判据与工具（不可协商）

```bash
# 单次降级（登记名 = 源码里的 spec，单元级口径）
./build/lain-meta.exe --units-dir build/lain0/units/ --out <新路径> --lower 0 \
    std/regions.lain bootstrap/std/regions.lain types.lain bootstrap/std/types.lain
# 判据：host_status=0 且
./build/check_unit.exe <新路径>      # 期望 PARSE+VERIFY OK
```

- **每次比对前先删 `--out` 目标**：失败的运行不写文件，陈旧产物会让判据假通过（已踩过多次）。
- 全局回归：`bash scripts/regress.sh --round-trip` 必须恒为 `PASS=38 FAIL=0`。
- **改的是 Meta 本体**，所以每个提交都必须复跑 Meta 管线：
  `python scripts/build_meta_unit.py` 后跑 arith，必须 `host_status=0` / `region grants=2` /
  `output bytes=74` / 与 `build/arith.out` 逐字节相同。

## 四、九步工作项

| 步 | 改哪 | 依赖 | 验收 |
| --- | --- | --- | --- |
| 1 mangle 按模块键 | `std/modules.lain`: `meta_build_mangle`(410-441)、`meta_build_path`/`meta_source_find`(143-248) | — | `g1_imp_ok` 产物从 `#proc probe/pkg/a__av()`（名字带 `/`，3001）变 `<键>__av` 且装回 OK |
| 2 跨模块函数成员调用 | `std/modules.lain`: `meta_emit_ns_operand`(641-701)、`meta_emit_ns_call`(471)、导出路径(321-370/529-575) | 步 1（名字） | `g3_member_call` 从 4 变 0 且装回 OK |
| 3 `symbol_addr` 内建 | `std/lex.lain` 加关键字（照 `meta_kw_ptr_of` 22-27）+ `std/funcs.lain` `meta_is_builtin_name`(1070-1077) + `meta.lain` 发射分支 | 步 1 | `g4_symaddr_body` 从 9415 变 0；**`std/regions` 端到端 PARSE+VERIFY OK** |
| 4 **循环体 trap**（杠杆最大） | `std/funcs.lain`: `meta_lower_loop_stmts`(1423-1530) 的 `let` 分支 → `meta_lower_stmt`(1902) → `meta.meta_emit_expr`；查类型那条路 `meta_loop_type_name`(2378)/`meta_local_type_name` | — | 五条探针（`g8_op_in_loop_let` 等）从 trap+3001 变 0+OK；examples 的 `loop`/`loop_if`/`loop_value`/`loop_local`/`jumps` 从 rc=1 变 rc=0 |
| 5 u8 算子表项 | `std/scalars.lain`: `meta_scalar_table`（第 56 行的 587 字节表）给 `u8`/`i8` 补 `eq ne ult ule ugt uge add sub mul udiv urem and or shl lshr` | — | `g4_u8cmp`/`g4_u8arith` 从 6 变 0 且装回 OK |
| 6 链式比较 | `meta.lain`: `meta_emit_expr`(1193) 的算子折叠 —— 右操作数要**递归**降级成子表达式 | — | `g9_chain_cmp_*` 从 6 变 0 且装回 OK；`std/lex` 越过 1803 字节的卡点 |
| 7 `ptr_of` 产物 | `std/funcs.lain`: `meta_is_builtin_name` + `meta_emit_builtin_temp`(2122) 的 `ptr_of`/`addr_of` 支：`#ptr2int`/`#int2ptr` → `#bitcast` | — | `g4_ptr_of`/`g6_ptr_of_addr_of` 从 `VERIFY FAILED 2005` 变 OK |
| 8 import 路径解析 | `std/modules.lain`(`meta_source_find`/`meta_build_path`) + `std/scope.lain`(`meta_scope_add`:445、`meta_scope_scan_root`:661) + **驱动侧** `seed/meta_driver.c:176` 登记名与区段预算 | 步 1 | `g10_lex_sorder` 与 `g10_lex` 结果一致；`selfhost_units.py` 里「今天可跑」4 → 28 |
| 9 闸门化 | `scripts/` 加一个脚本：每单元 `lain-meta --out` + `check_unit`，非零即红 | 全部 | 基线 0/28 → 目标 28/28 |

**没有一步需要改 `seed/lain0/**` 或 `seed/kernel/**`**：lain0 的模块语义（`Sema.cpp:1047`
的 `keyOf`）就是目标语义，kernel 的区段上限是驱动侧要绕的约束。缺口全在 Meta 自己
（`bootstrap/std/**`）加一处驱动登记。

## 五、里程碑（按最快见效排序）

| 里程碑 | 内容 | 完成判据 |
| --- | --- | --- |
| **A. 第一个端到端单元** | 步 1 + 2 + 3 → `std/regions`（24 行、无循环） | `std/regions` 的 Meta 降级 `PARSE+VERIFY OK` |
| **B. 循环族解锁** | 步 4（唯一一条 bug 形态的缺口） | 5 个 loop 样例从 rc=1 变 rc=0；`g10_types` 越过 279 字节 |
| **C. 依赖链打通** | 步 5 + 6 + 7 → `std/types` → `std/lex` → `std/registry` | 三个单元各自 `PARSE+VERIFY OK` |
| **D. 整份 28 模块** | 步 8（含驱动登记与区段预算） | `selfhost_units.py` 28/28 可跑；登记 62→28 |
| **E. 闸门化与收尾** | 步 9 | 脚本化的 28/28；文档与决策更新 |

## 六、边界与红线

1. 改动范围：`bootstrap/std/**`、`bootstrap/meta.lain`、`bootstrap/std/lex.lain` 与
   `seed/meta_driver.c`。**不改 `seed/lain0/**`、`seed/kernel/**`**。
2. **改 Meta 本体 = 改整个系统的编译器**，每个提交都要复跑：`regress.sh --round-trip`（38/38）
   + Meta 管线 arith（`host_status=0` 且与 `build/arith.out` 逐字节相同）。少了这两条，
   你就是在用一个坏掉的编译器验证一个坏掉的编译器。
3. 判据只用「`host_status=0` **且** `check_unit` OK」。任何"status 是 0 就算过"的写法都作废。
4. 每步一个 commit，提交信息带上该步的验收命令与真实输出。
5. 每步开跑前先确认 `build/probe/selfhost/run_all.tsv` 里该步的探针**改动前**是什么结果，
   改完必须逐条对比；不许把失败的探针从表里拿掉。

## 七、风险与未定项

| 项 | 说明 |
| --- | --- |
| 步 4 可能比预期深 | 它是唯一的 bug 形态（其余是"没实现"）。触发条件已剥到「循环体里 `let` 初值含算子且左操作数是名字」，但根因是节点文本地址被取坏 → `std_types.meta_bytes_equal` 越界读（`bootstrap.l1:916`），修法可能是"查类型那条路"本身要重写 |
| 步 8 要动驱动 | 区段预算（每份源码 3 → 1-2 个）与登记口径都要改；改驱动要同时保 arith 管线不变 |
| mangle 统一是规格级 | Meta 向 lain0 看齐（每个 `/` → 一个 `_`），这意味着**建议追加决策 D-005**：自举口径以 lain0 的模块键语义为准；判据为「`host_status=0` 且产出装得回去」 |
| 自举成功之后 | 手写 IR 档案早已删除（`5fbe5d5~1` 是最后一版）。真正的闭环是：Meta 编出的 Meta（LAINIR）装回 VM 再编一次，产物与本步产物逐字节相同 —— 那是**下一个**里程碑，不在本计划内 |
| 未知未定 | 差集表里 4 行「半」的具体修法未定（status=0 但装不回去）；它们不在里程碑 A-C 的必经路上，碰到再定 |

## 八、建议追加的决策（D-005）

**What**：自举的口径以 **lain0 的模块键语义**为准 —— 模块键 = 源根相对路径去 `.lain`、`/` 换 `_`；
Meta 的 mangle 与 `symbol_addr` 一律产出 `<键>__<名字>`；每一步的验收判据固定为
「`host_status=0` 且 `./build/check_unit.exe` 给 `PARSE+VERIFY OK`」。

**Why**：kernel 按平坦字符串解析名字，两边键不一致就永远互不了链；而只看 `host_status`
会把 trap 出来的截断产物当成功（`docs/selfhost-gap.md` §4.4 有实证）。

**How**：Meta 侧改 `std/modules.lain` 的 mangle 与 `meta_source_find`；lain0 侧不动。

**Status**：待定（本计划提交后确认）。


## 八、实测补充（来自差分测试 `scripts/diff_two_compilers.py`）

- **驱动侧是当前覆盖率的天花板**：28 个模块里有 24 个的 import 闭包需要 **63 条登记**，驱动
  `DRIVER_MAX_SOURCES=32` 直接拒绝（`rc=2 源码太多`）——**不是 Meta 的能力问题**。能进驱动的只有
  4 个：`std/lex`、`std/regions`、`std/registry`、`std/types`。第 8 步的"登记与区段预算"因此是硬前提。
- **第 1 步的范围要扩大**：Meta 现在连被降级单元**自己**的顶层定义都不加模块前缀——产物发
  `meta_region_scratch_record`，而 lain0 发 `std_regions__meta_region_scratch_record`；数据符号还少
  `_storage` 后缀（lain0 `…_record_storage`，Meta `…_record`）。对照实验：把被降级单元的逻辑路径换成
  6 种写法（含 `std/regions`、`std__regions`、`std::regions`），Meta 产出一模一样、**全是扁平名**——
  所以问题不在 import 站，而是**单元自己的定义压根没走 mangle**。第 1 步要同时覆盖两处。
- **当前基线（就是这么差，不要美化）**：4 个能跑的模块全部非零——`std_lex`=6、`std_regions`=9415、
  `std_registry`=4、`std_types`=0+VM trap；四份产物都被截断（`check_unit` 全报 `PARSE FAILED 3001`）。
  逐字相同 0、规范化后相同 0。
- 看表：`python scripts/diff_two_compilers.py`（**观测工具，rc 恒 0**；`--strict` 有红行才 rc 1）。
  每修好一处，对应的行会变绿——这是自举进度的唯一客观仪表。
