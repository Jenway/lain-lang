# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | seed 顶层工程；先 `add_subdirectory(kernel)` 编出 `lain_seed`，再 `add_subdirectory(lain0)` 编 C++ 前端 |
| `seed/kernel/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端和宿主能力（`lain_seed` 静态库） |
| `seed/lain0/` | C++ 写的 Lain 编译器前端，链接 `seed/kernel` 产出的 `lain_seed` |
| `bootstrap/` | Lain 源码写的 Meta 库（28 个模块，**无清单**）；`lain0 --entry bootstrap/meta.lain --split build/lain0/units/` 一次编出**每个模块一份** LAINIR（文件名 = 模块键），驱动按目录装载 |
| `tools/` | 驱动与验收工具，只装配、推进、记账，不含语言知识 |
| `docs/` | 设计、规范与进度|
| `build/` | 二进制、临时输入、快照和日志，不提交 |

根 README 和 AGENTS 只保留导航及必要工作规则。

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

### 驱动：lain-meta

把 LAINIR Meta 库装进一台 TCB 并推进它。先由 `scripts/build_meta_unit.py` 用
`lain0 --entry bootstrap/meta.lain --split build/lain0/units/` 把 Meta 编成**每模块一份**
LAINIR（28 份，文件名 = 模块键），驱动装载整个目录、按文件名排序后合并成一个镜像；
再把一份源码交给它。位置参数成对给出「逻辑路径 源文件」，逻辑路径就是 import
解析注册表里的名字：

```bash
python scripts/build_meta_unit.py     # 产物：build/lain0/units/*.l1（28 份，文件名 = 模块键）
build/lain-meta.exe --units-dir build/lain0/units/ \
    --out build/arith.out examples/arith.lain bootstrap/lain/examples/arith.lain
```

装载来源**二选一、必填**：`--unit <清单>`（每行一个 LAINIR 路径，驱动会给每行前缀
`bootstrap/`）或 `--units-dir <目录>`（装载目录里所有 `.l1`，按文件名排序 = 模块键排序）。
两者都不给 → 退出码 2 + 提示；都给 → 退出码 2 + `--unit 与 --units-dir 互斥`。
没有默认值，必须显式给出一个。

| 选项 | 默认 | 说明 |
| :--- | :--- | :--- |
| `--unit <文件>` | 无（与 `--units-dir` 二选一，必填） | 编译单元文件清单，每行一个 LAINIR 路径（驱动前缀 `bootstrap/`），`#` 起注释 |
| `--units-dir <目录>` | 无（与 `--unit` 二选一，必填） | 装载目录里所有 `.l1`，按文件名排序（= 模块键排序）合并 |
| `--entry <名>` | `lain_std_lower` | 入口过程 |
| `--lower <n>` | 0 | 交给入口的 lowering 档位 |
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
`bootstrap/lain/examples/` 下的样例就是回归集：`region grants=2`、`host_status=0`。

## 验收

改动 `bootstrap/` 或 `seed/` 之后：

```bash
cmake --build build               # 重建 seed 库与 lain-meta（改了 seed/ 必须重建，否则跑的是旧二进制）
bash scripts/regress.sh           # lain0 全量回归：examples 下每个 .lain 都要编译通过
bash scripts/regress.sh --round-trip   # 同上，另加 print(parse(text)) == text 固定点检查
python scripts/build_meta_unit.py --check-twice   # 入口 -> 每模块一份 LAINIR，并断言整批指纹跑两次一致
python scripts/units_fingerprint.py    # 单独看整批指纹（值见下）
python scripts/import_export_scan.py --audit   # 模块卫生检查：裸跨单元调用 / 影子 extern / 跨单元取址
python scripts/diff_two_compilers.py    # 步 4 差分：lain0 的各份单元 × Meta 降级同一份源码，逐份比对（见下）
```

**验收判据是「整批指纹跑两次一致」，不是「等于某个历史哈希」。** 拆成多份之后，
「一整份的字节数与 SHA-256」不再是有效判据（每份文件只取决于该模块自己，顺序无关）。
整批指纹的定义与算法见 `docs/proposal-modules-v2.md` §7.2，由
`scripts/units_fingerprint.py` 实现：按模块键排序，逐行拼 `<键>  <该份 SHA-256>`（两个
空格），行间 `\n`、末尾再补一个 `\n`，对这个字节串取 SHA-256。当前实测值：

```
units=28 fingerprint=ef2f198c12a36bd44ee040dcf1e06f928759c49697afdd9bc1144b28ebd4f977
```

拆分的等价性（28 份 = 一整份）由 `python scripts/split_equiv_check.py --build` 证明：
逐模块切出整份里属于该模块的那一段，把临时值编号（`%rN` → `%r#`）规范化后与
`units/<键>.l1` 逐一比对，28/28 相同。

**两条编译路径的差分（步 4）**由 `python scripts/diff_two_compilers.py`（可选 `--only <模块键>`）
报：对每个模块，① 取 lain0 的该份单元 `build/lain0/units/<键>.l1`，② 用 Meta 把**同一份源码**
`bootstrap/<路径>.lain` 降级一份到临时文件，逐份比对。表是「模块 × Meta 状态 × 是否一致」，
差异会**分类**（符号前缀/mangle 不一致、临时值编号、段布局/空行、缺失/截断/其他），并给规范化
前后两个口径（`%rN` → `%r#`、`<键>__` → 空，同 `split_equiv_check.py` 的思路），另报 Meta 已发出的行
规范化后与 lain0 单元对上的比例（如 `std_regions` 是 2/3 —— 对不上的那条正是少了 `_storage` 后缀的数据符号）。**退出码恒 0**
（它是观测工具不是闸门；要闸门加 `--strict`，红行即退出码 1）。**红行 = 两条路径还没对上，不是脚本坏了**：
今天 Meta 只把 4 个模块（import 闭包 ≤ 驱动 32 条登记上限）喂进驱动，且这 4 个都还没降级成功
（`std_types` `status=0`+trap、`std_regions` `9415`、`std_registry` `4`、`std_lex` `6`，产物截断），
所以逐字相同与规范化后相同都是 **0**；其余 24 个的闭包要 63 条登记，连驱动都进不去。随
`docs/plan-selfhost.md` 的九步进度逐行转绿。

`scripts/import_export_scan.py` 是可复用的**模块卫生检查器**：默认输出是源码形态分类与逐单元表，
`--audit` 逐单元数「裸跨单元调用 / `M.成员` 调用 / `M.数据成员` / 跨单元取址 / 影子 extern /
宿主 extern / 同单元取址」，`--rev <rev>` 用 `git show <rev>:<路径>` 读历史版本，同一套口径可以量
改写前后的状态。第 4 步的收尾数字（裸跨单元调用 0、影子 extern 0、跨单元取址 0、宿主 extern 136、
同单元取址 328、0 处错配）就是它报的。

跑样例（`bootstrap/lain/examples/` 就是回归集）：

```bash
build/lain-meta.exe --units-dir build/lain0/units/ \
    examples/arith.lain bootstrap/lain/examples/arith.lain
```

期望 `host_status=0`、`region grants=2`；`output bytes=` 是 Meta 写出的 canonical LAINIR 长度。
摘要行形如 `unit files=N bytes=N`、`verify subroutines=N data=N`、`run slice=N rounds=N steps=N
host_status=N`、`region grants=N`、`output bytes=N`，可直接被脚本断言。整份 Meta 上实测
`unit files=28 bytes=568387`、`verify subroutines=870 data=216`、`output bytes=74`，
arith 产物与 `build/arith.out` 逐字节相同。

当前 **38** 个样例（`bootstrap/lain/examples/*.lain`：35 个基础样例 + `modules_twin_a` /
`modules_twin_b` / `modules_twin`）。两套口径的数字不同：

- **lain0 全量回归**：`bash scripts/regress.sh` 与 `--round-trip` 都是 **PASS=38 FAIL=0**。
- **Meta 管线**（`lain-meta` 编译表面源码；判据是 `host_status=0` 且无 trap）：38 个样例中
  26 个通过。12 个失败：`jumps`、`loop`、`loop_if`、`loop_local`、`loop_value` 以 trap
  status 1004 结束、产物被截断；`modules_lib`（host_status=4）、`modules_main`（11）、
  `modules_twin`（11）、`refs`（5）、`strinline`（4）、`symbol_addr`（9415）、`u8math`（6）
  返回非零 host_status。诊断记录在 `docs/selfhost-gap.md`。
