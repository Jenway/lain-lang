# 计划：lain0 的模块命名空间、导出与 bootstrap 改用 import

本计划面向 HEAD `e717e9b`（分支 `master`）。全部改动落在 `seed/lain0/`、`bootstrap/`、`scripts/`、`docs/`；`seed/kernel/` 不改。

## 一、现状

lain0 已经能解析 `let M = import("…");`，但它只做三件事：把别名登记成一个类型、把被导入模块的 StaticLet 名字抄进导入方的 `topLevelTypes`、把被导入模块的 StaticLet/Struct/Enum 降级进导入方自己的 `data_` 与 `subs_`。于是：

| 位置 | 现状 | 后果 |
| --- | --- | --- |
| `seed/lain0/src/LowerExpr.cpp:702` | `lowerCall` 只接受 `ExprKind::Name` 的被调者，否则 `D::LowerUnsupported`「只支持直接调用具名过程」 | `M.bump(x)` 编译失败 |
| `seed/lain0/src/Sema.cpp:542` | `ModuleMember{Kind::Value, mangled = kv.first}`，`mangled` 是裸成员名 | `M.answer` 发出 `#call @answer()`；两个模块各有一个 `answer` 就撞上 verify 的 `duplicate subroutine`（code 2022） |
| `seed/lain0/src/LowerDecl.cpp:345` | `lowerImport` 把被导入文件的 StaticLet/Struct/Enum 降级进当前模块 | 跨模块读静态数据靠名字撞在一起才能凑巧工作 |
| `seed/lain0/include/lain0/Sema.hpp:54` | `ModuleMember::Kind::Func` 已经存在，但 `seed/` 里没有任何地方给它赋值 | 函数成员完全不可见 |
| `seed/lain0/src/LowerExpr.cpp:913` | `symbol_addr("X_storage")` 把字符串原样当作符号名 | 跨单元读数据只能靠全局平坦名字 |

bootstrap/ 因此继续用 28 份源码拼接 + 649 条跨单元 extern 影子声明的办法完成链接。

## 二、目标

1. lain0 提供模块命名空间：每个模块的顶层声明（函数、静态数据、类型）都带上模块前缀，不同模块的同名声明互不冲突。
2. 模块导出函数：`M.f(x)` 直接可用，`M.NAME`、`M.Type`、跨模块字段访问同时可用。
3. `bootstrap/` 的 28 个单元各自写出 import 依赖，删掉 649 条跨单元 extern 影子声明，不再拼接源码；`lain0 --unit bootstrap/SOURCE_ORDER` 一次产出整份模块。

## 三、设计

### 3.1 模块身份

- 源根：`lain0 --unit <清单>` 时是清单文件所在目录；`lain0 <文件>` 时是入口文件所在目录。
- 模块键（key）：被解析到的文件路径相对源根、去掉 `.lain`、把 `/` 换成 `_`。例：`bootstrap/std/lex.lain` → `std_lex`，`bootstrap/meta.lain` → `meta`。
- 路径解析沿用现有规则（`seed/lain0/src/Sema.cpp:32` `resolveModulePath`：先试 `<导入方目录>/<模块名>.lain`，再试 `<导入方目录>/<限定前缀>/<模块名>.lain`），解析结果按 `.`/`..`/`//` 做纯词法规范化后相对源根取键。规范化后跑出源根（出现 `..`）→ `D::SemaBadImport`「导入路径超出源根」。
- 模块键里出现 `__`（连续两个下划线）→ `D::SemaBadImport`「模块键不能含连续下划线」。这条保证下面 3.2 的切分没有歧义。
- 同一个模块键只加载一次、只解析一次、只降级一次。互相 import 的两个模块由此自然终止。

### 3.2 符号命名

| 声明 | 发出的名字 |
| --- | --- |
| `func f` | `<键>__f` |
| `let x`（字节串 / 定长表 / 顶层聚合构造） | 数据符号 `<键>__x_storage`，取址过程 `<键>__x` |
| `let x`（其余初值形态） | 值过程 `<键>__x` |
| `struct` / `enum` | 类型表里的键 `<键>__<名字>`，不产出 IR 项 |

清单里标为根名的声明例外，发出名字保持平坦（bootstrap 只有 `meta.lain` 的 `lain_std_abi_version`、`lain_std_initialize`、`lain_std_lower` 三个）。宿主 extern 的 `link_name` 依旧原样搬出、依旧跨单元重复、依旧平坦（`seed/kernel/src/ir/verify.c:890` 允许同名双方都是 extern 且 `link_name` 相等）。

### 3.3 清单格式

`bootstrap/SOURCE_ORDER` 就地扩成模块清单：每行 `<相对路径> [<根名> …]`，路径相对清单所在目录、省略 `.lain` 后缀；`#` 开头为注释。首部注释改写为「清单顺序即降级顺序，它是产物确定性的来源；同一份清单必须给出逐字节相同的模块」，并说明根名的含义。例：

```
std/lex
…
meta lain_std_abi_version lain_std_initialize lain_std_lower
```

### 3.4 加载与遍历顺序

1. 从清单（或单文件入口）出发，按宽度优先把整个 import 依赖图解析完；已加载的模块键直接复用。每个模块保存自己的文本、AST、别名表（`别名 → 模块键`）与成员表。
2. 每个模块的成员表由「被导入模块自己的顶层名字」构成：`别名.成员` → `{Kind::Type | Value | Func, 类型, 发出的符号名}`。成员表只引用被导入模块的顶层声明，所以一轮就能填满，不需要迭代到不动点。
3. 降级顺序：先清单里的模块（按清单顺序），再依赖图里新出现的模块（按发现顺序），每个模块内按源码顺序。整个编译只用一个 `L1Builder`，所有模块的数据块与过程依次追加，产出一个 `lainir_module`（模块名取根模块键；`seed/kernel/src/text/print.c` 不打印模块名，产物文本与现在可比）。

### 3.5 类型表

`TypeStore` 的键改成模块限定名：`addStruct`/`addEnum` 用 `<键>__<名字>`，`byName_` 的既有查询点全部改成先限定再查。`TypeStore::resolve` 增加一个「当前模块上下文」参数（模块键 + 别名表）：

- 裸名 → `byName_[<键>__<名字>]`；
- 限定名 `M.Point` → 别名表查出 `M` 的模块键，再查 `byName_[<键>__Point]`；别名不在别名表里则退回「限定符本身是类型名」（`Shape.Circle` 形态）的既有分支；
- 内建标量表不变，模块不导出标量。

聚合布局登记分两轮：第一轮给所有模块的 struct/enum 建名字条目，第二轮解析字段与载荷类型并算布局；字段类型指向尚未算布局的聚合时递归计算，递归回到自身 → `D::SemaUnknownType`「按值包含自身的聚合」。

### 3.6 表达式与数据访问

| 写法 | 降级结果 |
| --- | --- |
| `f(x)`（本模块） | `#call @<键>__f(…)` |
| `M.f(x)` | `#call @<被导入模块键>__f(…)`（`lowerCall` 接受 `DottedExpr` 被调者） |
| `M.NAME` | `#call @<被导入模块键>__NAME()`，结果是 addr（既有访问器形态） |
| `NAME`（本模块静态数据） | `#call @<键>__NAME()` |
| `symbol_addr("NAME_storage")` | 只在当前模块内解析：核对 `NAME` 是本模块的顶层数据名，发出 `#bitcast[#addr](%<键>__NAME_storage)`；不是本模块的数据名 → `D::SemaUnknownName`「symbol_addr 只能取本模块的静态数据，跨模块请用 M.名字」 |
| `p.x`（`p: M.Point`） | 由 Sema 给出的类型取布局，`INST_LEA` + `INST_LOAD`；`p` 的类型已经是 `<键>__Point`，这条通路随 3.5 自动成立 |
| `M.NAME + i`、`store(M.NAME + i, v)` | 访问器给的是基址，按既有地址算术处理 |
| `M.f` 当值使用、`M.Shape.Circle` | `D::LowerUnsupported`，消息写明该形态不支持 |

跨模块读写静态数据一律走 `M.名字`，`symbol_addr` 退回「本模块静态数据取址」这一种用途。bootstrap 里 167 处跨单元 `symbol_addr` 按此改写，329 处同单元 `symbol_addr` 保持原样（仍然要看本模块的数据名表）。

### 3.7 命令行与产物

`seed/lain0/tools/main.cpp` 增加 `--unit <清单>`：

```
lain0 <file.lain> [-o out.l1] [--dump-tokens] [--dump-ast] [--round-trip]
lain0 --unit <清单> [-o out.l1] [--round-trip]
```

`--unit` 与位置参数互斥，两者都给或都不给 → 退出码 2；清单打不开 → 退出码 2，消息 `lain0: 打不开清单 <路径>`。其余行为（verify 不通过不产出文本、`--round-trip` 固定点检查、写出失败的消息与退出码）保持不变。

`scripts/build_meta_unit.py` 缩成一层包装：调用 `lain0 --unit bootstrap/SOURCE_ORDER -o build/lain0/bootstrap.l1`，然后把 `../build/lain0/bootstrap.l1` 一行写进 `build/lain0/bootstrap_unit.txt`（`seed/meta_driver.c:220` 会在每行前拼 `bootstrap/`）。拼接源码与丢弃 extern 行的逻辑删除。

### 3.8 内核不改

`#call` 的名字解析、extern 的能力绑定、数据符号映射全部是按名字的字符串比较（`seed/kernel/src/vm/image.c:318`、`seed/kernel/src/vm/tcb.c:113`、`seed/kernel/src/meta/apply.c:26`），模块前缀足够区分符号。宿主能力的 `link_name` 保持平坦，`LAINVM_CAPS_NAME_MAX`（64）与 IR 名字长度都不受影响（最长的新名字 `std_macros_my_block__meta_expand_replacement_my_block`，53 字节）。

## 四、代码改动清单

| 文件 | 改动 |
| --- | --- |
| `seed/lain0/include/lain0/Sema.hpp` | `ModuleMember` 增加「发出的符号名」语义并真正填入 `Kind::Func`；新增 `ModuleContext`（模块键、源根、别名表）；`TypeStore::resolve` 增参；`SemaResult` 的键改为模块限定名；新增 `Program`（模块集合、加载、两轮分析）声明 |
| `seed/lain0/src/Sema.cpp` | `resolveModulePath`/`readFileText`/`parseModuleText`/`directoryOf` 移入 `Program`；`Walker` 持有 `ModuleContext`，登记与查询全部限定；import 分支改为查已加载模块的成员表；类型布局两轮登记 |
| `seed/lain0/include/lain0/Lower.hpp` | `LowerOptions` 增加模块键与别名表；`Lowerer` 增加模块上下文与跨模块成员查询 |
| `seed/lain0/src/Lower.cpp` | `run` 改为按 Program 的模块顺序依次降级，共用一个 `L1Builder`，末尾建一个 `lainir_module` |
| `seed/lain0/src/LowerDecl.cpp` | `lowerFunc`/`lowerStaticLet`/`buildAccessor` 的过程名与数据符号加模块前缀；`lowerImport` 删除（改由 Program 加载）；根名声明发出平坦名字 |
| `seed/lain0/src/LowerExpr.cpp` | `lowerCall` 接受模块成员被调者；`lowerDottedExpr` 用成员表里真正的符号名；`symbol_addr` 按本模块数据名表限定；`lowerProjection` 的枚举名限定 |
| `seed/lain0/src/Driver.cpp`、`seed/lain0/include/lain0/Driver.hpp` | `compileFile` 走 Program；新增 `compileUnit(manifestPath, opts, …)`；诊断与 verify 门槛不变 |
| `seed/lain0/tools/main.cpp` | `--unit` 选项与互斥检查 |
| `bootstrap/SOURCE_ORDER` | 改成模块清单，28 行保留顺序，`meta` 行带三个根名，注释改写 |
| `bootstrap/std/**`、`bootstrap/meta.lain` | 加 import、删 649 条影子 extern、改写 652 个跨单元调用点与 167 处跨单元 `symbol_addr` |
| `bootstrap/lain/examples/` | `modules_lib.lain`/`modules_main.lain` 扩展；新增 `modules_twin_a.lain`、`modules_twin_b.lain`、`modules_twin.lain` |
| `scripts/build_meta_unit.py` | 缩成调用 `lain0 --unit` 的包装 |
| `scripts/namespace_audit.py` | 删除（审计已完成，脚本属于一次性统计） |
| `docs/04-bootstrap-dialect.md` | 模块一节的成员位置表、mangle 例子、跨模块函数成员与字段访问的「待补规则」改写为新行为 |
| `docs/03-lain-meta.md`、`docs/lain0.md`、`docs/development.md`、`docs/plan-lain0.md`、`docs/decisions.md` | 命令行、清单格式、状态数字更新；追加决策 D-004 |

## 五、bootstrap 改写的工作量

| 项目 | 数量 |
| --- | --- |
| 单元 | 28（另加 `meta.lain` 在清单末位） |
| 顶层声明 | 734（518 个 func、216 个 let） |
| 现有 extern 行 | 785（649 条指向本仓库定义的影子声明 → 删除；136 条宿主能力 → 保留） |
| 模块依赖边（调用 + 跨单元取址） | 180 |
| 需要改写的跨单元调用名字对 | 652（按「单元 × 外来名字」逐对替换所有调用点） |
| 需要改写的跨单元 `symbol_addr` | 167 处，落在 28 个「单元 × 模块」对上 |
| 保持原样的同单元 `symbol_addr` | 329 处 |
| 无 extern 声明的静默跨单元调用 | 4 处（`std/emit.lain` → `lainir_emit_uint`、`lainir_repr`；`std/lex.lain` → `meta_bytes_equal`；`std/regions.lain` → `meta_cell_get`），import 之后自然解决 |
| 单元里声明的别名 | 采用模块键作为别名（`std_lex`、`std_handlers_struct`、`meta`），已核对与关键字、内建标量、任何顶层声明都不冲突 |

改写用文件编辑工具逐处完成，不使用脚本批量改源码。

## 六、验收闸门

1. `cmake --build build` 通过，`build/lain0/lain0.exe`、`headers_check.exe`、`probe_seed.exe`、`build/lain-meta.exe` 全部重新生成。
2. `bash scripts/regress.sh` 与 `bash scripts/regress.sh --round-trip` 都是 `PASS=38 FAIL=0`、退出码 0（现在是 35，新增 3 个样例）。
3. 命名空间正确性：`modules_twin` 样例里两个模块各自声明同名的 `dup`，编译产物同时含 `modules_twin_a__dup` 与 `modules_twin_b__dup`，运行结果等于 `1 + 2`。改动之前这一编译必然报 verify 的 `duplicate subroutine`（2022）。
4. 函数导出：`modules_main` 样例里的 `M.bump(M.twice)` 编译通过，产物含 `#call @modules_lib__bump`。
5. 跨模块数据与字段：`M.table` 读到的地址可被 `store` 使用；`func px(p: M.Point) -> i32 { return p.x; }` 编译并通过 verify。
6. 确定性：`lain0 --unit bootstrap/SOURCE_ORDER -o build/acp/unit_a.l1` 连续跑两次，两份产物 SHA-256 相同。
7. bootstrap 规模与信号：`lain-meta --unit build/lain0/bootstrap_unit.txt --out build/x.out examples/arith.lain bootstrap/lain/examples/arith.lain` 输出 `verify subroutines=870 data=216`、`sources count=1`、`image ok`、`host_status=0`、`region grants=2`、`output bytes=74`，与改造前的数值逐项相同。
8. 全样例对照：24 个绿色样例的 `host_status`、`region grants`、输出字节数、`.out` 的 MD5 与改造前逐项相同；11 个红色样例（jumps、loop、loop_if、loop_local、loop_value、modules_lib、modules_main、refs、strinline、symbol_addr、u8math）的退出码、`host_status`、trap、输出 MD5 与改造前逐项相同。红色样例的修复不在本计划范围内。
9. `extern` 净化：`bootstrap/` 里不再有任何 extern 名字同时是别处的顶层定义名（脚本核对一次，数量为 0）。
10. 文档闸门：`docs/04-bootstrap-dialect.md` 里不再有「模块里的函数成员还调不了」「跨模块的字段访问 p.x 报 4」这类描述；`docs/development.md` 的样例统计更新为 38 与新的通过数。

## 七、工作顺序

每一步都要跑完自己的闸门再进入下一步。

1. **Sema 与类型表限定**：`Program` + `ModuleContext`，类型键限定，单文件入口也走模块前缀。闸门：`cmake --build build` 通过；`bash scripts/regress.sh --round-trip` 仍是 35 全过；抽查 `arith.lain` 的产物里顶层名字带 `arith__` 前缀。
2. **导出与命令行**：成员表填 `Kind::Func`，`M.f(x)`、`M.NAME`、`M.Type` 打通，`--unit` 与清单解析落地，根名保持平坦。闸门：闸门 3、4、5、6（新样例与确定性），`regress.sh` 全过。
3. **样例扩充**：新增 `modules_twin_a/b`、`modules_twin`，扩展 `modules_lib`/`modules_main`。闸门：验收闸门 2。
4. **bootstrap 改写**：按清单顺序逐个单元改（先 `std/lex`，最后 `meta.lain`），每改完一个单元用 `lain0 --unit` 编译整份清单确认不引入新诊断；全部改完后跑闸门 7、8、9。这一步之前 `--unit bootstrap/SOURCE_ORDER` 预期报错，属于中间状态。
5. **脚本与文档**：`build_meta_unit.py` 缩成包装，删除审计脚本，更新四份文档，追加 D-004。闸门：`python scripts/build_meta_unit.py`、`bash scripts/regress.sh`、闸门 7 一起复跑一遍。

## 八、决策 D-004（写入 docs/decisions.md）

**What**：模块命名空间与根名清单。每个模块的顶层声明发出 `<模块键>__<名字>`，模块键由源根相对路径去掉 `.lain` 后把 `/` 换成 `_` 得到；清单里标为根名的少数声明保持平坦，供宿主按名字查找；跨模块访问一律写 `M.名字`，`symbol_addr` 只管本模块的静态数据。

**Why**：内核按平坦字符串解析名字，模块前缀是让两个模块的同名声明共存的最小手段；清单承载顺序与根名，使整份模块的产物只由清单决定，满足「同一份 SOURCE_ORDER 必须给出逐字节相同的模块」。

**How**：`Program` 一次加载整个依赖图并两轮分析，`Lowerer` 按清单顺序把全部模块降级进同一个 `L1Builder`；`scripts/build_meta_unit.py` 只负责调用 CLI 并生成驱动要的 unit 列表。

**Status**：采纳。
