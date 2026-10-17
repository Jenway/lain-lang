# lain0 模块命名空间

`bootstrap/` 的 28 个单元各自写出 import 依赖，由入口 `bootstrap/meta.lain` 顺 import
求闭包，`lain0 --entry … --split` 编成**每个模块一份** LAINIR。不拼接源码，也不保留
跨单元 extern 影子声明。

## 3.1 模块身份

- 源根：`lain0 --entry <入口>` 与 `--split <目录>` 下是入口文件所在目录；`lain0 <文件>`
  时是入口文件所在目录。
- 模块键（key）：被解析到的文件路径相对源根、去掉 `.lain`、把 `/` 换成 `_`。例：
  `bootstrap/std/lex.lain` → `std_lex`，`bootstrap/meta.lain` → `meta`。
- 路径解析沿用现有规则（`seed/lain0/src/Sema.cpp` 的 `resolveModulePath`：先试
  `<导入方目录>/<模块名>.lain`，再试 `<导入方目录>/<限定前缀>/<模块名>.lain`），解析结果
  按 `.` / `..` / `//` 做词法规范化后相对源根取键。规范化后跑出源根（出现 `..`）→
  `D::SemaBadImport`「导入路径超出源根」。
- 模块键里出现 `__`（连续两个下划线）→ `D::SemaBadImport`「模块键不能含连续下划线」。
  这条保证 3.2 的切分没有歧义。
- 同一个模块键只加载一次、只解析一次、只降级一次。

## 3.2 符号命名

| 声明 | 发出的名字 |
| --- | --- |
| `func f` | `<键>__f` |
| `let x`（字节串 / 定长表 / 顶层聚合构造） | 数据符号 `<键>__x_storage`，取址过程 `<键>__x` |
| `let x`（其余初值形态） | 值过程 `<键>__x` |
| `struct` / `enum` | 类型表里的键 `<键>__<名字>`，不产出 IR 项 |

带 **`@root`** 标注的声明例外，发出名字保持平坦（`bootstrap/meta.lain` 的
`lain_std_abi_version`、`lain_std_initialize`、`lain_std_lower` 三个）。宿主 extern 的
`link_name` 原样搬出、跨单元重复、保持平坦（`seed/kernel/src/ir/verify.c` 允许同名双方
都是 extern 且 `link_name` 相等）。

## 3.3 模块集合与产出

- 模块集合由入口 `bootstrap/meta.lain` 顺 `import("…")` 求闭包得到，实测 28 个模块，
  没有模块清单。
- `lain0 --entry bootstrap/meta.lain --split build/lain0/units/` 每个模块出一份
  `<模块键>.l1`。每份的文本只由该模块自己的源码决定（每模块一个 `L1Builder`，临时值
  编号从 0 起），与模块顺序无关。
- 写出前先把 28 份拼成一份过 `lainir_parse` + `lainir_verify`；不过闸门就一份都不写。
- 装载：驱动 `--units-dir <目录>` 装入目录里所有 `.l1`，按文件名排序（= 模块键排序）
  合并成一个镜像，合并后按名解析跨模块引用，`verify` 在合并之后做。`--unit <清单>`
  仍保留（每行一个 LAINIR 路径），两者必须显式给一个。

## 3.4 加载与遍历顺序

1. 从入口出发，把整个 import 依赖图解析完；已加载的模块键直接复用。每个模块保存自己的
   文本、AST、别名表（`别名 → 模块键`）与成员表。
2. 每个模块的成员表由「被导入模块自己的顶层名字」构成：`别名.成员` →
   `{Kind::Type | Value | Func, 类型, 发出的符号名}`。成员表只引用被导入模块的顶层声明，
   一轮就能填满。
3. 降级按模块分别进行：每个模块一个 `L1Builder`，产物写进 `<模块键>.l1`。

## 3.5 类型表

`TypeStore` 的键是模块限定名：`addStruct` / `addEnum` 用 `<键>__<名字>`，`byName_` 的
查询点先限定再查。`TypeStore::resolve` 带一个「当前模块上下文」参数（模块键 + 别名表）：

- 裸名 → `byName_[<键>__<名字>]`；
- 限定名 `M.Point` → 别名表查出 `M` 的模块键，再查 `byName_[<键>__Point]`；别名不在
  别名表里则退回「限定符本身是类型名」（`Shape.Circle` 形态）的既有分支；
- 内建标量表不变，模块不导出标量。

聚合布局登记分两轮：第一轮给所有模块的 struct / enum 建名字条目，第二轮解析字段与载荷
类型并算布局；字段类型指向尚未算布局的聚合时递归计算，递归回到自身 →
`D::SemaUnknownType`「按值包含自身的聚合」。

## 3.6 表达式与数据访问

| 写法 | 降级结果 |
| --- | --- |
| `f(x)`（本模块） | `#call @<键>__f(…)` |
| `M.f(x)` | `#call @<被导入模块键>__f(…)`（`lowerCall` 接受 `DottedExpr` 被调者） |
| `M.NAME` | `#call @<被导入模块键>__NAME()`，结果是 addr（访问器形态） |
| `NAME`（本模块静态数据） | `#call @<键>__NAME()` |
| `symbol_addr("NAME_storage")` | 只在当前模块内解析：核对 `NAME` 是本模块的顶层数据名，发出 `#bitcast[#addr](%<键>__NAME_storage)`；不是本模块的数据名 → `D::SemaUnknownName`「symbol_addr 只能取本模块的静态数据，跨模块请用 M.名字」 |
| `p.x`（`p: M.Point`） | 按 Sema 给出的类型取布局，`INST_LEA` + `INST_LOAD` |
| `M.NAME + i`、`store(M.NAME + i, v)` | 访问器给基址，按既有地址算术处理 |
| `M.f` 当值使用、`M.Shape.Circle` | `D::LowerUnsupported`，消息写明该形态不支持 |

跨模块读写静态数据一律走 `M.名字`，`symbol_addr` 只用于本模块静态数据取址。

## 3.7 命令行与产物

`seed/lain0/tools/main.cpp` 提供三条入口：

```
lain0 <file.lain> [-o out.l1] [--dump-tokens] [--dump-ast] [--round-trip]
lain0 --unit <清单> [-o out.l1] [--round-trip]
lain0 --entry <file.lain> [--split <目录>] [-o out.l1] [--round-trip]
```

`--split` 只能与 `--entry` 同用；`--split` 与 `-o` 互斥，产物写进目录。其余行为
（verify 不通过不产出文本、`--round-trip` 固定点检查、写出失败的消息与退出码）保持不变。

`scripts/build_meta_unit.py` 调 `lain0 --entry bootstrap/meta.lain --split
build/lain0/units/`，产出 28 份（文件名 = 模块键）；`--check-twice` 再拆一次到临时目录，
比较整批指纹。

## 3.8 内核不改

`#call` 的名字解析、extern 的能力绑定、数据符号映射全部是字符串比较
（`seed/kernel/src/vm/image.c`、`seed/kernel/src/vm/tcb.c`、`seed/kernel/src/meta/apply.c`），
模块前缀足够区分符号。宿主能力的 `link_name` 保持平坦；`LAINVM_CAPS_NAME_MAX`（64）与
IR 名字长度都不受影响。

## 验收

1. `cmake --build build` 通过，`build/lain0/lain0.exe`、`headers_check.exe`、
   `build/lain-meta.exe` 全部重新生成。
2. `bash scripts/regress.sh` 与 `bash scripts/regress.sh --round-trip` 都是
   `PASS=38 FAIL=0`、退出码 0。
3. 命名空间正确性：`modules_twin` 样例里两个模块各自声明同名的 `dup`，编译产物同时含
   `modules_twin_a__dup` 与 `modules_twin_b__dup`。
4. 函数导出：`modules_main` 样例里的 `M.bump(M.twice)` 编译通过，产物含
   `#call @modules_lib__bump`。
5. 跨模块数据与字段：`M.table` 读到的地址可被 `store` 使用；
   `func px(p: M.Point) -> i32 { return p.x; }` 编译并通过 verify。
6. 确定性：`python scripts/build_meta_unit.py --check-twice` 连跑两次，28 份逐份相同、
   整批指纹一致。
7. bootstrap 规模与信号：`lain-meta --units-dir build/lain0/units/ --out build/arith.out
   examples/arith.lain bootstrap/lain/examples/arith.lain` 输出 `image ok`、
   `host_status=0`、`region grants=2`、`output bytes=74`。
8. `extern` 净化：`bootstrap/` 里没有任何 extern 名字同时是别处的顶层定义名。
