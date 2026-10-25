# 提案：模块的身份、产出与导出

每个模块各出一份 LAINIR；顺序不影响任何一份文本，清单与排序规则随之取消。模块身份、
`@root` 导出、允许 import 环与差分测试保持不变。

## 一、设计依据

模块集合、编译顺序与平坦根名都不放在手写清单里：

| 信息 | 手写清单的代价 | 本设计的来源 |
| --- | --- | --- |
| 编译哪些模块 | 加模块要记得改清单 | 从入口顺 import 求闭包（`meta.lain` 闭包 **28/28**、不可达 0） |
| 按什么顺序编译 | 一份清单决定全部文本 | 每模块各出一份，顺序不影响任何一份文本 |
| 哪些名字对宿主平坦 | 根名与声明能悄悄脱节（改名不报错，跑到 `start FAILED 9102` 才炸） | 声明自己的 `@root` 标注 |

## 二、决定一：每个模块各出一份 LAINIR

**编译器**（新增一个产出模式）：

```bash
lain0 --entry bootstrap/meta.lain --split build/lain0/units/
# → units/std_lex.l1  units/std_emit.l1  …  units/meta.l1      共 28 份
# 文件名 = 模块键（键本来就是"路径换下划线"，天然当文件名）
```

**驱动**（不再需要任何清单）：

```bash
./build/lain-meta.exe --units-dir build/lain0/units/ --out … <样例>
# 装载目录里所有 .l1，按文件名排序（= 模块键排序，确定），合并成一个镜像；
# 合并后按名字解析跨模块引用，verify 在合并之后做
```

- **没有清单、没有顺序表、没有需要人维护的东西。**
- 一份单元的构成：它自己定义的过程与数据（名字仍带模块前缀）、它用到的宿主服务声明。
  外部名字声明不发（第三节实测装载器接受未声明的跨单元引用）。

编译仍然一次分析全部 28 份源码：要知道每个名字是函数还是类型，编译器本来就要读完整依赖图。
分开分析（各自只读接口文件，例如 C 的头文件或 OCaml 的 `.cmi`）需要另造接口层，不在本提案内。

## 三、前提（已实测）：装载器**接受**未在本单元声明的跨单元引用

一个单元**调用 / 取址另一个单元定义的名字**时，装载与 verify 收不收"没在本单元声明过就引用"？
**实测：收。** 于是拆分是**纯切片** —— 编译器**不需要**为每份单元补发任何外部名字声明。

复现（仓库根目录，逐条实跑）：

```bash
# 1) 拆 28 份
python scripts/build_meta_unit.py                 # -> build/lain0/units/（28 份，文件名 = 模块键）

# 2) 找一处「本单元没声明就引用别的单元」的调用
grep -n 'std_types__meta_cell_get' build/lain0/units/std_regions.l1
#   -> 25:  %r7 = #call @std_types__meta_cell_get(%r6)
grep -cE '^(extern|#proc|data) .*std_types__meta_cell_get' build/lain0/units/std_regions.l1
#   -> 0（本单元里它没有任何声明）

# 3) 整批合并装载 -> parse + verify 通过，行为与单份一致
./build/lain-meta.exe --units-dir build/lain0/units/ \
    examples/arith.lain bootstrap/lain/examples/arith.lain
#   unit files=28 bytes=593524 / verify subroutines=774 data=177 / image ok / host_status=0
```

全批普查：单元里「本单元未声明、直接引用别的单元定义的名字」共 **2649** 处（`@<名字>` 形态），
装载与 verify 全部接受。这反过来解释了 7.3 表里「它用到的**别的模块**的名字」一行**不发**任何东西：
拆分之后每份单元只含**它自己**定义的过程与数据、以及它用到的**宿主**服务声明（平坦名
`lain_meta_*`，跨单元重复是允许的）。

## 四、决定二：导出名写在声明上（`@root`）

```lain
@root
func lain_std_lower(source: u64) -> u32 { ... }
```

- 只能写在顶层声明上；带标注的声明发**平坦名**（不加 `<模块键>__` 前缀）
- **重名即错**：两个模块各有一个同名 `@root` → **编译期报错**
- 平坦名不得与任何 `<键>__<名字>` 相同 → 编译期检查
- 宿主按平坦名进入（驱动 `--entry` 语义不变）；清单里的根名列随之删除
- 词法器认 `@` 记号；只支持 `root` 一个属性，遇到别的属性名报错

## 五、保留不变

| 项 | 理由 |
| --- | --- |
| 模块键 = 源根相对路径去 `.lain`、`/`→`_`；顶层声明发 `<键>__<名字>` | 内核按平名字符串解析名字，前缀是让两个模块同名声明共存的最小手段 |
| **允许 import 环** | 编译器自己就是互递归的（26/28 个模块在环上）；分开产出之后，环不再要付代价 |
| 源码里跨模块写 `别名.名字` / `别名.f(x)` / `别名.Type` | 不变 |
| 模块键**只从解析到的文件路径算**，不能从 import 那行字符串算，并配差分测试 | 两套编译器现在在这一处分叉（Meta 从字符串算、lain0 从路径算），统一按解析到的文件路径算 |

## 六、实施步骤与判据

| 步 | 做什么 | 判据 |
| --- | --- | --- |
| **0** | 可行性实测：编一个**互相 import 的两模块**小例子，各出一份，两份一起装载 | 合并装载后 verify 过、行为与"编成一整份"一致；装载器接受未声明引用，编译器不发外部声明（第三节） |
| **1** | 编译器加 `--split` 产出 | **规范化逐份比对**：把整份里属于模块 K 的那一段切出来，与 `units/K.l1` 逐一比；临时值编号（`%rN`→`%r#`）规范化后 **28/28 相同**，即拆分零行为变化。脚本：`python scripts/split_equiv_check.py --build`。（整份的 `%rN` 是全局编号、每份各自从 1 起；根名在整份里带 `meta__` 前缀、在单元里平坦，所以比较前先规范化这两处。） |
| **2** | 加 `@root`，三个宿主入口改用标注，清单去掉根名列 | **产物逐字节不变**（证明只是搬家，行为没动） |
| **3** | 驱动改为装载目录；bootstrap 与文档切到多份产出；删掉清单 | **实测通过**：`python scripts/build_meta_unit.py --check-twice` → 28 份、整批指纹 `29ec2c2d…` 跑两次一致；`./build/lain-meta.exe --units-dir build/lain0/units/ --root bootstrap --lower 0 --out build/lain0/arith.out bootstrap/lain/examples/arith.lain` → `unit files=28 bytes=593524` / `verify subroutines=774 data=177` / `image ok` / `host_status=0` / `region grants=2` / `output bytes=74`，产物与 `build/arith.out` 逐字节相同；驱动 `--unit`/`--units-dir` 没有默认值（都不给 → 退出码 2 + 提示） |
| **4** | 差分测试：同一棵树，lain0 与 Meta 各出各的，逐份比对。脚本 `python scripts/diff_two_compilers.py`（可选 `--only <模块键>`，`--strict` 让红行变退出码 1） | 输出「模块 × Meta 状态 × 是否一致」表，**红 = 两边还没对上**。这是观测工具，不是闸门：它按「规范化 spec 逐字节相等」登记，只能喂进闭包不超过驱动登记上限的模块。Meta 侧的降级进度由 `scripts/selfhost_gate.ps1`（`--root bootstrap` 登记闭包）度量，当前 3 个叶子单元通过，卡点与燃料上限见 `docs/plan-selfhost.md` |
| **5** | `symbol_addr` 取消（**另立项**） | 普查已做：329 处全部**同单元**（312 个 `ro` 表 + 17 个定长表）、0 处跨单元；**裸名当地址实测不通过**，要先定语言规则 |

**验收判据**：用**整批指纹**——把 `"<模块键> <该份 SHA-256>"` 按模块键排序、逐行拼起来，
再取一次 SHA-256。判据是跑两次、整批指纹一致。

## 七、实施要点与落点

### 7.1 命令行与产物

```bash
# 拆分产出（新）：从入口顺 import 求闭包，每模块一份
lain0 --entry bootstrap/meta.lain --split build/lain0/units/
#   产物名 = 模块键 + .l1：units/std_lex.l1 … units/meta.l1

# 装载（驱动新开关）：装载目录里所有 .l1，按文件名排序后合并
./build/lain-meta.exe --units-dir build/lain0/units/ --out … <样例>
```

- `--entry` 与 `--unit <清单>` 互斥，`--split <dir>` 只与 `--entry` 同用。
- **不生成任何清单**：目录里的文件集合就是产物，装载顺序由文件名排序决定（确定）。
- `--unit <清单>` 保留为兼容路径：每行一个 LAINIR 路径，按行序装载。

### 7.2 整批指纹

```
fingerprint = SHA-256( 按模块键排序后逐行拼接 "<模块键>  <该份文件的 SHA-256>" )
```

判据：**同一二进制 + 同一棵树 → 同一 fingerprint**（跑两次一致）。实现是
`scripts/units_fingerprint.py`（`--list` 打每份的键与 SHA-256）；
`python scripts/build_meta_unit.py --check-twice` 一次跑两次并断言。2026-10-25 实测：

```
units=28 fingerprint=29ec2c2db1218cb1152b0d9ffd0fb04ca29e8537d57f6fca135aa74aaca26aed
整批指纹跑两次一致: True
```

### 7.3 每份单元里放什么

| 内容 | 说明 |
| --- | --- |
| 它自己定义的过程与数据 | 名字仍带模块前缀（`std_lex__…`、`std_lex__…_storage`） |
| 它用到的宿主服务声明 | 平坦名（`lain_meta_*`）；跨单元重复是允许的（verify 允许同名且 link_name 相等） |
| 它用到的**别的模块**的名字 | **不发**（第三节已实测：装载器接受未在本单元声明的跨单元引用；全批 2649 处），拆分是纯切片 |
| 平坦根名 | 只有 `@root` 标注的那几个，且只出现在定义它们的那一份里 |

### 7.4 必须报错的情形

| 情形 | 处理 |
| --- | --- |
| 同一个名字在多份单元里都被**定义** | 编译期发现（编译器知道全部定义）→ 报错；verify 也该有检查 |
| 两个模块各有一个同名 `@root` | 编译期报错 |
| `@root` 平坦名与某个 `<键>__<名字>` 相同 | 编译期报错 |
| 模块键里出现 `__` | 报错（沿用现有规则，保证前缀切分无歧义） |

### 7.5 落点（按迁移步）

| 步 | 文件 | 函数 / 位置 | 做什么 |
| --- | --- | --- | --- |
| 1 | `seed/lain0/tools/main.cpp` | 选项解析 | 加 `--entry` / `--split <dir>`，与 `--unit` 互斥 |
| 1 | `seed/lain0/include/lain0/Driver.hpp`、`src/Driver.cpp` | `compileUnit` 一族 | 新增「按入口求闭包 + 逐模块产出」的入口 |
| 1 | `seed/lain0/src/Lower.cpp` | `run`（模块循环） | 每模块一个 builder，各出一份文本 |
| 1 | `seed/kernel/src/text/print.c` | 模块打印 | 不变（改成按模块调用一次） |
| 1 | `seed/meta_driver.c` | `--unit` 装载段 | 加 `--units-dir`：装载目录内所有 `.l1`，按文件名排序 |
| 2 | `seed/lain0/src/Lexer.cpp`、`include/lain0/Lexer.hpp` | `Tok` / 扫描 | 加 `@` 记号 |
| 2 | `seed/lain0/src/Parser.cpp` | 顶层声明 | 读一行或多行 `@root`，挂到后面的声明上 |
| 2 | `seed/lain0/src/Sema.cpp`、`Sema.hpp` | 顶层登记 | 记「根名」标志 + 7.4 的三条防撞检查 |
| 2 | `seed/lain0/src/LowerDecl.cpp` | 过程名 / 数据符号 | 带标注的发改平坦名 |
| 3 | `bootstrap/meta.lain` | 三个入口 | 加 `@root` |
| 3 | `scripts/build_meta_unit.py` | 全部 | 调 `--entry … --split …`，模块集合来自入口的 import 闭包 |

## 八、风险与未定

| 项 | 说明 |
| --- | --- |
| **装载器行为**（第三节） | **已实测：接受未在本单元声明的跨单元引用 → 编译器不发外部声明**（复现命令在第三节）。剩下的是 verify 对「多份产出里同名重复定义」的检查（见下一行） |
| **重复定义** | 合并后每个名字必须**只有一处定义**；编译器要保证，verify 该有检查 |
| 记录用整批指纹 | `04-bootstrap-dialect.md`、`plan-selfhost.md` 里的记录都按整批指纹写 |
| 小编译形式 | 编一个样例的产物是"样例 + 它 import 的模块"多份，命令与脚本按多份写 |
| **接口文件（分开分析）** | 明确**不在本提案内**，另作一件事 |
| Meta 侧 | 拆分产出与 `@root` 都要在 Meta 里同样实现；两边一致由第 4 步的差分测试守 |

## 九、与既有决策的关系

本提案的内容记在 **D-004**：模块键与前缀、`@root` 平坦导出、每模块各出一份 + 由入口
推导模块集合 + 装载器按名合并、跨模块一律 `别名.名字`。
