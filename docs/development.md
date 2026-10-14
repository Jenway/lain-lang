# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端和宿主能力 |
| `bootstrap/` | Lain 源码写的 Meta 库（28 个单元 + `SOURCE_ORDER` 链接顺序）；编译出的 LAINIR 就是 Meta 本体 |
| `tools/` | 驱动与验收工具，只装配、推进、记账，不含语言知识 |
| `docs/` | 设计、规范与进度|
| `build/` | 二进制、临时输入、快照和日志，不提交 |

根 README 和 AGENTS 只保留导航及必要工作规则。

## 构建

### CMake：seed 库

```powershell
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/cmake
```

产物是 `lain_seed`（静态库）与 `lain-meta`（驱动）。

### 驱动：lain-meta

把 LAINIR Meta 库装进一台 TCB 并推进它。先由 `scripts/build_meta_unit.py` 把
`bootstrap/` 的 Lain 源码拼成一份、用 lain0 编成 LAINIR，再把一份源码交给它。
位置参数成对给出「逻辑路径 源文件」，逻辑路径就是 import 解析注册表里的名字：

```bash
python scripts/build_meta_unit.py     # 产物：build/lain0/bootstrap.{lain,l1} 与 bootstrap_unit.txt
build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    --out build/arith.out examples/arith.lain bootstrap/lain/examples/arith.lain
```

`--unit` 要显式给：驱动的默认值是 `bootstrap/SOURCE_ORDER`，而本仓库里那个文件是
**Lain 源码**的链接顺序表，不是 LAINIR 单元清单（清单由上面的脚本生成，里面写
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
python scripts/build_meta_unit.py # 源码 -> 单份 LAINIR 单元 + 驱动要的清单
```

跑样例（`bootstrap/lain/examples/` 就是回归集）：

```bash
build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    examples/arith.lain bootstrap/lain/examples/arith.lain
```

期望 `host_status=0`、`region grants=2`；`output bytes=` 是 Meta 写出的 canonical LAINIR 长度。
摘要行形如 `unit files=N bytes=N`、`verify subroutines=N data=N`、`run slice=N rounds=N steps=N
host_status=N`、`region grants=N`、`output bytes=N`，可直接被脚本断言。

当前 34 个样例中 29 个通过；其余 5 个（`symbol_addr` / `u8math` / `refs` / `strinline` /
`modules_main`）报的是 Meta 的语言级状态码，出处与定位见 `docs/plan-meta-lain.md` 的「阶段 3」。
