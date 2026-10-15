# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | seed 顶层工程；先 `add_subdirectory(kernel)` 编出 `lain_seed`，再 `add_subdirectory(lain0)` 编 C++ 前端 |
| `seed/kernel/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端和宿主能力（`lain_seed` 静态库） |
| `seed/lain0/` | C++ 写的 Lain 编译器前端，链接 `seed/kernel` 产出的 `lain_seed` |
| `bootstrap/` | Lain 源码写的 Meta 库（28 个模块 + `SOURCE_ORDER` **模块清单**：每行 `<相对路径> [<根名> …]`）；`lain0 --unit bootstrap/SOURCE_ORDER` 一次编出的 LAINIR 就是 Meta 本体 |
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
`lain0 --unit bootstrap/SOURCE_ORDER` 把清单里的 28 个模块编成一份 LAINIR，再把一份源码交给它。
位置参数成对给出「逻辑路径 源文件」，逻辑路径就是 import 解析注册表里的名字：

```bash
python scripts/build_meta_unit.py     # 产物：build/lain0/bootstrap.l1 与 build/lain0/bootstrap_unit.txt
build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    --out build/arith.out examples/arith.lain bootstrap/lain/examples/arith.lain
```

`--unit` 要显式给：驱动的默认值是 `bootstrap/SOURCE_ORDER`，而本仓库里那个文件是
**Lain 源码**的模块清单（每行 `<相对路径> [<根名> …]`，见 `docs/plan-modules.md` §3.3），
不是 LAINIR 单元清单（单元清单由上面的脚本生成，里面写
`../build/...` 是因为驱动会把每行当 `bootstrap/<行>` 打开）。

| 选项 | 默认 | 说明 |
| :--- | :--- | :--- |
| `--unit <文件>` | `bootstrap/SOURCE_ORDER` | 编译单元文件清单，`#` 起注释 |
| `--entry <名>` | `lain_std_lower` | 入口过程 |
| `--lower <n>` | 0 | 交给入口的 lowering 档位 |
| `--out <文件>` | 无 | 产物文本落盘 |
| `--fuel` | 按已登记源码规模推导 | 执行指令上限；给出时以给出的为准 |
| `--slice` | 2000000 | 每次引擎推进的片长 |
| `--stack` | 4194304 | 栈租约字节数 |
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
python scripts/build_meta_unit.py # 模块清单 -> 单份 LAINIR 单元 + 驱动要的清单
python scripts/import_export_scan.py --audit   # 模块卫生检查：裸跨单元调用 / 影子 extern / 跨单元取址
```

`scripts/import_export_scan.py` 是可复用的**模块卫生检查器**：默认输出是源码形态分类与逐单元表，
`--audit` 逐单元数「裸跨单元调用 / `M.成员` 调用 / `M.数据成员` / 跨单元取址 / 影子 extern /
宿主 extern / 同单元取址」，`--rev <rev>` 用 `git show <rev>:<路径>` 读历史版本，同一套口径可以量
改写前后的状态。第 4 步的收尾数字（裸跨单元调用 0、影子 extern 0、跨单元取址 0、宿主 extern 136、
同单元取址 328、0 处错配）就是它报的。

跑样例（`bootstrap/lain/examples/` 就是回归集）：

```bash
build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    examples/arith.lain bootstrap/lain/examples/arith.lain
```

期望 `host_status=0`、`region grants=2`；`output bytes=` 是 Meta 写出的 canonical LAINIR 长度。
摘要行形如 `unit files=N bytes=N`、`verify subroutines=N data=N`、`run slice=N rounds=N steps=N
host_status=N`、`region grants=N`、`output bytes=N`，可直接被脚本断言。

当前 **38** 个样例（`bootstrap/lain/examples/*.lain`：35 个旧样例 + `modules_twin_a` /
`modules_twin_b` / `modules_twin`）。两套口径的数字不同，别混：

- **lain0 全量回归**：`bash scripts/regress.sh` 与 `--round-trip` 都是 **PASS=38 FAIL=0**。
- **Meta 管线**（`lain-meta` 编译表面源码；判据是 `host_status=0` **且**产物过 `lainir_parse` +
  `lainir_verify`）：**26 通过 / 12 失败** —— `jumps`、`loop`、`loop_if`、`loop_local`、
  `loop_value`（产物被截断，装回去报 `PARSE FAILED 3001`）、`modules_lib`、`modules_main`、
  `modules_twin`（Meta 的 import 降级缺口）、`refs`、`strinline`、`symbol_addr`、`u8math`。
  旧的 5 个红样例（`symbol_addr` / `u8math` / `refs` / `strinline` / `modules_main`）仍然红，
  诊断见 `docs/plan-meta-lain.md` 的「阶段 3」与「新判据下的真实进度」；只按「Meta 有没有跑完」
  数则是 31/38。这些缺口与 `--unit` 一次只报一个诊断的实现局限记在 `docs/plan-meta-lain.md`
  的待办里。
