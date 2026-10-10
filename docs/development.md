# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端和宿主能力 |
| `bootstrap/` | LAINIR Meta 库（stage0：手写 LAINIR 文本） |
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

把 LAINIR Meta 库装进一台 TCB 并推进它。位置参数成对给出「逻辑路径 源文件」，
逻辑路径就是 import 解析注册表里的名字：

```powershell
build/lain-meta.exe --out build/arith.out examples/arith.lain bootstrap/lain/examples/arith.lain
```

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

改动 `bootstrap/` 之后按顺序跑这三条：

```powershell
powershell -File build/merge.ps1                 # SOURCE_ORDER 拼成 build/bootstrap_merged.l1
build/check_unit.exe build/bootstrap_merged.l1   # 单元本身必须 PARSE+VERIFY OK
powershell -File build/reg.ps1                   # 样例与层 0 探针回归（脚本被 Git 忽略）
```

`build/check_unit.exe` **不是 CMake 目标**（`CMakeLists.txt` 只建 `lain-meta` 驱动），改了 `seed/` 或 `bootstrap/` 之后要手工重建，否则校验结果来自旧二进制：

```powershell
cmake --build build
clang -O0 -std=c17 -D_DEBUG -D_DLL -D_MT -g -I seed/include -o build/check_unit.exe build/check_unit.c build/lib/lain_seed.lib
```
