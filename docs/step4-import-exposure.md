# 第 4 步前置实测：模块数据成员的地址算术（点 A）与 import 导出覆盖面（点 B）

对象：分支 `master`，起点 `75ac183`（计划的第 1/2/3 步已落地）。
范围：`seed/lain0/` 的代码改动（点 A）+ `bootstrap/` 的只读统计（点 B）。
本文件不改 `docs/plan-modules.md`，只记录实测结论。

## 0. 结论

1. **点 A 已修**（`seed/lain0/` 三处改动，见 §1）。`M.NAME`、`M.NAME + i`、`M.NAME[i]`、
   `store(M.NAME + i, v)` 四种形态都编译并过 verify，`bootstrap/std/funcs.lain:1056` 那个
   `symbol_addr("X_storage") + 1` 的模块版（`M.X + 1`）同样成立。回归 `PASS=38 FAIL=0`
   （`--round-trip` 同），Meta 管线 `host_status=0` / `region grants=2` / `output bytes=74`
   且与 `build/arith.out` 逐字节相同。
2. **点 B 的原始担心在 bootstrap/ 里不成立**：`bootstrap/` 28 个单元共 216 条顶层静态
   `let`，**全部**是 `[...]` 定长表初值 —— 一条「调用式初始化」都没有。而定长表初值在
   Meta 的导出路径上被**整条跳过**（不是报 13，见 §2.1）。逐单元实测 import：27 个单元
   `host_status=0` 跑到底，`std/funcs.lain` 在驱动默认燃料（4e9 步）内 `RUN INCOMPLETE`
   （是解析/扫描开销，不是形态被拒）。
   → 会触发 13 的形态 **0 条**，会触发 4 的形态 **0 条**，因「被导入单元里有不支持的静态
   let」而受影响的单元对 **0 对**。
3. **第 4 步可以直接开干**（用 `lain0 --unit`）：lain0 的 import 对初值形态没有限制 ——
   实测 8 种初值形态（数字 / 同模块标识符 / u8 表 / u64 表 / ro 表 / `symbol_addr` 调用 /
   顶层聚合构造 / 普通函数调用）全部编译 + verify 通过，导入方取用每种成员也都通过。
4. 但有两件**先决条件**必须落在第 4 步之前，否则第 4 步自己的自查闸门（「每改完一个单元
   用 `lain0 --unit` 编译整份清单」）跑不起来：
   - `bootstrap/SOURCE_ORDER` 还是旧的 `.l1` 清单，`lain0 --unit bootstrap/SOURCE_ORDER`
     现在报 `<unknown>: error: 打不开源文件: bootstrap/std/lex.l1.lain (600)`（清单行会被
     直接当成路径、再补 `.lain`）。必须换成模块清单（`std/lex` 这种不带后缀的路径）。
   - `scripts/build_meta_unit.py` 现在把 28 份源码**拼成一份单文件**（`assemble()`）再编。
     加了 `import` 之后，拼出来的文本里 `import("std::scope")` 会相对 `build/lain0/` 解析，
     找不到文件 → 这条路会断；必须换成计划 §3.7 的 `lain0 --unit <清单>`。
5. Meta 侧的 import 实现（`bootstrap/std/modules.lain`）是 **v0 窄口径**，缺口不在
   「初值形态」而在别处，且**不在第 4 步的链路上**（第 4 步用 lain0 编译，Meta 本体由
   lain0 从改写后的源码重建）。详见 §2.5：跨模块**函数**调用报 4；跨模块**数据**成员被取用
   会发出悬空 `#call`，产物 verify 2015。

## 1. 点 A：模块数据成员的地址算术

### 1.1 根因

`seed/lain0/src/Sema.cpp` 的 `Walker::registerImports()` 把被导入单元的静态 let 登记成
模块成员时，成员的值类型直接抄了**声明类型**（`topLevelTypes[q]`，例如 `[u8]`）：

```cpp
mm.type = t->second;          // 声明类型
```

而 `M.NAME` 的降级结果是**对访问器过程的调用**（§3.6），访问器返回什么取决于初值形态：

| 初值形态 | 发出的东西 | 访问器返回 |
| --- | --- | --- |
| 字节串 / 定长表 / 顶层聚合构造 | `data <键>__NAME_storage` + `#proc <键>__NAME() -> #addr` | `#addr` |
| 其余（数字 / 标识符 / 调用 …） | `#proc <键>__NAME() -> <声明类型>` | 声明类型 |

所以表成员的 Sema 类型是 `[u8]`（数组），而实际拿到的值是地址：
`M.table + i` 走 `lowerBinary` → `selectBinary(Add, [u8])` → `[u8] 上没有这个算子 (303)`。
同模块之所以能用，是因为 `registerTopLevel()` 对所有静态 let 一律 `bind(name, addr)`
（`seed/lain0/src/Sema.cpp:594`）。

### 1.2 改动（3 处，均在 `seed/lain0/`）

| 文件 | 改动 |
| --- | --- |
| `include/lain0/Sema.hpp` | `ModuleMember` 的 `type` 语义改为「访问器真正返回的类型」并加注释；新增 `declType`（静态 let 的声明类型，供 `M.NAME[i]` 取元素步长） |
| `src/Sema.cpp` `registerImports()` | `mm.type = topLevelData.count(q) ? builtin("addr") : t->second`；`mm.declType = t->second` |
| `src/LowerExpr.cpp` `lowerIndexExpr()` | `M.NAME[i]` 的步长：base 是 Dotted 形态的模块成员时，从 `ModuleMember::declType` 取元素宽度（此前只有 `NameExpr` 那条路取步长，跨模块索引一律按 1 字节算） |

第三处是顺带的正确性修复：`M.NAME[i]` 在 `[u8]` 表上「碰巧对」，在 `[u64]` 表上原来会按
1 字节步长算（错值）。现在 `P.words[i]` 发出 `#lea[#addr](%r, %i, 8, 0)`。

### 1.3 四种形态的真实产物

探针 `build/probe/gateA.lain`（`build/` 下，不提交；被导入的 `examples::modules_lib` 是
`bootstrap/lain/examples/modules_lib.lain` 的副本，`probe_lib.lain` 模拟 bootstrap 单元的
顶层数据块）：

```
$ ./build/lain0/lain0.exe build/probe/gateA.lain     # 退出码 0，IR 在 verify 通过后才打印
#proc gateA__idx(%i: #bits<64>) -> #addr {            // (1) M.table[i]
  %r1 = #call @examples_modules_lib__table()
  %r2 = #lea[#addr](%r1, %i, 1, 0)
  #return %r2
}
#proc gateA__arith(%i: #bits<64>) -> #addr {          // (2) M.table + i
  %r3 = #call @examples_modules_lib__table()
  %r4 = #lea(%r3, %i, 1, 0)
  #return %r4
}
#proc gateA__poke(%i: #bits<64>, %v: #bits<8>) -> #bits<8> {   // (3) store(M.table + i, v)
  %r5 = #call @examples_modules_lib__table()
  %r6 = #lea(%r5, %i, 1, 0)
  #store[#bits<8>](%v, %r6)
  %r7 = #call @examples_modules_lib__table()
  %r8 = #lea(%r7, %i, 1, 0)
  %r9 = #load[#bits<8>](%r8)
  #return %r9
}
#proc gateA__np1() -> #addr {                          // (4) funcs.lain:1056 的模块版
  %r10 = #call @probe_lib__text_buffer()
  %r11 = #lea(%r10, 1, 1, 0)                           // let np1: addr = M.X + 1;
  #return %r11
}
#proc gateA__wide(%i: #bits<64>) -> #addr {            // 附：跨模块 u64 表的步长
  %r12 = #call @probe_lib__words()
  %r13 = #lea[#addr](%r12, %i, 8, 0)
  #return %r13
}
```

`bootstrap/std/funcs.lain:1056` 原文（同模块形态）与模块版的对应关系：

```
let np1: addr = symbol_addr("lainir_text_buffer_storage") + 1;   // 原形（同单元）
let np1v: addr = P.text_buffer + 1;                              // 模块版（探针 (4)）
```

### 1.4 闸门（全部真实跑到）

| 闸门 | 命令 | 结果 |
| --- | --- | --- |
| 1 构建 | `cmake --build build` | `[23/23] Linking CXX executable lain0\lain0.exe`，`BUILD_EXIT=0` |
| 2 回归 | `bash scripts/regress.sh` | `PASS=38 FAIL=0` |
| 2 回归 | `bash scripts/regress.sh --round-trip` | `PASS=38 FAIL=0` |
| 3 形态 | `./build/lain0/lain0.exe build/probe/gateA.lain` | 退出码 0（见 §1.3） |
| 3 形态 | `./build/lain0/lain0.exe build/probe/shapes/main.lain`（8 种初值形态 + 取用） | 退出码 0；`--round-trip` 也 0 |
| 4 Meta | `./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --out build/probe/arith_gate4.out examples/arith.lain bootstrap/lain/examples/arith.lain` | `verify subroutines=870 data=216`、`image ok`、`host_status=0`、`region grants=2`、`output bytes=74`；`cmp build/probe/arith_gate4.out build/arith.out` → 逐字节相同 |

（闸门 4 的 `--out` 每次用全新路径，避免陈旧 `.out` 造成假通过。）

## 2. 点 B：Meta 的 import 导出覆盖面

### 2.1 机制（代码级，`bootstrap/std/modules.lain`）

`let M = import("…")` 的处理链：

```
meta_lower_binding → meta_lower_import → meta_emit_module_exports → meta_emit_module_binding
```

- `meta_emit_module_exports` 在 **import 声明处**遍历作用域表里**属于该模块的全部
  `kind == 3`（值绑定）条目**，逐条调用 `meta_emit_module_binding` —— 与它们有没有被
  引用**无关**。（`std/scope.lain:623` 把 import 绑定登记为 `kind == 1`，不在其列。）
- `meta_emit_module_binding` 只看初值的**头节点**（`meta_node_start/len(init_node)`）：
  - 头是数字 → 发一个字面量；
  - 否则把**头文本**当标识符去当前模块的作用域表里查：查到 `kind == 3` → 递归发那条 +
    发 `#call` 转接；查到别的 kind → `lain_meta_fail(4)`；查不到 → `lain_meta_fail(13)`。
- 表初值走不到这里：`std/scope.lain` 的 let 处理器在登记表绑定后立刻
  `meta_scope_set_emitted(last, 每元素字节数)`，而 `meta_emit_module_binding` 开头就是
  `if (emitted != 0) return 0;` —— **整条跳过**，不报错也不发过程。

### 2.2 边界矩阵（实测，`build/probe/meta_matrix.py`）

每个模块只放一条静态 let，探针只声明 `import`、不取成员，所以输出里出现的就是导出路径
为那条绑定发出的东西（`build/probe/meta/matrix.tsv`）：

| 初值形态 | 模块源码 | 实测 status | 导出产物 |
| --- | --- | --- | --- |
| 数字 | `let a: u64 = 1;` | 0 | `#proc …__a() -> #bits<64> { #return 1 }` |
| 同模块标识符 | `let a: u64 = 1; let b: u64 = a;` | 0 | `…__a` 的字面量 + `#proc …__b() { #return #call …__a() }` |
| 定长表 `[u8]` | `let t: [u8] = [1, 2, 3];` | 0 | **不发**（只有探针自己的过程） |
| 定长表 `[u64]` | `let t: [u64] = [1, 2];` | 0 | **不发** |
| `ro` 表 | `let t: ro [u8] = [1, 2];` | 0 | **不发** |
| `symbol_addr` 调用 | `let a: addr = symbol_addr("t_storage");` | **13** | 空 |
| 普通函数调用 | `func g…; let a: u64 = g(1);` | **4** | 空 |
| 运算符（头是字面量） | `let a: u64 = 0 - 1;` | 0 | **静默错值**：`#return 0` |
| 负数字面量 | `let a: u64 = -1;` | **13** | 空 |
| 字符串 | `let s: ro [u8] = "abc";` | **13** | 空 |
| 顶层聚合构造 | `struct P { x: i32 } let p: P = P { x: 1 };` | **4** | 空 |
| 转换式 | `let c: u64 = symbol_addr("a_storage") as u64;` | **13** | 空 |

两张必须记住的边界：

- 触发 13 的不是「调用式」这一类，而是**头节点解析不到作用域条目**（内建名 `symbol_addr`、
  未知名、`-1`、`"…"` 都算）；头节点是**函数名**或**类型名**时报 4。
- 头是数字但初值是表达式时（`0 - 1`）**不报错，静默发头那个字面量**。

### 2.3 bootstrap/ 的实测

`python scripts/import_export_scan.py`（源码形态分类）+ `build/probe/meta_sweep.py`
（拿 Meta 本体当预言机，逐单元 import）：

```
单元数 28，顶层静态 let 216 条
头形态统计: table=216
→ 会触发 13 的 0 条；会触发 4 的 0 条；定长表（导出路径跳过）216 条
```

| 单元 | 顶层 let | 定长表 | 触发 4 | 触发 13 |
| --- | --- | --- | --- | --- |
| `std/lex.lain` | 27 | 27 | 0 | 0 |
| `std/emit.lain` | 29 | 29 | 0 | 0 |
| `std/regions.lain` | 1 | 1 | 0 | 0 |
| `std/registry.lain` | 3 | 3 | 0 | 0 |
| `std/scope.lain` | 1 | 1 | 0 | 0 |
| `std/macros/my_if.lain` | 2 | 2 | 0 | 0 |
| `std/macros/my_block.lain` | 1 | 1 | 0 | 0 |
| `std/macros/my_require.lain` | 1 | 1 | 0 | 0 |
| `std/syntax.lain` | 1 | 1 | 0 | 0 |
| `std/expand.lain` | 4 | 4 | 0 | 0 |
| `std/modules.lain` | 2 | 2 | 0 | 0 |
| `std/eval.lain` | 2 | 2 | 0 | 0 |
| `std/scalars.lain` | 1 | 1 | 0 | 0 |
| `std/lainir.lain` | 141 | 141 | 0 | 0 |
| 其余 14 个单元 | 0 | 0 | 0 | 0 |

216 条的名字与行号见附录 A。

逐单元 import 实测（`build/probe/meta/sweep.tsv`，28 行）：

```
std/lex.lain … std/records.lain   host_status=0   output bytes=43   (25 个单元)
std/sums.lain / std/lainir.lain / meta.lain       host_status=0   output bytes=43
std/funcs.lain                    host_status=0   但 RUN INCOMPLETE 燃料耗尽 fuel=4e9
```

`output bytes=43` 正好是探针自己那个函数 —— 印证了「被导入模块的 216 条表绑定一条也没
发出来」。`std/funcs.lain`（2108 行）在默认燃料 4e9 步内没跑完（`RUN INCOMPLETE`），
是解析/扫描开销（`std/lex.lain` 27 行就已经要 4e7 步），不是形态被拒；把燃料加到 20e9
后 40 分钟仍未结束（本次终止，未取到结果）。

### 2.4 受影响单元对

分两个口径：

**（a）第 4 步（lain0 `--unit`）：0 对。** lain0 的成员登记不看初值形态（`topLevelTypes` /
`topLevelSymbols` 直接抄被导入单元的 Sema 结果），`build/probe/shapes/` 的 8 种形态矩阵
全部通过。

**（b）Meta 编译带 import 的源码：全部 180 条依赖边都受影响**，但原因不是点 B 说的
「调用式初始化」：

- 跨模块**函数**调用（652 个调用点 / 168 对）→ `meta_emit_ns_operand` 要求成员
  `kind == 3`，函数成员不是 → **status 4**。实测：`modules_twin`（`A.dup(1)`）跑出
  `host_status=4`，产物停在 `#proc total() -> #bits<64> {`。
- 跨模块**数据**成员取值（167 处取址 / 28 对，第 4 步会改写成 `M.NAME`）→ 取用路径发出
  `#call <mangle>__NAME()`，而导出路径从没发过这个 `#proc` → 产物**悬空调用**。实测：

```
$ ./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --out build/probe/meta/use.out \
    probe/imp_use.lain build/probe/meta/imp_use.lain std/lex.lain bootstrap/std/lex.lain
host_status=0     output bytes=79
$ cat build/probe/meta/use.out
#proc f() -> #bits<64> {
  %r1 = #call std__lex__meta_kw_let()
  #return %r1
}
$ printf '../build/probe/meta/use.out\n' > build/probe/meta/use_unit.txt
$ ./build/lain-meta.exe --unit build/probe/meta/use_unit.txt … 
VERIFY FAILED code=2015 line=2          # unknown callee
```

28 对跨单元取址（`symbol_addr("X_storage")` → 第 4 步的 `M.X`）：

| 导入方 → 被导入单元 | 跨单元取址 | 涉及的数据名（节选） | 跨单元调用点 |
| --- | --- | --- | --- |
| std/funcs.lain → std/lex.lain | 39 | meta_kw_acc, meta_kw_addr_of, meta_kw_as, meta_kw_break … | 18 |
| meta.lain → std/lainir.lain | 21 | lainir_assign, lainir_bitcast_addr_head, lainir_indent … | 101 |
| std/funcs.lain → std/emit.lain | 20 | meta_fn_open, meta_fn_pct, meta_hash_if … | 6 |
| std/funcs.lain → std/lainir.lain | 20 | lainir_colon_text, lainir_comma, lainir_text_buffer, lainir_zero | 149 |
| std/records.lain → std/lainir.lain | 11 | lainir_base_name, lainir_name_b, lainir_name_b2 … | 40 |
| meta.lain → std/emit.lain | 8 | meta_dsep, meta_foff, meta_rsize, meta_sym_add … | 7 |
| std/sums.lain → std/lainir.lain | 7 | lainir_eq_name, lainir_text_buffer, lainir_zero | 60 |
| std/modules.lain → std/lainir.lain | 5 | lainir_text_buffer, lainir_zero | 30 |
| std/modules.lain → std/emit.lain | 4 | meta_dsep | 0 |
| meta.lain → std/lex.lain | 3 | meta_kw_as, meta_kw_if, meta_kw_ro | 73 |
| std/handlers/statements.lain → std/lex.lain | 3 | meta_kw_for, meta_kw_loop, meta_kw_return | 0 |
| std/modules.lain → std/registry.lain | 3 | meta_path_cap, meta_reg_count_off | 0 |
| std/sums.lain → std/emit.lain | 3 | meta_dsep, meta_eoff, meta_etag | 5 |
| std/handlers/func.lain → std/lex.lain | 2 | meta_kw_extern, meta_kw_func | 0 |
| std/handlers/if.lain → std/lex.lain | 2 | meta_kw_else, meta_kw_if | 0 |
| std/handlers/let.lain → std/lex.lain | 2 | meta_kw_let, meta_kw_ro | 11 |
| std/scalars.lain → std/emit.lain | 2 | meta_sym_i32, meta_sym_i64 | 0 |
| std/scope.lain → std/registry.lain | 2 | meta_path_cap, meta_reg_base_state | 0 |
| meta.lain → std/modules.lain | 1 | meta_kw_import | 5 |
| std/handlers/enum.lain → std/emit.lain | 1 | meta_kw_enum | 0 |
| std/handlers/func.lain → std/expand.lain | 1 | meta_expand_decl_phase | 4 |
| std/handlers/struct.lain → std/emit.lain | 1 | meta_kw_struct | 0 |
| std/lainir.lain → std/scalars.lain | 1 | meta_scalar_table | 0 |
| std/macros/my_if.lain → std/lex.lain | 1 | meta_kw_else | 0 |
| std/modules.lain → std/scalars.lain | 1 | meta_scalar_table | 0 |
| std/scope.lain → std/expand.lain | 1 | meta_expand_decl_phase | 3 |
| std/scope.lain → std/lex.lain | 1 | meta_kw_eq | 7 |
| std/scope.lain → std/modules.lain | 1 | meta_kw_import | 1 |

（28 对 / 167 处，与计划 §5 的数字一致。`scripts/import_export_scan.py` 默认输出的依赖边
是 187，多出的 7 条来自「裸标识符引用」这个更宽的匹配，第 4 步写 import 时以 180 为准。）

### 2.5 建议与代价

| 方案 | 内容 | 代价 / 判断 |
| --- | --- | --- |
| **A（建议）** | 第 4 步只走 lain0：不动 Meta 的 import 实现。把「Meta 能编译带 import 的源码」当成第 5 步之后的独立工作项。 | 代价 0。风险：`modules_lib` / `modules_main` / `modules_twin` 样例继续红（计划 §6 闸门 8 已把它们的红色状态列为「改造前相同」），Meta 侧缺口不阻塞第 4 步。 |
| **B（Meta 侧补齐，若要让 Meta 编译 bootstrap）** | ① 导出路径真正发**数据块 + 访问器**：表绑定现在被 `meta_scope_set_emitted` 预置成「已发」而整条跳过，需要一个「按被导入模块发 data 块」的路径（`lainir_data_*` 写入器已存在，数据块只能顶层发，位置在 import 处）；② `meta_emit_ns_operand` 接受**函数**成员，发 `#call <mangle>__<f>(args)`（现在直接 4）；③ 统一 mangle：Meta 现在把每个 `/` 变成一个 `__`（`std/lex.lain` → `std__lex__NAME`），lain0 用一个 `_`（→ `std_lex__NAME`，计划 §3.2），两者必须一致才能互链。 | 改动落在 `bootstrap/std/{modules,scope}.lain` 与 `std/handlers/let.lain`，且要重跑 `scripts/check_meta_output.py` 全样例（状态码不等于产物可加载）。是独立增量，**不是**第 4 步的前置。 |
| **C（把 bootstrap 的静态 let 改写成受支持形态）** | 216 条全是字节表，内容必须逐字节保留（`data … { … }`）。改写成「数字 / 同模块标识符」等于重写数据表示。 | **不可行**，代价远大于 B。 |
| D（记账项） | `0 - 1` 这类「头是字面量的表达式初值」在 Meta 里静默发成头字面量。bootstrap 现在没有这种初值；第 4 步改写时**不要**引入。 | 只要不引入就不触发。 |

## 3. 第 4 步能不能直接开干

能，但顺序上先做两件事（§0.4）：把 `bootstrap/SOURCE_ORDER` 换成模块清单、把
`scripts/build_meta_unit.py` 从「拼源码」换成 `lain0 --unit <清单>`。之后：

- 逐单元改写的自查闸门 `lain0 --unit <清单>` 可用，且 lain0 对 216 条表绑定 + 各种初值
  形态都有覆盖（§1.3 / §2.4a）。
- 167 处跨单元 `symbol_addr` → `M.NAME`、652 处跨单元调用 → `M.f(…)` 都在 lain0 的能力
  范围内（地址算术与索引形态见 §1.3，成员调用见样例 `modules_lib`/`modules_main`）。
- Meta 侧的 v0 缺口（§2.5 B）不挡第 4 步；但要清楚：**第 4 步之后，Meta 仍然编译不了
  带 import 的源码**，所以「用 Meta 编译 bootstrap」这个目标要单独立项。

## 附录 A：216 条顶层静态 let（名字@行号）

- `std/lex.lain`（27 条）：
  meta_kw_let@1, meta_kw_eq@2, meta_kw_colon@3, meta_kw_func@4, meta_kw_return@5, meta_kw_semi@6
  meta_kw_if@7, meta_kw_else@8, meta_kw_and@9, meta_kw_or@10, meta_kw_as@11, meta_kw_for@12
  meta_kw_in@13, meta_kw_loop@14, meta_kw_while@15, meta_kw_break@16, meta_kw_continue@17, meta_kw_acc@18
  meta_kw_load@19, meta_kw_store@20, meta_kw_lea@21, meta_kw_len@22, meta_kw_ro@23, meta_kw_extern@24
  meta_bare_sigil@25, meta_kw_ptr_of@26, meta_kw_addr_of@27
- `std/emit.lain`（29 条）：
  meta_kw_struct@34, meta_dsep@35, meta_foff@36, meta_rsize@37, meta_fn_pct@38, meta_st_pct@39
  meta_st_eqop@40, meta_st_nl@41, meta_sym_plus@42, meta_sym_zext@43, meta_sym_trunc@44, meta_sym_add@45
  meta_sym_sub@46, meta_fn_open@47, meta_st_ind@48, meta_st_if@49, meta_hash_if@50, meta_st_arrow_lp@51
  meta_st_yield@52, meta_st_else@53, meta_st_lhead@54, meta_st_lend@55, meta_sym_lt@56, meta_sym_i32@57
  meta_sym_i64@58, meta_sym_dotdot@59, meta_kw_enum@60, meta_etag@61, meta_eoff@62
- `std/regions.lain`（1 条）：meta_region_scratch_record@3
- `std/registry.lain`（3 条）：meta_reg_count_off@9, meta_reg_base_state@10, meta_path_cap@11
- `std/scope.lain`（1 条）：meta_kw_colon_b@46
- `std/macros/my_if.lain`（2 条）：meta_kw_my_if@4, meta_kw_generated_if@5
- `std/macros/my_block.lain`（1 条）：meta_kw_my_block@4
- `std/macros/my_require.lain`（1 条）：meta_kw_my_require@3
- `std/syntax.lain`（1 条）：meta_syntax_state@2
- `std/expand.lain`（4 条）：meta_expand_defer_state@15, meta_expand_decl_phase@16, meta_expand_decl_state@17, meta_expand_id_state@18
- `std/modules.lain`（2 条）：meta_kw_import@53, meta_suffix_lain@54
- `std/eval.lain`（2 条）：meta_eval_align_module@28, meta_eval_align_entry@29
- `std/scalars.lain`（1 条）：meta_scalar_table@48
- `std/lainir.lain`（141 条）：
  lainir_proc@11, lainir_open@12, lainir_bits32@13, lainir_body@14, lainir_return_text@15, lainir_newline@16
  lainir_close@17, lainir_assign@18, lainir_bits_open@19, lainir_close_type@20, lainir_comma@21, lainir_rparen_nl@22
  lainir_rparen_nl_close@23, lainir_pct_r@24, lainir_indent@25, lainir_hash@26, lainir_addr_open@27, lainir_add_name@28
  lainir_sub_name@29, lainir_mul_name@30, lainir_sdiv_name@31, lainir_udiv_name@32, lainir_srem_name@33, lainir_urem_name@34
  lainir_and_name@35, lainir_or_name@36, lainir_xor_name@37, lainir_shl_name@38, lainir_lshr_name@39, lainir_ashr_name@40
  lainir_eq_name@41, lainir_ne_name@42, lainir_slt_name@43, lainir_sle_name@44, lainir_sgt_name@45, lainir_sge_name@46
  lainir_ult_name@47, lainir_ule_name@48, lainir_ugt_name@49, lainir_uge_name@50, lainir_zext_name@51, lainir_sext_name@52
  lainir_trunc_name@53, lainir_bitcast_name@54, lainir_ptr2int_name@55, lainir_int2ptr_name@56, lainir_paren_open@57, lainir_typed_close@58
  lainir_load_name@59, lainir_store_name@60, lainir_lea_name@61, lainir_alloca_name@62, lainir_data_addr_name@63, lainir_bitcast_addr_head@66
  lainir_proc_addr_name@67, lainir_call_space@68, lainir_lparen@69, lainir_data_name@70, lainir_data_mode@71, lainir_call_name@72
  lainir_call_assign@73, lainir_call_indirect_word@74, lainir_args_open@75, lainir_space@76, lainir_if_name@77, lainir_loop_name@78
  lainir_yield_name@79, lainir_break_name@80, lainir_continue_name@81, lainir_switch_name@82, lainir_default_line@83, lainir_arrow_open@84
  lainir_close_rparen@85, lainir_brace_nl@86, lainir_else_line@87, lainir_close_line@88, lainir_case_prefix@89, lainir_extern_marker@90
  lainir_quote_nl@91, lainir_close_bracket_space@92, lainir_result_sep@93, lainir_store_open@94, lainir_after_type@95, lainir_base_name@96
  lainir_text_buffer@97, lainir_lea_text_open@98, lainir_lea_tail_text@99, lainir_t_bits@100, lainir_t_f@101, lainir_t_vec@102
  lainir_t_addr@103, lainir_t_gt@104, lainir_scratch_byte@105, lainir_op_bracket@106, lainir_op_close_text@107, lainir_c3@108
  lainir_ret_call_text@109, lainir_rret@110, lainir_colon_text@111, lainir_close_lines@112, lainir_storage_text@113, lainir_base_assign@114
  lainir_data_ro_head_text@115, lainir_proc_close_open@116, lainir_addr_body@117, lainir_return_base_close_text@118, lainir_data_rw_head_text@119, lainir_data_rw_bare_head_text@120
  lainir_name_b@121, lainir_name_b2@122, lainir_name_v@123, lainir_indent4@124, lainir_if_head@125, lainir_continue_head4@126
  lainir_break_head4@127, lainir_close4@128, lainir_pct_only@129, lainir_loop_head@130, lainir_colon_only@131, lainir_result_head@132
  lainir_loop_body_open@133, lainir_if_head2@134, lainir_arrow_lp@135, lainir_yield_head@136, lainir_else_head@137, lainir_bracket_paren@138
  lainir_st_pct3@139, lainir_store_head@140, lainir_proc_word@141, lainir_empty_params@142, lainir_ret_call_head@143, lainir_body_return@144
  lainir_call_word@145, lainir_lea_head@146, lainir_zero@147, lainir_bind_head@148, lainir_vproj_load@149, lainir_vproj_if@150
  lainir_vproj_lea@151, lainir_vproj_value@152, lainir_vproj_tail@153

## 附录 B：复现命令

```
cmake --build build
bash scripts/regress.sh ; bash scripts/regress.sh --round-trip

# 点 A：四种形态 + funcs.lain:1056 的模块版
./build/lain0/lain0.exe build/probe/gateA.lain
./build/lain0/lain0.exe --round-trip build/probe/shapes/main.lain

# 闸门 4（每次用全新 --out）
rm -f build/probe/arith_gate4.out
./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt --out build/probe/arith_gate4.out \
    examples/arith.lain bootstrap/lain/examples/arith.lain
cmp build/probe/arith_gate4.out build/arith.out

# 点 B
python scripts/import_export_scan.py            # 源码形态分类 + 单元表
python build/probe/meta_matrix.py               # 边界矩阵（每个形态一条 let）
python build/probe/meta_sweep.py                # 28 个单元逐个试 import（约 30 分钟）
```

`build/probe/*` 下的探针与脚本都放在 `build/`（Git 忽略），不随本文件提交；
`scripts/import_export_scan.py` 是第 4 步的记账工具（与 `namespace_audit.py` 一样，
第 5 步可以按计划删掉）。
