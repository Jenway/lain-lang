# 自举差集：让 Meta 降级 bootstrap 自己的源码（实测报告）

> 本文件只做**调查**，不改 `bootstrap/` 与 `seed/` 的源码。所有结论都附命令与输出；
> 探针与脚本放在未跟踪的 `build/probe/` 下，本文把关键探针的源码与命令抄进来。
>
> 基线：本仓库工作区，`cmake --build build` 后 `build/lain-meta.exe`、`build/lain0/lain0.exe`
> 均最新。回归 `bash scripts/regress.sh --round-trip` PASS=38 FAIL=0。

## 0. 调查结果

1. **`host_status=11` 的精确来源**：`bootstrap/std/scope.lain:619-622` 的
   `meta_scope_scan_root` —— 顶层 `let NAME = import("…")` 那条分支里
   `std_modules.meta_source_find` 返回 `NOT_FOUND` 时 `lain_meta_fail(11); return 11`。
  它在**降级之前**的预扫描里就被抛出（`meta.lain:66` 的 `meta_scope_scan`），
   所以「完全没有产物」——这是 11 与 4/6/13 那类「降级到一半」失败的分界线。
2. **四个单元都报 11 来自登记名不匹配**：Meta 的 import 解析是
   「把 spec 规范化后与宿主登记的逻辑路径**逐字节相等**」，而 bootstrap 源码里的
   spec 是**相对导入方目录**写的（`import("types")` = `std/types.lain`）。
   用 SOURCE_ORDER 路径登记（自然写法）→ 4 个单元全 11；把登记名换成源码里
   spec 的规范化形态 → `std/lex` 立刻走到 **6**（1803 字节产物），
   `std/registry` 走到 **4**（442 字节），`std/types` 走到 **0 + 1004 trap**，
   `std/regions` 走到 **9415**。
3. **差集表共 53 行**（§4.1 导入层 7 行 + §4.2 跨模块成员 8 行 + §4.3 单模块构造 24 行 +
   §4.4 循环族 14 行），另有单元级 4 行与红样例 12 行。分布：**能 24 行 / 半 4 行 /
   不能 25 行**，「未知」0 行 —— 每一行都有 `build/probe/selfhost/run_all.tsv` 里的
   真实 status 与装回判定。
4. **第一件该做的事（按杠杆）**：修**循环体 trap**（`let` 初值含算子、左操作数是名字
   → `LAINVM_TRAP_LOAD_FAULT 1004`，产物截断、`host_status=0`）。它是**唯一一条
   以 bug 形态出现的缺口**，19 个含 `loop` 的单元几乎全中，最小复现只有 6 行。
5. **最快的「第一个单元端到端」是 `std/regions`**（24 行、无循环）：只需要
   ①mangle 按模块键（`std_types__x`）②跨模块函数成员调用 ③`symbol_addr` 内建
   三项，全是别的单元也需要的公共项；`std/lex` 反而要 4 项（多一条链式比较）。
6. **几处容易看错的结果（见 §5）**：
   - `modules_main` / `modules_twin` 是 **11 / 0 字节**：只登记样例自己，import 找不到源码；
   - `jumps` / `loop*` 5 个样例的失败形态是 **LOAD FAULT trap（status=1004，落在
     `std_types__meta_bytes_equal` 的循环体）**，产物截断是 trap 的后果；
   - `symbol_addr` 在**目标单元**里（函数体或顶层绑定）都报 **9415**；「被导入模块
     **导出**这条绑定」报 13，两处来源不同（§5.3）；
   - 跨单元调用点 **2482 处**，另有 167 处跨模块取值；
   - bootstrap 源码**没有** `struct`/`enum`/`ref`/`slice`/`for`/赋值语句/本单元表索引 ——
     这几类缺口对自举**不构成阻塞**（但样例里仍然红）。

---

## 1. 判据与工具

### 1.1 三态口径（本文件不用「未知」）

| 判定 | 含义 | 判据 |
| :--- | :--- | :--- |
| **能** | Meta 能降级，且产物是合法 LAINIR | `host_status=0` **且** `check_unit.exe <产物>` 输出 `PARSE+VERIFY OK` |
| **半** | Meta 降级完成，但产物装不回 VM | `host_status=0`，`check_unit.exe` 报 `PARSE FAILED 300x` / `VERIFY FAILED 20xx` |
| **不能** | Meta 自己失败 | `host_status != 0`，或 VM trap（`trap kind=1 status=1004`），产物截断 |

每一行的结果都来自同一轮探针实测，登记名、源码与结果记在 §4 各表。

### 1.2 两条命令

```bash
# 让 Meta 降级一份源码（--lower 是「第几个位置参数是目标」，位置参数成对：登记逻辑路径 + 文件）
./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "p/leaf.lain" build/probe/selfhost/src/p_leaf.lain

# 「产出装得回去」闸门：把产物当 LAINIR 单元重新 parse + verify
./build/check_unit.exe build/probe/selfhost/g1_leaf.out
# -> PARSE+VERIFY OK ...
```

`--unit build/lain0/bootstrap_unit.txt` 里只有一行 `../build/lain0/bootstrap.l1`，
即 `scripts/build_meta_unit.py` 产出的**整份 Meta**（524574 字节，`verify subroutines=709
data=167`）。`check_unit.exe` 是 `build/check_unit.c` 编出来的临时工具（只调
`lainir_parse` + `lainir_verify`），源码不在版本库里，属本仓 `build/` 惯例。

### 1.3 调查脚本

| 脚本 | 作用 |
| :--- | :--- |
| `selfhost_census.py` | 28 个单元源码的构造普查（数量 + 分布 + 首条样例位置） |
| `selfhost_units.py` | 每个单元的 import 闭包 / 需要的登记条数 / 驱动区段数（能不能今天跑） |
| `selfhost_run.py` | 76 个探针批量实测，写 `selfhost/run_all.tsv`（含每条的完整命令） |
| `gate8.sh` | 38 个 examples 全量过 Meta（`selfhost/gate8_self.tsv`，与 `build/probe/gate8_head.tsv` 逐字节相同） |

---

## 2. `host_status=11` 的精确来源

### 2.1 11 有两个语义（先分清）

| 站点 | 函数 | 条件 |
| :--- | :--- | :--- |
| `std/scope.lain:619-622` | `meta_scope_scan_root` | 顶层 `let NAME = import("…")`：`meta_source_find` 返回 `NOT_FOUND` → `lain_meta_fail(11); return 11` |
| `std/modules.lain:358` | `meta_lower_import` | 逻辑路径**语法**不对（`import(` 后没有引号串） |
| `std/modules.lain:366` | `meta_lower_import` | 找不到被导入源码 |
| `std/modules.lain:599` | `meta_lower_ns_value` | `let x = M.member` 时找不到源码 |
| `std/modules.lain:653` | `meta_emit_ns_operand` | 表达式里的 `M.member` 找不到源码 |
| `std/funcs.lain:1951 / 1970 / 2007 / 2013` | `meta_lower_stmt`（1902） | **有意**的 11：局部构造/局部绑定的聚合要逃出帧（`build/probes/escape*.lain` 记的就是它） |
| `std/scalars.lain:293-297` | `meta_type_resolve` | 限定类型名找不到源码 → 返回 **0**（不是 11） |

所以看到 11 要先分：**import 找不到源码**（前五个）还是**聚合逃逸被拒**（funcs 那四个）。
本任务关心的自举失败是前者。

### 2.2 因果链（最小复现）

探针 `build/probe/selfhost/src/p_imp_missing.lain`：

```lain
let A = import("nope");
let z: u64 = 1;
```

```bash
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "p/missing.lain" build/probe/selfhost/src/p_imp_missing.lain
sources count=1
scratch bytes=...
image ok
run slice=2 rounds=21 steps=40809355 host_status=11
trap none
result kind=1 width=32 bits=11
diagnostic code=11 source=18446744073709551615 offset=18446744073709551615
output bytes=0
```

链条：

1. 驱动把源码按**位置参数里写的字符串**登记（`seed/meta_driver.c:176`，
   `sources[i].path = dup_cstr(argv[i])`），Meta 侧读它的是
   `lain_meta_source_path_data(i)`。
2. `lain_std_lower`（`bootstrap/meta.lain:37`）先 `meta_expand_declared_source`（无宏 → 0），
   再 `meta_scope_setup(1)`、`meta_scope_set_current(source)`，然后
   **`std_scope.meta_scope_scan(base, len, source, mark)`（`meta.lain:66`）**。
3. `meta_scope_scan_root`（`scope.lain:661`）逐条看顶层声明；碰到
   `let NAME = import("…")`（`scope.lain:608` 认出 `import` 那个词，分支体到 625）→
   取引号内字节 → `meta_source_find`。
4. `meta_source_find`（`modules.lain:191`）把 spec 规范化
   （`meta_build_path`：`::`→`/`，末尾补 `.lain`），然后**逐字节**与每份已登记源码的
   逻辑路径比（`meta_cstr_equal`）——**没有「相对导入方目录」这一步**。
   `import("nope")` → `"nope.lain"`，与登记名全不等 → 走
   `break s(0xFFFFFFFFFFFFFFFF)`（`modules.lain:244`）。
5. 回到 `scope.lain:619-622`：`idx == NOT_FOUND` → `lain_meta_fail(11)`（把宿主 status
   置成 11）→ `return 11` → `meta_scope_scan` 返回 11 → `lain_std_lower` 返回 11。
   **降级阶段（`meta_lower_source`）根本没进**，所以 `output bytes=0`。

**判别实验**（证明 11 出自预扫描阶段）：把 import 放到一条可降级语句**之后**：

```lain
let z: u64 = 1;
let A = import("nope");
```

```bash
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "p/miss2.lain" build/probe/selfhost/src/p_miss2.lain
run slice=2 rounds=21 steps=... host_status=11
output bytes=0            # ← z 的 #proc 一个字节都没发出来
```

对照：同形状但 import 可解析时（`g5_imp_ok_after`）产物 89 字节，
`#proc z() { #return 1 }` 与 `#proc probe__a__av() { #return 7 }` 都在。
→ 11 在**任何降级之前**发生，站点就是 `scope.lain:618`。

### 2.3 为什么 bootstrap 的 28 个单元在自然登记下**必然**撞 11

Meta 的规范化规则（`meta_build_path` + `meta_source_find`）：
`spec` 里 `::`→`/`、末尾补 `.lain`，然后与登记名**全串相等**。

| 源码写的 spec | Meta 规范化后 | 必须登记的字符串 |
| :--- | :--- | :--- |
| `import("types")` | `types.lain` | `types.lain` |
| `import("handlers/let")` | `handlers/let.lain` | `handlers/let.lain` |
| `import("../emit")` | `../emit.lain` | `../emit.lain` |
| `import("std/lex")` | `std/lex.lain` | `std/lex.lain` |

而 bootstrap 源码里的 spec 是**相对导入方目录**写的（lain0 的语义，
`seed/lain0/src/Sema.cpp:79` `resolveModulePath`：先试 `<导入方目录>/<模块名>.lain`，
再试 `<导入方目录>/<限定前缀>/<模块名>.lain`；模块键 = 相对源根路径去 `.lain`、`/`→`_`）。
于是 `std/lex.lain` 里的 `import("types")` 在 lain0 眼里是 `std/types.lain`，
在 Meta 眼里是字面量 `types.lain` —— 两边要互链必须先统一（§5 第 3 条就是这件事）。

**实测对照**（同一份源码，只换登记名）：

```bash
# A. 登记名 = SOURCE_ORDER 路径（自然写法）
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "std/lex" bootstrap/std/lex.lain "std/types" bootstrap/std/types.lain \
    "std/regions" bootstrap/std/regions.lain
run ... host_status=11   output bytes=0            # g1_lex_sorder / g10_lex_sorder

# B. 登记名 = 源码里 spec 的规范化形态
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "std/lex.lain" bootstrap/std/lex.lain "types.lain" bootstrap/std/types.lain \
    "regions.lain" bootstrap/std/regions.lain
run ... host_status=6    output bytes=1803         # g1_lex_basename / g2_lex / g10_lex
```

同一份 `std/lex.lain`、同一份 Meta，**只换登记字符串**就从「0 字节 + 11」变成
「1803 字节 + 6」。所以 11 在这里来自**路径解析语义**，Meta 的表达能力足够。

补一条：登记名少写 `.lain`（`"types"`，源码里是 `"types.lain"`）时，Meta 补成
`types.lain` 后照样不等，这次的结果是 1004 trap：

```bash
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "std/lex.lain" bootstrap/std/lex.lain "types" bootstrap/std/types.lain
run slice=3 rounds=28 steps=55133749 host_status=0
trap kind=1 status=1004 line=… column=34 region=… position=0
output bytes=0
```

（报告里的 `line=` / `region=` 取自测量时的 `bootstrap.l1`；源码编辑会让它们移动。）

连跑两次 steps 完全相同（确定性）；`types` 放前面、`--lower 1` 也一样。
trap 落在 `build/lain0/bootstrap.l1` 里
`#proc std_types__meta_cell_get_raw` 的 `#load[#bits<64>](%cell)`，
调用点在 `meta_source_find` 的**诊断前导**（`modules.lain:230-237`，
读 `scratch + meta_path_buffer()` 那 8 字节）。这两行是排查期加的脚手架，
`modules.lain:231-233` 的注释记着作者的怀疑：
「判断路径缓冲区是否被语义旁表覆盖（`%buf` = scratch+13448，而旁表也从此处开始）」。
→ **第二层根因**：`meta_path_buffer()`（`scope.lain:174-182`，位置 =
`sc_base + 作用域表字节数 + 依赖区字节数`，随作用域表增长而**移动**）在某些配置下
算出的缓冲区落在不可读的位置/被语义旁表覆盖，于是同一段逻辑有时返回 11、
有时直接 1004。修 import 那一层时必须把这块一起理清（否则修完仍会间歇性 trap）。

---

## 3. bootstrap 源码实际用到的构造（普查）

`python build/probe/selfhost_census.py`（28 个单元，注释已剔除）：

| 构造 | 次数 | 涉及单元 |
| :--- | ---: | ---: |
| 跨单元**函数**成员调用 `alias.name(…)` | **2482** | 28 |
| 跨模块**数据**成员取值 `alias.name` | **167** | 15 |
| `symbol_addr("…_storage")` | **328** | 12 |
| 顶层 `ro [u8]` 静态表 | 207 | 11 |
| 顶层定长表（rw） | 9 | 5 |
| `extern … = link_name;` | 136 | 27 |
| `import("…")` | 180（90 裸名 / 50 `../` / 38 子目录 / **0 个 `::`**） | 28 |
| `loop LABEL(参数…)` | 131 | 19 |
| `break LABEL(值)` / `continue LABEL(实参…)` | 339 / 135 | 20 / 19 |
| `let x: T = if …`（带值 if） | 147 | 20 |
| `as u32` / `as u64` / `as addr` / `as u8` | 98 / 25 / 1 / 3 | 17 / 10 / 1 / 1 |
| `: u8` 标注 | 114 | 20 |
| 十六进制字面量 | 274 | 19 |
| 负数字面量 | 27 | 9 |
| `load(` / `store(` / `ptr_of(` | 24 / 11 / 2 | 7 / 4 / 1 |
| 字符串字面量 | 180 在 `import` 里 + 328 在 `symbol_addr` 里 + **0 在别处** | — |
| `struct` / `enum` / `ref` / `slice` / `for` / 赋值语句 / 本单元 `t[i]` | **0 / 0 / 0 / 0 / 0 / 0 / 0** | — |

最后一行很关键：**bootstrap 不用聚合值层、不用引用/切片、不用 `for`、不用赋值语句、
不索引自己的静态表**。这些在 examples 里是红的（§4.6），但**不是自举缺口**。

---

## 4. 自举差集清单（三态表，无「未知」）

判据见 §1.1。所有行都来自 `build/probe/selfhost/run_all.tsv`（第一列就是下面每行的 # 对应的
`tag`，可 grep 到该条的完整命令与原始输出）。
命令模板：`./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 <登记名> <文件>`，
下表的「命令」列只写 `--lower` 之后的参数（登记名 文件 对，`src/` = `build/probe/selfhost/src/`）。

### 4.1 导入层

| # | 构造（bootstrap 用量） | 最小样例 | 命令（`--lower` 之后） | 实测 | 判定 |
| :- | :--- | :--- | :--- | :--- | :--- |
| 1 | `import("裸名")` + 登记名 = 规范化 spec（90 处） | `let A = import("probe::a");` + 模块 `let av: u64 = 7;` | `"p/ok3.lain" src/p_ok3.lain "probe/a.lain" src/pkg_a2.lain` | `status=0`，89 字节，装回 **OK**（`#proc probe__a__av(){#return 7}`） | **能** |
| 2 | 同上，但 spec 含 `/`（38 处子目录 import） | `let A = import("probe/pkg/a");` | `"p/ok.lain" src/p_imp_ok.lain "probe/pkg/a.lain" src/pkg_a.lain` | `status=0`，产物 `#proc probe/pkg/a__av()` → 装回 **PARSE FAILED 3001**（名字里有 `/`，不是合法标识符） | **半** |
| 3 | 登记名 = SOURCE_ORDER 路径（自然写法） | `std/lex.lain` 的 `import("types")` | `"std/lex" bootstrap/std/lex.lain "std/types" bootstrap/std/types.lain "std/regions" bootstrap/std/regions.lain` | `host_status=11`，`diagnostic code=11`，**0 字节** | **不能** |
| 4 | 被导入模块**自己**的 import 是相对名 | `probe/pkg/b.lain`: `let A = import("a");` 只登记 `probe/pkg/a.lain` | `"p/chain.lain" src/p_chain.lain "probe/pkg/b.lain" src/pkg_b.lain "probe/pkg/a.lain" src/pkg_a.lain` | `host_status=11`，0 字节 | **不能** |
| 5 | 同上，但把同一个文件**再登记一个别名** `a.lain` | 同上 + `"a.lain" src/pkg_a.lain` | `"p/chain.lain" src/p_chain.lain "probe/pkg/b.lain" src/pkg_b.lain "probe/pkg/a.lain" src/pkg_a.lain "a.lain" src/pkg_a.lain` | `status=0` 但 **1004 trap（line 954）**，0 字节 | **不能** |
| 6 | 登记名少 `.lain` | 同 #3，把 `std/types` 登记成 `types` | `"std/lex.lain" bootstrap/std/lex.lain "types" bootstrap/std/types.lain` | `status=0` + **1004 trap（line 954）**，0 字节（确定性，两次 steps 相同） | **不能** |
| 7 | 只登记直接依赖，不管传递依赖 | `std/lex` + `types`（不登记 `regions`） | `"std/lex.lain" bootstrap/std/lex.lain "types.lain" bootstrap/std/types.lain` | `host_status=11`，0 字节 | **不能** |

### 4.2 跨模块成员（2482 + 167 处）

| # | 构造 | 最小样例 | 命令 | 实测 | 判定 |
| :- | :--- | :--- | :--- | :--- | :--- |
| 8 | 跨模块**值**成员当操作数（数字初值） | `let A = import("probe::m_av"); let x: u64 = A.av;` | `"p/mv.lain" src/p_mv.lain "probe/m_av.lain" src/m_av.lain` | `status=0`，115 字节，装回 **OK** | **能** |
| 9 | 跨模块**值**成员（静态表） | 模块 `let t: [u8] = [1,2,3];`，探针 `func f() -> addr { return A.t; }` | `"p/mtv.lain" src/p_mtv.lain "probe/m_tbl.lain" src/m_tbl.lain` | `status=0`，产物 `%r1 = #call probe__m_tbl__t()` → 装回 **VERIFY FAILED 2015 `#call: unknown callee`** | **半** |
| 10 | 跨模块**函数**成员调用 | 模块 `func g(v: u64) -> u64`，探针 `return A.g(1);` | `"p/mc.lain" src/p_mc.lain "probe/m_fn.lain" src/m_fn.lain` | `host_status=4`，25 字节，产物截断 | **不能** |
| 11 | 跨模块函数成员 + 值成员实参 | `return A.g(A.av);` | `"p/mcn.lain" src/p_mcn.lain "probe/m_both.lain" src/m_both.lain` | `host_status=4`，80 字节 | **不能** |
| 12 | 跨模块静态表索引 `M.t[i]` | `let p: addr = A.t[i];` | `"p/mt.lain" src/p_mt.lain "probe/m_tbl.lain" src/m_tbl.lain` | `host_status=4`，69 字节 | **不能** |
| 13 | 跨模块类型名进参数类型 | `func f(p: A.Point) -> i32` | `"p/nty.lain" src/p_nty.lain "probe/m_ty.lain" src/m_ty.lain` | `host_status=4`，34 字节 | **不能** |
| 14 | 只声明 import、不取成员（隔离导出路径） | `let A = import("probe::m_av");` | `"p/mo.lain" src/p_mo.lain "probe/m_av.lain" src/m_av.lain` | `status=0`，53 字节，装回 **OK** | **能** |
| 15 | 被导入模块里**同时**有值成员与函数成员（只用值成员） | 模块 `let av` + `func g` | `"p/l2.lain" src/p_l2.lain "probe/m_both.lain" src/m_both.lain` | `status=0`，130 字节，装回 **OK** | **能** |

4 的来源是 `modules.lain:673-674`（`meta_emit_ns_operand` 的 `pick_chk` 里
`member.kind != 3` → `lain_meta_fail(4); break pick_chk(4)`）：**导出路径只发值绑定，
函数成员不可见**。
半 的来源是导出路径对静态表**什么都不发**（`meta_emit_module_binding`
（`modules.lain:529-575`）只认「数字」与「同模块标识符」两种初值）。

补一条（不在上表编号内）：被导入模块**导出**一条初值为 `symbol_addr` 的绑定时报 **13**：

```bash
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --lower 0 \
    "p/msym.lain" build/probe/selfhost/src/p_msym.lain \
    "probe/m_sym.lain" build/probe/selfhost/src/m_sym.lain
run ... host_status=13   output bytes=0
# m_sym.lain = `let t: [u8] = [1]; let a: addr = symbol_addr("t_storage");`
```

即「13 = 模块**导出**路径找不到那条绑定名」「9415 = **目标单元**里 `symbol_addr`
被当成普通调用」（#19/#20）。两者来自不同的路径。

### 4.3 单模块构造（bootstrap 主力形态）

| # | 构造 | 最小样例 | 命令 | 实测 | 判定 |
| :- | :--- | :--- | :--- | :--- | :--- |
| 16 | 同单元函数调用 | `func g…; func f() -> u64 { return g(1); }` | `"p/oc.lain" src/p_oc.lain` | `status=0`，101 字节，装回 **OK** | **能** |
| 17 | 顶层 `ro`/rw 定长表（207+9 处） | `let t: ro [u8] = [1,2,3];` | `"p/ro.lain" src/p_ro.lain` | `status=0`，59 字节，装回 **OK** | **能** |
| 18 | 本单元表索引 `t[i]`（bootstrap 用 0 处） | `let p: addr = t[i]; return load(p);` | `"p/rt.lain" src/p_rt.lain` | `status=0`，产物 `#load[#bits<8>](%t…)` → 装回 **VERIFY FAILED 2002 `undefined value t`** | **半** |
| 19 | `symbol_addr("…")` 在**函数体**里（328 处，12 个单元） | `func f() -> addr { return symbol_addr("t_storage"); }` | `"p/sab.lain" src/p_sab.lain` | `host_status=9415`，41 字节 | **不能** |
| 20 | `symbol_addr("…")` 在**顶层绑定**里 | `let a: addr = symbol_addr("t_storage");` | `"p/sat.lain" src/p_sat.lain` | `host_status=9415`，20 字节 | **不能** |
| 21 | `extern … = link_name;` + 调用（136 处） | `extern consume(p: u64) -> u64 = consume;` | `"p/ex.lain" src/p_ex.lain` | `status=0`，114 字节，装回 **OK** | **能** |
| 22 | 早退（`if` + `return`） | `if a == 0 { return 1; } return a;` | `"p/mr.lain" src/p_mr.lain` | `status=0`，111 字节，装回 **OK** | **能** |
| 23 | 带值 `if`（147 处） | `let x: i32 = if c { 1 } else { 2 };` | `"p/vi.lain" src/p_vi.lain` | `status=0`，126 字节，装回 **OK** | **能** |
| 24 | 十六进制字面量（274 处） | `return 0xff00;` | `"p/hx.lain" src/p_hx.lain` | `status=0`，43 字节，装回 **OK** | **能** |
| 25 | 负数字面量（27 处） | `return -1;` | `"p/ng.lain" src/p_ng.lain` | `status=0`，71 字节，装回 **OK** | **能** |
| 26 | `as u32` / `as addr` / `as u8` 窄化 | `let b: u8 = ch as u8;` | `"p/cu8.lain" src/p_cu8.lain` | `status=0`，82 字节，装回 **OK** | **能** |
| 27 | u8 **算子**（`==`、`+`；bootstrap 用 0 处） | `func f(a: u8, b: u8) -> bool { return a == b; }` | `"p/u8c.lain" src/p_u8c.lain` | `host_status=6`，50 字节 | **不能** |
| 28 | u8 先 `as u32` 再比较（bootstrap 的写法，98 处） | `let x: u32 = a as u32; … x == y` | `"p/u8o.lain" src/p_u8o.lain` | `status=0`，154 字节，装回 **OK** | **能** |
| 29 | `ptr_of`（2 处，`std/types`） | `return ptr_of(p);` | `"p/po.lain" src/p_po.lain` | `status=0`，产物 `#ptr2int[…]` → 装回 **VERIFY FAILED 2005**（D-002 已删的算子仍在发） | **半** |
| 30 | `load` / `store` | `store(p, v); return load(p);` | `"p/ls.lain" src/p_ls.lain` | `status=0`，121 字节，装回 **OK** | **能** |
| 31 | 参数上的字段访问 | `func f(p: P) -> i32 { return p.x; }` | `"p/fd.lain" src/p_fd.lain` | `status=0`，249 字节，装回 **OK** | **能** |
| 32 | 函数体内聚合构造（bootstrap 用 0 处） | `let p: P = P { x: 1 y: 2 }; return p.x;` | `"p/ct.lain" src/p_ct.lain` | `status=0`，381 字节，装回 **OK** | **能** |
| 33 | 函数体内字符串字面量（bootstrap 用 0 处） | `func f() -> addr { return "hi"; }` | `"p/sv.lain" src/p_sv.lain` | `host_status=4`，37 字节；装回 **VERIFY FAILED 2002 `undefined value r0`** | **不能** |
| 34 | 给局部变量**赋值**（bootstrap 用 0 处） | `let acc: i32 = 0; acc = acc + n;` | `"p/as.lain" src/p_as.lain` | `host_status=4`，69 字节 | **不能** |
| 35 | **链式比较** `a<b and c<d`（7 处：`std/lex` 2、`std/syntax` 5） | `let x: bool = a < b and b < 10;` | `"p/c1.lain" src/p_c1.lain` | `host_status=6`，117 字节；产物 `%r2 = #and[#bits<32>](%r1, %b)`（把右操作数当裸名字，宽度也取错） | **不能** |
| 36 | 链式比较出现在 `return` 位置 | `return a < b and b < 10;` | `"p/c4.lain" src/p_c4.lain` | `host_status=6`，117 字节 | **不能** |
| 37 | 三段链式比较 | `let x: bool = a < b and b < 10 and a < 1;` | `"p/c5.lain" src/p_c5.lain` | `host_status=6`，117 字节 | **不能** |
| 38 | 链式比较拆成两条 `let`（对照） | `let p: bool = a < b; let q: bool = b < 10; let x: bool = p and q;` | `"p/c2.lain" src/p_c2.lain` | `status=0`，159 字节，装回 **OK** | **能** |
| 39 | 链式**算术** `a + b + 1`（对照） | `let x: u32 = a + b + 1;` | `"p/c3.lain" src/p_c3.lain` | `status=0`，131 字节，装回 **OK** | **能** |

### 4.4 循环族（19 个单元、131 处 `loop`）

这一族的失败形态最重：**VM trap，`host_status` 还是 0，产物被截断**——
只按 `host_status` 判「成功」会把坏产物当通过。

| # | 构造 | 最小样例（`func f(n: i32) -> i32 { let t: i32 = loop outer(i: i32 = n) … }`） | 命令 | 实测 | 判定 |
| :- | :--- | :--- | :--- | :--- | :--- |
| 40 | 循环体只有 `break LABEL(值)` | `… while i > 0 { break outer(i); }` | `"p/le.lain" src/p_le.lain` | `status=0`，179 字节，装回 **OK** | **能** |
| 41 | 循环体 = 一条 `let`（字面量）+ `break` | `let x: i32 = 7; break outer(x);` | `"p/l1.lain" src/p_l1.lain` | `status=0`，208 字节，装回 **OK** | **能** |
| 42 | 循环体 `let` 初值是**裸名字** | `let x: i32 = i;` | `"p/q2.lain" src/p_q2.lain` | `status=0`，209 字节，装回 **OK** | **能** |
| 43 | 循环体 `let` 初值是**函数调用** | `let x: i32 = helper(i);` | `"p/o2.lain" src/p_o2.lain` | `status=0`，261 字节，装回 **OK** | **能** |
| 44 | 循环体 `let` 初值是算子、**两侧都是字面量** | `let x: i32 = 1 + 2;` | `"p/q1.lain" src/p_q1.lain` | `status=0`，208 字节，装回 **OK** | **能** |
| 45 | `break` 的实参是算子表达式 | `break outer(x + 1);` | `"p/o4.lain" src/p_o4.lain` | `status=0`，240 字节，装回 **OK** | **能** |
| 46 | **循环体 `let` 初值是算子、左操作数是名字** | `let x: i32 = i + 1;` | `"p/o1.lain" src/p_o1.lain` | `status=0` + **trap 1004**（落在 `std_types__meta_bytes_equal` 的循环体，当前 `bootstrap.l1` 第 790 行），136 字节，装回 PARSE FAILED 3001 | **不能** |
| 47 | 循环体含 `if` | `if i > 1 { break outer(i); }` | `"p/l3.lain" src/p_l3.lain` | `status=0` + **trap 1004**，136 字节 | **不能** |
| 48 | 循环体 = `let`（算子）+ `if`（bootstrap 骨架） | `let m: bool = i > 1; if m { break outer(i); }` | `"p/l5.lain" src/p_l5.lain` | `status=0` + **trap 1004**，136 字节 | **不能** |
| 49 | 循环体含 `continue LABEL(实参)` | `continue outer(i + 1);` | `"p/l2.lain" src/p_l2.lain` | `status=0` + **trap 1004**，136 字节 | **不能** |
| 50 | 循环体含 `store` | `store(buf + i, 1); break outer(i);` | `"p/l4.lain" src/p_l4.lain` | `host_status=4`，195 字节，装回 2006 | **不能** |
| 51 | 循环体给参数**赋值**（bootstrap 用 0 处） | `i = i - 1;` | `"p/li.lain" src/p_li.lain` | `status=0` + **trap 1004**，136 字节 | **不能** |
| 52 | **bootstrap 主力循环形态**（照抄 `std/modules.lain` 的 `meta_buf_puts`） | `loop put(k: u64 = 0, j: u64 = at) { let more: bool = k < n; if more { … continue put(k+1, j+1); } else { break put(j); } }` | `"p/lb2.lain" src/p_lb2.lain` | `status=0` + **trap 1004**，177 字节 | **不能** |
| 53 | 循环 + `while` 条件，体里给参数赋值（bootstrap 用 0 处） | `loop outer(i: i32 = n, acc: i32 = 0) while i > 0 { acc = acc + i; i = i - 1; }` | `"p/lw.lain" src/p_lw.lain` | `status=0` + **trap 1004**，161 字节 | **不能** |

**#46 的归因**（把「循环体里的算子」逐层剥开，5 组对照都实测过）：
- `1 + 2`（两侧字面量）→ 能；`i + 1`（左操作数是名字）→ trap；
- `i`（裸名字）→ 能；`helper(i)`（调用）→ 能；
- `break outer(x + 1)`（算子只在 break 实参里）→ 能。

所以触发条件是「**循环体里 `let` 的初值含算子，且操作数要靠名字查类型**」——
查类型那条路（`meta_loop_type_name` / `meta_local_type_name` 一族）把某个文本地址
取坏了。trap 固定落在 `build/lain0/bootstrap.l1` 第 790 行（`std_types__meta_bytes_equal`
循环体里的 `#eq[#bits<8>]`，两侧的 `#load` 读的是**节点文本地址**，`std/parse.lain` 的
`meta_node_is_word` 是这条路的调用者），`region=115 position=3` 每次都一样。
对照：同样含算子的 `let` 出现在**函数体**里（非循环体）完全正常（#22/#23/#26/#39）。

### 4.5 单元级现状（现有驱动上能跑的四个单元）

单元可跑性统计（闭包与登记数）：

| 单元 | 需要的登记条数 | 驱动区段数（3R+3） | 今天能跑 | 备注 |
| :--- | ---: | ---: | :--- | :--- |
| `std/lex` | 3 | 12 | 是 | 闭包 = lex + types + regions |
| `std/types` | 2 | 9 | 是 | types ↔ regions 互相 import，两个名字都要登记 |
| `std/regions` | 2 | 9 | 是 | 同上 |
| `std/registry` | 3 | 12 | 是 | |
| 其余 24 个 | 62 | 189 | **否** | `LAINVM_SPACE_MAX_REGIONS=64`（`seed/kernel/include/lain/vm/space.h:44`），驱动每份源码映射 3 个区段（text/path/ast_in）→ 最多 20 份 |

实测（登记名 = 源码里 spec 的规范化形态）：

| 单元 | 命令 | 实测 | 卡在哪 |
| :--- | :--- | :--- | :--- |
| `std/lex` | `"std/lex.lain" bootstrap/std/lex.lain "types.lain" bootstrap/std/types.lain "regions.lain" bootstrap/std/regions.lain` | `host_status=6`，**1803 字节**，装回 3001 | `std/lex.lain:54-55` 的链式比较（#35） |
| `std/types` | `"types.lain" bootstrap/std/types.lain "regions.lain" bootstrap/std/regions.lain` | `status=0` + **trap 1004**，279 字节 | 第 1 个函数 `meta_bytes_equal` 的循环体 `let more: bool = i < n;`（#46） |
| `std/regions` | `"regions.lain" bootstrap/std/regions.lain "types.lain" bootstrap/std/types.lain` | `host_status=9415`，263 字节 | `meta_region_scratch_window` 的 `symbol_addr("…_storage")`（#19） |
| `std/registry` | `"std/registry.lain" … "types.lain" … "regions.lain" …` | `host_status=4`，442 字节 | 跨模块函数成员调用（#10） |

四个单元的产物**全都装不回去**（3001：截断在未闭合的区域里）。

### 4.6 红样例全量（`gate8.sh`，38 个 examples）

38 个样例的全量实测：

非零/异常行（其余 26 个 `host_status=0` 且 `trap none`）：

| 样例 | rc | host_status | 产物 | trap | 说明 |
| :--- | :-: | :--- | ---: | :--- | :--- |
| `jumps` | 1 | 0 | 165 | `kind=1 status=1004 line=790 col=37 region=115` | 循环族 trap（#53 同源） |
| `loop` | 1 | 0 | 169 | 同上 | 同上 |
| `loop_if` | 1 | 0 | 164 | 同上 | 同上 |
| `loop_local` | 1 | 0 | 398 | 同上 | 同上 |
| `loop_value` | 1 | 0 | 160 | 同上 | 同上 |
| `modules_lib` | 0 | 4 | 45 | none | 跨模块函数成员（#10） |
| `modules_main` | 0 | **11** | **0** | none | 只登记了样例自己，`import("examples::modules_lib")` 找不到 → 11 |
| `modules_twin` | 0 | **11** | **0** | none | 同上 |
| `refs` | 0 | 5 | 0 | none | `ref`/`mut ref`/`slice` 形参（bootstrap 不用） |
| `strinline` | 0 | 4 | 231 | none | 函数体字符串字面量（#33） |
| `symbol_addr` | 0 | 9415 | 54 | none | `symbol_addr` 内建（#19） |
| `u8math` | 0 | 6 | 52 | none | u8 算子（#27） |

---

## 5. 容易混淆的边界

1. **`modules_main` / `modules_twin` 报 11**：样例里写 `import("examples::modules_lib")`，
   驱动只登记了样例自己，import 找不到源码 → 11 / 0 字节。把被导入模块也登记上，
   才会看到成员形态的 4 与悬空调用的 2015（§4.2 #9 #10 是缩小版）。
2. **`jumps` / `loop*` 的产物截断来自 trap**：`LAINVM_TRAP_LOAD_FAULT`（1004，落在
   `std_types__meta_bytes_equal`），写出与配额都正常，是 Meta 自己在循环体里越界读。
3. **`symbol_addr` 的失败码看位置**：在目标单元里（直接降级这份源码）报 9415；
   把它放进被导入模块、让导出路径去发那条绑定时报 13（0 字节）。13 出自
   `meta_emit_module_binding` 的「名字在模块表里找不到」，9415 出自
   `meta_scope_require_func` 的「被调名字不是函数」。
4. **跨单元调用 2482 处、跨模块取值 167 处**：按「`alias.name` 且 alias 由
   `let alias = import(…)` 绑定」统计全部出现位置。
5. **bootstrap 不使用的构造**：`struct` / `enum` 值层、`ref` / `mut ref` / `slice`、
   `for`、给局部变量赋值、本单元 `t[i]` 索引、函数体字符串字面量、聚合逃逸。
   修自举不需要碰它们；与自举相关的只有 `symbol_addr`、`u8` 算子、import 与成员访问。
6. **11 是双语义**：除了「import 找不到源码」，`funcs.lain` 的 4 处 11 是有意拒绝
   聚合逃逸（`meta_lower_value_stmt`）。看产物有没有内容就能分开。
7. **驱动区段上限是硬约束**：`LAINVM_SPACE_MAX_REGIONS=64` 在 `seed/kernel/` 里，
   每份源码占 3 个区段 → 一次最多登记 20 份源码。整份 bootstrap 的闭包有 28 个模块
   （62 条登记），路径解析修好后还要在驱动侧减区段或分组映射。

---

## 6. 推进路径（按「最快见效」排序）

每一步的验收都是 **「`host_status=0` 且 `./build/check_unit.exe <产物>` 输出
`PARSE+VERIFY OK`」**。只看 `host_status` 会把 trap 之后截断的产物当成通过（§4.4）。

### 第 1 步 · mangle 按模块键（Meta 侧）

- **改哪**：`bootstrap/std/modules.lain` 的 `meta_build_mangle`（410-441）。
  现在只把 `::` 折成 `__`、`/` **原样保留**，于是 spec `probe/pkg/a` 产出
  `#proc probe/pkg/a__av()` —— 名字里有 `/`，`lainir_parse` 直接 3001。
  目标：从**解析到的模块**（相对源根的路径，`/`→`_`）算 mangle，
  与 lain0 的 `Program::keyOf`（`seed/lain0/src/Sema.cpp:1047`）逐字一致：
  `std/types.lain` → `std_types__meta_bytes_equal`。
- **为什么在它前面**：它决定产物里**每一个**跨模块符号的名字；不先修，
  后面几步的产物都是非法的，验收判据无从谈起。`symbol_addr`（第 3 步）也要发
  `<键>__NAME_storage`，同一个键。
- **验收**：`g1_imp_ok` 那条命令的产物从
  `#proc probe/pkg/a__av()` 变成 `#proc probe_pkg_a__av()`（或 lain0 口径的键），
  且 `check_unit.exe` 从 `PARSE FAILED 3001` 变 `PARSE+VERIFY OK`。
- 附带：`meta_build_path` / `meta_source_find`（143-248）要能拿到「模块键」，
  这需要第 8 步的路径解析一起做（或先用「登记名 = 键」的临时约定）。

### 第 2 步 · 跨模块函数成员调用（Meta 侧）

- **改哪**：`bootstrap/std/modules.lain` 的 `meta_emit_ns_operand`（641-701，
  现在 `member.kind != 3` 就 `return 4`）与 `meta_emit_ns_call`（471），
  以及导出路径 `meta_emit_module_exports` / `meta_emit_module_binding`（321-370、529-575）：
  被导入模块的**函数**成员也要能被 `M.f(x)` 调到。lain0 的做法是发
  `<键>__f`，Meta 侧最省事的是**不转发**：`M.f(x)` 直接发
  `#call <键>__f(<实参>)`（lain0 就是这么发的，见 `docs/plan-modules.md` §3.2）。
- **为什么在它前面**：2482 处，28 个单元**每个**都有；`std/registry` 的第一个
  卡点就是它（442 字节处报 4）。
- **验收**：`g3_member_call` 与 `g10_registry` 两条命令：
  前者 `status` 从 4 变 0 且装回 OK；后者从 442 字节的 4 往后走（下一步是 9415 或 6）。

### 第 3 步 · `symbol_addr("…_storage")` 内建（Meta 侧）

- **改哪**：`bootstrap/std/lex.lain` 加关键字（照 `meta_kw_ptr_of` 的样子，
  22-27 行那批），`bootstrap/std/funcs.lain` 的 `meta_is_builtin_name`（1070-1077）
  加一条判断，`bootstrap/meta.lain` 的表达式发射器加分支：读字符串实参的字节当符号名，
  发 `#bitcast[#addr](%<键>__NAME_storage)`（复用 `meta_emit_symbol_addr_line`）。
- **为什么在这里**：328 处、12 个单元；`std/regions` 的第一个卡点（9415，263 字节处）；
  依赖第 1 步的键（符号名要带模块键）。
- **验收**：`g4_symaddr_body`（`status` 9415 → 0）与 `g10_regions`
  （9415 → 下一个卡点）；`std/regions` 端到端应能到达
  `status=0` + `PARSE+VERIFY OK`（它是 24 行、无循环，`std_types.meta_cell_get`
  由第 2 步解决）——**这就是「第一个端到端单元」**。

### 第 4 步 · 循环体 trap（Meta 侧，**杠杆最大**）

- **改哪**：`bootstrap/std/funcs.lain` 的 `meta_lower_loop_stmts`（1423-1530）
  的 `let` 分支 —— 它把 `lvi = si + 5` 交给 `meta_lower_stmt`（1902），
  后者走 `meta.meta_emit_expr` 的算子路。触发条件是「循环体里 `let` 初值含算子、
  操作数要靠名字查类型」，查类型那条路（`meta_loop_type_name` 2378 /
  `meta_local_type_name`）把节点文本地址取坏 → `std_types.meta_bytes_equal`
  越界读（`bootstrap.l1:790`）。
  排查手段：在这些调用点往 scratch 安全格写标记（照 `meta_source_find` 的
  2064 那套），跑 `g8_op_in_loop_let` 看落在哪一格。
- **排序理由**：它是**唯一一条 bug 形态**的缺口（其余都是「没实现」），19 个含
  `loop` 的单元几乎全中，修好一次解锁最多；它不挡 `std/regions`（无循环），所以排在
  「第一个端到端单元」的公共项之后。
- **验收**：`g8_op_in_loop_let`、`g7_body_if_break`、`g7_body_continue`、
  `g6_loop_bootstrap`、`g5_loop_assign_i` 五条从
  「trap 1004 + PARSE FAILED 3001」变 `status=0` + `PARSE+VERIFY OK`；
  `g10_types` 从 279 字节的 trap 往后走（下一个卡点是 u8 算子，第 5 步）；
  examples 的 `loop`/`loop_if`/`loop_value`/`loop_local`/`jumps` 5 个从 rc=1 变 rc=0。

### 第 5 步 · u8 算子表项（Meta 侧，纯数据）

- **改哪**：`bootstrap/std/scalars.lain` 的 `meta_scalar_table`（顶层 `ro [u8]`
  字节表，`scalars.lain:56`，587 字节）。表里现在有 `2 105 56 0 8 0`（`i8`、宽度 8、
  0 个算子）与 `2 117 56 0 8 0`（`u8`、宽度 8、0 个算子）。
  `meta_scalar_op`（154-185）是按表查的，所以给 `u8` 补上与 `u32` 同名的
  `== != < <= > >= + - * / % and or << >>`（物理名 `eq ne ult ule ugt uge add sub mul
  udiv urem and or shl lshr`）即可。**`std/types.lain:13` 的 `if ba == bb` 就是它。**
- **为什么在这里**：只有 `std/types` 的 `meta_bytes_equal` 真的需要
  （bootstrap 其余地方一律 `as u32`，98 处），但它是 `std/types` 的第一个
  语义卡点，而 `std/types` 是很多单元的直接依赖。
- **验收**：`g4_u8cmp` / `g4_u8arith` 从 6 变 0 + 装回 OK；
  `g10_types` 越过 279 字节继续（下一个卡点大概是 `ptr_of` 的 2005，第 7 步）。

### 第 6 步 · 链式比较（Meta 侧）

- **改哪**：`bootstrap/meta.lain` 的 `meta_emit_expr`（`meta.lain:1193`，607 行那个大函数）
  里算子折叠那一支：现在只把算子**右边**当下一个 token（名字/字面量）取，
  于是 `a < b and b < 10` 发成 `#and[#bits<32>](%r1, %b)` 然后 6。
  需要让右操作数**递归**降级成子表达式（`1 + 2` 能过是因为右操作数是字面量）。
- **为什么在这里**：只有 7 处（`std/lex:54-55`、`std/syntax:56/89/112/161/166`），
  但其中 2 处正好卡在 `std/lex`（`bs_is_ident`）—— 想要 `std/lex` 端到端就绕不过。
- **验收**：`g9_chain_cmp_and` / `_return` / `_three` 从 6 变 0 + 装回 OK；
  `g10_lex` 从 1803 字节的 6 往后走（下一个卡点是 `std_types.meta_bytes_equal`
  的跨模块调用，第 2 步）。

### 第 7 步 · `ptr_of` 产物（Meta 侧）

- **改哪**：`std/funcs.lain` 里 `ptr_of` 的发射 —— `meta_is_builtin_name`（1070-1077）
  的判定与 `meta_emit_builtin_temp`（2122 一族）的 `meta_kw_ptr_of` 分支：
  仍在发 D-002 已删的 `#ptr2int[#bits<W>]`，应为
  `#bitcast[#bits<W>](%p)`。同理 `addr_of` 的 `#int2ptr` → `#bitcast[#addr](%w)`。
- **为什么在这里**：只有 `std/types.lain` 用（2 处），但产物装不回去（2005）。
- **验收**：`g4_ptr_of` / `g6_ptr_of_addr_of` 的装回从 `VERIFY FAILED 2005` 变 OK。

### 第 8 步 · import 路径解析（Meta 侧 + 驱动侧）

- **改哪**：
  - `bootstrap/std/modules.lain` 的 `meta_source_find` / `meta_build_path`（143-248）：
    改成「相对**导入方**的目录解析」。导入方是谁需要记下来 ——
    `bootstrap/std/scope.lain` 的 `meta_scope_add`（445）现在往 import 条目 aux 里存
    「引号位置」，要再存「导入方源码下标」（或导入方路径），
    `meta_scope_scan_root`（661）的 import 分支（611-625）负责填。
  - 把 `meta_path_buffer()`（`scope.lain:174-182`）与语义旁表的边界理清：路径缓冲区
    的位置随作用域表增长移动，可能落在不可读的位置或被旁表覆盖。
  - **驱动侧**（`seed/meta_driver.c:176`）：登记名改成「相对源根的路径」
    一份模块一条；同时把「每份源码 3 个区段」压到 1-2 个
    （或按组映射），否则 28 个模块 84+ 个区段过不了 `LAINVM_SPACE_MAX_REGIONS=64`。
- **为什么排在后面**：它是**整份 28 模块一次跑通**的前提，但单单元端到端可以先用
  「登记名 = 源码里的 spec」绕过（本文所有单元级实测都是这么跑的）。
- **验收**：`g10_lex_sorder`（登记名 = SOURCE_ORDER 路径）从 11/0 字节
  变成与 `g10_lex` 相同的 status；单元闭包里「今天可跑」从 4 个变成 28 个
  （登记数 62 → 28，区段数 189 → ≤87）。

### 第 9 步 · 全量判据与收尾

- 把「产出装得回去」做成闸门：`scripts/` 下加一个脚本，对每个单元
  `lain-meta --out` + `check_unit`，非零即红。当前基线是 **0/28**（四个能跑的单元
  全都装不回去）。
- 目标顺序建议：`std/regions` → `std/types` → `std/lex` → `std/registry` →
  其余按 `SOURCE_ORDER` 依赖顺序（`std/modules`、`std/scope` 在最后，
  它们自己就是 import 层的实现）。

### 归属汇总

| 步 | Meta 侧（`bootstrap/std/**`） | 驱动侧（`seed/meta_driver.c`） | lain0 侧（`seed/lain0/**`） |
| :- | :--- | :--- | :--- |
| 1 mangle | `std/modules.lain`: `meta_build_mangle` / `meta_source_find` / `meta_build_path` | 登记名口径 | 无（lain0 已是目标语义，`Sema.cpp:1047` 是参照） |
| 2 函数成员 | `std/modules.lain`: `meta_emit_ns_operand` / `meta_emit_ns_call` / `meta_emit_module_exports` | — | 无 |
| 3 symbol_addr | `std/lex.lain`(关键字) + `std/funcs.lain`(`meta_is_builtin_name`) + `meta.lain`(发射) | — | 无 |
| 4 循环体 trap | `std/funcs.lain`: `meta_lower_loop_stmts` / `meta_lower_loop_if` / `meta_lower_loop_jump`；`std/parse.lain`: `meta_node_is_word` 的调用点 | — | 无（`bootstrap.l1:790` 只是症状） |
| 5 u8 算子 | `std/scalars.lain`: `meta_scalar_table` 字节表 | — | 无 |
| 6 链式比较 | `meta.lain`: `meta_emit_expr` 的算子折叠 | — | 无 |
| 7 ptr_of | `std/funcs.lain`: `ptr_of` / `addr_of` 发射支 | — | 无 |
| 8 import 解析 | `std/modules.lain` + `std/scope.lain`(`meta_scope_add:445`/`meta_scope_scan_root:661`) | 登记与区段预算 | 无 |

**没有一步需要改 `seed/lain0/**` 或 `seed/kernel/**`**：lain0 的模块语义就是目标语义，
kernel 的区段上限是驱动侧要绕的约束。这也是本任务「只调查」的一个副产品结论 ——
自举的缺口全在 Meta 自己（`bootstrap/std/**`）加一处驱动登记。

---

## 7. 复现

样例级状态可以这样重跑：

```bash
cmake --build build
python scripts/build_meta_unit.py
for f in bootstrap/lain/examples/*.lain; do
  n=$(basename "$f" .lain)
  ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt "$n.lain" "$f" > "build/meta-$n.log" 2>&1
  grep -E 'host_status|trap kind|output bytes' "build/meta-$n.log"
done
```

判据：`host_status=0`、没有 `trap kind` 行、产物过 `lainir_parse` + `lainir_verify`。

§3–§5 的差集数字来自探针矩阵，每个探针是一份最小源码。探针源码与脚本放在未跟踪的
`build/probe/` 下，已随 build 目录清理；每条探针的登记名、最小源码与实测结果写在上面的
表里，可以按原样重建。
