# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | seed 顶层工程；先 `add_subdirectory(kernel)` 编出 `lain_seed`，再 `add_subdirectory(lain0)` 编 C++ 前端 |
| `seed/kernel/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端和宿主能力（`lain_seed` 静态库） |
| `seed/lain0/` | C++ 写的 Lain 编译器前端，链接 `seed/kernel` 产出的 `lain_seed` |
| `bootstrap/` | Lain 源码写的 Meta 库（28 个模块，**无清单**）；`lain0 --entry bootstrap/meta.lain --split build/lain0/units/` 一次编出**每个模块一份** LAINIR（文件名 = 模块键），驱动按目录装载 |
| `scripts/` | 构建、回归、闸门与观测脚本；只装配、推进、记账，不含语言知识 |
| `docs/` | 设计、规范与进度 |
| `build/` | 二进制、临时输入、快照和日志，不提交 |

根 README 只放导航，AGENTS 只放工作规则。

## 构建

### CMake：seed 库

```powershell
cmake -S seed -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

`seed/CMakeLists.txt` 先 `add_subdirectory(kernel)`（C，产出 `build/lib/lain_seed`），
再 `add_subdirectory(lain0)`（C++，链接该库）。这个顺序不许调换：`lain0` 只消费
`kernel` 的产物。

产物是 `build/lib/lain_seed`（静态库）、`build/lain-meta`（驱动）、
`build/lain0/lain0`（lain0 的 CLI）。

### 校验工具：check_unit

`scripts/check_unit.c` 只调 `lainir_parse` + `lainir_verify`，把一份 LAINIR 文本装回
验证器。单元闸门用它判断 Meta 的产物是否合格，需要先编出来：

```powershell
clang -std=c11 scripts/check_unit.c \
    -Iseed/kernel/include -Lbuild/lib -llain_seed -o build/check_unit.exe
```

静态库在非 Windows 平台上的扩展名是 `.a`。

## 驱动：lain-meta

把 LAINIR Meta 库装进一台 TCB 并推进它。先由 `scripts/build_meta_unit.py` 用
`lain0 --entry bootstrap/meta.lain --split build/lain0/units/` 把 Meta 编成**每模块一份**
LAINIR（28 份，文件名 = 模块键），驱动装载整个目录、按文件名排序后合并成一个镜像；
再把一份源码交给它。

源码的登记有两种给法：

- 位置参数成对给出「登记名 源文件」，登记名就是 import 解析注册表里的名字；
- 给 `--root <目录>`，位置参数只写源文件，登记名由源文件相对该目录的词法路径算出。
  自举闸门用这一种：`--root bootstrap` 下 `bootstrap/std/lex.lain` 登记为
  `std/lex.lain`，与源码里 `import("lex")` 相对导入方目录解析的结果一致。

```bash
python scripts/build_meta_unit.py     # 产物：build/lain0/units/*.l1（28 份，文件名 = 模块键）
build/lain-meta.exe --units-dir build/lain0/units/ \
    --root bootstrap --lower 0 --out build/arith.out bootstrap/lain/examples/arith.lain
```

装载来源**二选一、必填**：`--unit <清单>`（每行一个 LAINIR 路径，驱动会给每行前缀
`bootstrap/`）或 `--units-dir <目录>`（装载目录里所有 `.l1`，按文件名排序 = 模块键排序）。
两者都不给 → 退出码 2 + 提示；都给 → 退出码 2 + `--unit 与 --units-dir 互斥`。
没有默认值，必须显式给出一个。

| 选项 | 默认 | 说明 |
| :--- | :--- | :--- |
| `--root <目录>` | 无 | 登记名 = 源文件相对该目录的词法路径；此时位置参数只写源文件 |
| `--unit <文件>` | 无（与 `--units-dir` 二选一，必填） | 编译单元文件清单，每行一个 LAINIR 路径（驱动前缀 `bootstrap/`），`#` 起注释 |
| `--units-dir <目录>` | 无（与 `--unit` 二选一，必填） | 装载目录里所有 `.l1`，按文件名排序（= 模块键排序）合并 |
| `--entry <名>` | `lain_std_lower` | 入口过程 |
| `--lower <n>` | 0 | 交给入口的 lowering 档位（`--root` 下是第几个源文件，从 0 起） |
| `--out <文件>` | 无 | 产物文本落盘 |
| `--fuel` | 按已登记源码规模推导 | 执行指令上限；给出时以给出的为准 |
| `--slice` | 2000000 | 每次引擎推进的片长 |
| `--stack` | 4194304 | stack 租约字节数 |
| `--quota` | 268435456 | 字节配额，0 表示不限额 |
| `--caps` / `--depth` | 64 / 64 | 能力表项数与调用深度 |
| `--expect-status <n>` | 无 | 要求最终 host_status 等于 n，否则退出 1 |
| `--max-output` | 4096 | 打印产物文本的字节上限 |
| `-q` / `-v` | | 只印结论 / 印逐源细节 |

退出码：0 跑到底或 `--expect-status` 满足；1 编译或执行失败；2 用法或 IO 错误；3 燃料耗尽（打印 `RUN INCOMPLETE 燃料耗尽 fuel=… used=… steps=…`，此时 `host_status` 仍是 0、产物为空）。
摘要行形如 `unit files=N bytes=N`、`verify subroutines=N data=N`、
`run slice=N rounds=N steps=N host_status=N`、`result kind=N width=N bits=N`、
`region grants=N`（宿主整块授予次数）、`output bytes=N`，可直接被脚本断言。

## 验收

改动 `bootstrap/` 或 `seed/` 之后：

```bash
cmake --build build               # 重建 seed 库与 lain-meta（改了 seed/ 必须重建，否则跑的是旧二进制）
bash scripts/regress.sh           # lain0 全量回归：examples 下每个 .lain 都要编译通过
bash scripts/regress.sh --round-trip   # 同上，另加 print(parse(text)) == text 固定点检查
python scripts/build_meta_unit.py --check-twice   # 入口 -> 每模块一份 LAINIR，并断言整批指纹跑两次一致
python scripts/units_fingerprint.py    # 单独看整批指纹
python scripts/split_equiv_check.py --build   # 28 份 = 一整份的逐模块等价性
python scripts/import_export_scan.py --audit   # 模块卫生检查：裸跨单元调用 / 影子 extern / 跨单元取址
powershell -NoProfile -File scripts/selfhost_gate.ps1   # 自举闸门：Meta 降级每个 bootstrap 单元
python scripts/diff_two_compilers.py   # 两条编译路径的逐份差分（观测工具，不是闸门）
```

**验收判据是「整批指纹跑两次一致」**；具体的历史哈希只作参考。拆成多份之后，
「一整份的字节数与 SHA-256」不再是有效判据（每份文件只取决于该模块自己，顺序无关）。
整批指纹的定义与算法见 `docs/proposal-modules-v2.md` §7.2，由
`scripts/units_fingerprint.py` 实现：按模块键排序，逐行拼 `<键>  <该份 SHA-256>`（两个
空格），行间 `\n`、末尾再补一个 `\n`，对这个字节串取 SHA-256。2026-10-25 的实测值：

```
units=28 fingerprint=29ec2c2db1218cb1152b0d9ffd0fb04ca29e8537d57f6fca135aa74aaca26aed
```

拆分的等价性由 `python scripts/split_equiv_check.py --build` 证明：逐模块切出整份里属于
该模块的那一段，把临时值编号（`%rN` → `%r#`）与根名规范化后与 `units/<键>.l1` 逐一比对，
28/28 相同。

**两条编译路径的差分**由 `python scripts/diff_two_compilers.py`（可选 `--only <模块键>`）
报：对每个模块，① 取 lain0 的该份单元 `build/lain0/units/<键>.l1`，② 用 Meta 把**同一份
源码** `bootstrap/<路径>.lain` 降级一份到临时文件，逐份比对。表是「模块 × Meta 状态 ×
是否一致」，差异会分类（符号命名不一致、临时值编号、段布局/空行、缺失/截断/其他），
并给规范化前后两个口径（`%rN` → `%r#`、`<键>__` → 空）。退出码恒 0（观测工具；要闸门加
`--strict`，红行即退出码 1）。红行表示两条路径还没对上；脚本本身没有坏。

`scripts/import_export_scan.py` 是可复用的**模块卫生检查器**：默认输出是源码形态分类与
逐单元表，`--audit` 逐单元数「裸跨单元调用 / `M.成员` 调用 / `M.数据成员` / 跨单元取址 /
影子 extern / 宿主 extern / 同单元取址」，`--rev <rev>` 用 `git show <rev>:<路径>` 读历史
版本，同一套口径可以量改写前后的状态。

**自举闸门** `scripts/selfhost_gate.ps1`：对每个 bootstrap 单元算出 import 闭包（相对
导入方解析，与 lain0 的 `resolveModulePath` 同语义），用 `--root bootstrap` 只登记闭包里
的源码，把该单元降级一份，再用 `check_unit` 把「产物 + 依赖单元」合并校验。判据是
`host_status=0`、输出里没有 trap 行、校验打印 `PARSE+VERIFY OK`，三者缺一即红。
`-Only <单元>` 只跑指定的单元（可写多个，逗号分隔）：

```powershell
powershell -NoProfile -File scripts/selfhost_gate.ps1 -Only std/regions,std/types
```

当前通过 `std/emit`、`std/regions`、`std/registry`；其余单元的卡点、闭包规模与燃料上限见
`docs/plan-selfhost.md`。

跑样例（`bootstrap/lain/examples/` 就是回归集）：

```bash
build/lain-meta.exe --units-dir build/lain0/units/ --root bootstrap \
    --lower 0 --out build/arith.out bootstrap/lain/examples/arith.lain
```

期望 `host_status=0`、`region grants=2`、`output bytes=74`。整份 Meta 上实测
`unit files=28 bytes=593524`、`verify subroutines=774 data=177`，arith 产物与
`build/arith.out` 逐字节相同。

当前 **38** 个样例（`bootstrap/lain/examples/*.lain`：35 个基础样例 + `modules_twin_a` /
`modules_twin_b` / `modules_twin`）：

- **lain0 全量回归**：`bash scripts/regress.sh` 与 `--round-trip` 都是 **PASS=38 FAIL=0**。
- **Meta 管线**（`lain-meta` 编译表面源码；判据是 `host_status=0` 且无 trap）：38 个样例
  全部通过。产物单独过 `lainir_parse` + `lainir_verify`（用 `build/check_unit.exe` 逐份
  校验）的 35 个；`bytes` 的数据符号多一层 `_storage`，`modules_main` 引用的被导入模块
  顶层值绑定过程没有模块前缀，`modules_twin` 的产物与 `modules_twin_a` /
  `modules_twin_b` 合并后通过。缺口与自举进度记录在 `docs/plan-selfhost.md`。
