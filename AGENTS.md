# Repository Guidelines

Lain 是一个原生语言实验（版本见 `VERSION`，当前 `0.1.0-alpha.1`）。核心问题：编译器能否只保留稳定的**物理底座**，把函数、类型、模块、泛型、effect 等高层语言规则放进**可替换、可自举的 Meta 层**。

三层组成：**LAINIR**（结构化物理 IR）、**编译期执行**（`#eval`）、**Meta 函数**（自举前端，RawAst → LAINIR）。

**这份文件按 `HEAD` 的树实测写**：目录、命令、约定都对着当前仓库核过。跟别处文档对不上时，以树和 `python scripts/build.py` 的输出为准。

---

## 1 仓库现状

| 路径 | 文件数 | 是什么 |
| --- | --- | --- |
| `seed/` | 125 | **C 最小运行时**：LAINIR 的 parse / verify / load / engine、能力表、VSpace。`src/{core,vm,backend,fold,meta}` + `include/` + `tests/` |
| `bootstrap/` | 16 | **手写 LAINIR 的初代 Meta**（`.l1`）+ 临时标准库。**当前主战场** |
| `archive/` | 136 | **归档**。只读参考，不进构建闭包 |
| `docs/` | 11 | 设计文档 + 当前计划 |
| `scripts/` | 1 | 只有 `build.py` |
| `build/` | — | 所有生成物（**永不提交**，见 `.gitignore`） |

根文件：`VERSION`、`README.md`、`LICENSE`、`.gitignore`、`.gitattributes`、`AGENTS.md`、`build.py`。

`archive/` **是归档**：只读参考，不进构建闭包，不要在上面加东西。

**不存在的东西**（照着旧文档干活会撞墙）：`src/`、`std/`、`seed/build.zig`、`docs/roadmaps/`、`docs/history/`，以及任何 `check_*.py` / `build_seed.py` / `run_lainir_self_host.py` / `build_lain_compiler.py`。旧的校验脚本**不在版本库里**，历史被重写过，也恢复不出来。

---

## 2 两个底座，别混

**`seed/`（C）** 只懂物理 IR：解析、验证、装载、执行、能力表、地址授权。它**不判断任何语言规则**——`let` 是什么、`i32` 是什么类型，它都不知道。

**`bootstrap/`（手写 LAINIR）** 才是「知道哪些词是什么意思」的那一层。它存在的唯一理由是**打破鸡生蛋**：Meta 必须能用 Lain 写、必须能被整份替换，但要用 Lain 写 Meta 先得有个 Lain 编译器；所以第一代 Meta 直接用物理 IR 手写——那是唯一不需要编译器就能被 seed 执行的底座。等它能编译 Lain 源码，就用它把 Meta 换成 Lain 版，然后丢掉它。

- v0 语言的全貌、两层分工、宿主能力表写在 **`bootstrap/README.md`**（当前最权威的一份说明，先读它）。
- `bootstrap/SOURCE_ORDER` 决定 `.l1` 的拼接顺序；同一份清单必须给出**逐字节相同**的模块。
- 宿主能力只有 **11 个**（`lain_meta_source_{count,data,length,path_data}`、`lain_meta_emit_{reset,write,data,length}`、`lain_meta_scratch_{data,size}`、`lain_meta_fail`），名字同时是 link_name，所以必须是合法 C 标识符。没有全局状态：host 地址是**显式输入**。
- 地址**必须显式授权**：Meta 的 TCB 不自带地址空间，源码文本也不在它的映像里；不授权第一次 `#load` 就是 trap 1004。这是设计要的行为。

**不得把这两层当成同一份实现。语言语义不得通过改 `seed/` 偷渡**——只有 LAINIR/VM 指令语义或宿主能力确实缺失时才动 C。

---

## 3 怎么建、怎么跑、怎么验

**构建 + 冒烟（唯一的入口）：**

```text
python scripts/build.py
```

自包含、无参数：用本地 C 编译器（`zig cc` → `clang` → `gcc`，自动挑）把 `seed/src/**/*.c` + `seed/tests/meta_boot.c` 编成 `build/meta_boot.exe`，设好 `LAIN_META_MANIFEST` 与 prelude，再跑 `seed/tests/meta_for.lain`（`main = 10`）冒烟。退出码 **0 通过 / 1 失败 / 2 缺前置**。

**跑单条用例：**

```text
build/meta_boot.exe <源码> <入口> <期望值> [断言文本] [模式] [逻辑路径=文件]...
```

- 模式：`-`（默认；期望值是真断言）、`tree`（按每份源码自己的缓冲印语法树）、`types`（列类型注册表）、`canonical`（规范 LAINIR 文本）、`c`（出 C，不执行）。
- **`LAIN_META_MANIFEST` 手敲时必须自己设成 `bootstrap/SOURCE_ORDER`。** 不设会退回已失效的 `seed/bootstrap/SOURCE_ORDER`，每条都报 `cannot load the Meta sources` —— 那跟被测代码毫无关系。（`scripts/build.py` 会替你设。）
- prelude 要当模块显式传：`std/prelude.lain=bootstrap/lain/std/prelude.lain`。import 的语料在 `seed/tests/modules/**`（逻辑路径 = 相对该目录的路径）。
- 期望值要**拿错值对照一次**：同一个 `1 + 2 + 3` 传 `6` 是 `ALL PASS`，传 `99` 是 `FAIL`。不做这个对照，「跑过了」可能只是断言没生效。

**改动前后的回归验收**（当前约定写在 `docs/implementation/meta-parser-plan.md` §6）：

```text
git worktree add --detach build/oldwt <改动前的提交>
python build/oldwt/scripts/build.py
# 两边对 seed/tests/**/*.lain（60 个）各跑一遍，逐条比 stdout 和退出码
# 只滤掉 bootstrap: / caps: / meta: / scratch: 四行计数（它们必然随代码量变）
git worktree remove --force build/oldwt
```

**没有提交在版本库里的回归脚本**，要用就现写（`build/` 不进版本库）。注意这条验收的边界：「全语料一致」只证明**没破坏已有行为**，**不证明新行为对**——60 个用例里没有一条覆盖到的路径，静默 bug 就是这么活下来的。新能力必须另配正例。

**CI 不可信**：`.github/workflows/lain-bootstrap.yml` 仍指向 5 个已删除的脚本（`build_seed.py`、`check_lainvm_boundary.py`、`check_eval_tcb.py`、`check_lainir_physical_safety.py`、`run_lainir_self_host.py`），**当前不反映这棵树**（`origin/master` 上就已经没有 `scripts/` 目录）。别把它当门禁。

**两个 `build.py`，只有一个能用：**

- `scripts/build.py` —— **活的入口**（上面那条）。
- 根 `build.py` —— **坏的历史遗留**：它是从 `scripts/` 抄出来落错地方的，`ROOT = Path(__file__).resolve().parents[1]` 指向仓库**外面**，也不设清单，跑起来只会报 `缺源文件`。不要用它，也不要照着它找入口。

**生成物只落 `build/`**，永不写进 `seed/`、`bootstrap/`、`docs/`。`build/seed/bin/*` 是旧状态留下的可执行文件，当前树里没有它们的源码。

**换行**：`.gitattributes` 强制 `*.l1` 和 `*.lain` 为 `eol=lf`（`.md` 不在内）。这两类文件是逐字节比较的对象，CRLF 会污染对比。

---

## 4 手写 LAINIR 的约定（新方言，与旧塔不同）

- **绑定是单赋值**：`%x = #op[...](...)`，不能重新赋值。循环状态只能靠 `#loop` 的携带值 + `#continue` 传递（`#break` 只给**结果**值）。
- **类型四种**：`#bits<N>`、`#f<N>`、`#vec<N>`、`#addr`（**没有** `#unit` / `#never` / `#simd`）。
- **算子扁平、类型实参必写**：`#add[#bits<32>](%a, %b)`、`#sdiv`/`#udiv`、`#slt`/`#ult`——没有 `#div`/`#lt`/`#field`/`#primitive`。
- **`#extern` 写法**：`#proc name(...) #extern "link_name"`。
- `#break` / `#continue` **带括号**，`#yield` / `#return` 不带。
- **诊断码**：**0 = 成功，非 0 = 稳定诊断码**。Meta 侧见过/实测过：3 缺操作数、4 值后面有垃圾、6 类型的 op 表里没这个算子、9 投了声明里没有的变体、10 投影无载荷的变体、11 模块找不到、12 不是 import 绑定、14 标量只能写在模块里、16 同层重名、17 暂存区装不下、20 函数体没有 return、21 语句缺 `;` 或局部变量缺类型标注、22 重复绑定；验证器另有 1004 地址未授权、2002 引用未定义的值、2019 返回类型不符。

**最容易再撞的三条**（完整十条在计划的 §7「坑清单」，都是实测撞过的）：

1. **静默错值是主要失败模式，不是崩溃。** 验证器拦得住「引用未定义的值 / 返回类型不符 / 越权访问」，拦不住**语义错**——一条合法但算错的指令会一路通过。本仓库明确禁止这种失败；对策是：先量、再改、把产物文本抄进提交信息、给新能力配正例。
2. **物理算子名放在暂存区 `+24`/`+32`**（`meta_op_resolve` 写、`meta_emit_op_mid` 读）。中间只要有一次递归就会把它换掉，于是发出一条**错的但合法**的指令（实测 `12 / (2 + 1)` 曾发成 `#add`，验证通过、算出来 15）。规矩：**先把会递归的东西全降级完，再查算子，从查完到发完之间不许再有递归。**
3. **暂存区布局的单一来源是 `bootstrap/std/scope.l1` 的文件头**（每一格归谁、动态区怎么排、碰撞式分配器的水位 `+200` / 峰值 `+208`）。节点步长 **48** 出现在四处（`parse.l1` 的两个访问器、`scope.l1` 的容量检查与 `meta_path_buffer`），改要一起改——容量检查那处就漏过一次（还是 40，比真实占用小 17%，「装不下报 17」判不出来）。

---

## 5 硬性约束（违反即错）

- 不得把 `seed/` 和 `bootstrap/` 当作同一份实现；不得为兼容旧源码保留旧方言写法。
- 语言语义不得通过改 `seed/` 偷渡；只有指令语义或宿主能力确实缺失才动 C。
- **不得用「改产物」让验收通过**：不许改 `build/` 里的东西、不许为了让某条检查过而改生成物。生成物只落 `build/`。
- 结论必须是**跑出来的**：数值、拒绝码、产物文本都要有可复现的命令。**文本搜索不能替代执行命令**——它只能证明旧拼写消失。
- 计划里标「**需要人定 / 必须先讨论**」的（`docs/implementation/meta-parser-plan.md` §4 与 §7），**停下**：给最小复现 + 两个方案，不要自己挑。
- **不要动 `seed/src/vm/**`**：VSpace 那个抽象「管不管分配」还是开着的停线项，定了才动。
- `bootstrap/` 与 `seed/` 里的注释、提交信息都用中文；提交信息带实测数字或产物文本。

---

## 6 文档地图

| 文件 | 状态 |
| --- | --- |
| `bootstrap/README.md` | **权威**：当前 v0 语言全貌、两层分工、宿主能力表、踩过的坑 |
| `docs/implementation/meta-parser-plan.md` | **当前唯一活跃的计划**：五步状态、§4 停线项、§7 下一步 + 十条坑 + 怎么验。换 session 先读它的 §0 |
| `docs/00-intro.md` … `04-lain-vm.md`、`docs/LAINIR.md`、`docs/lain-roadmap.md`、`docs/REFACTOR_PLAN.md`、`docs/stdlib/effect-system.md` | 设计文档与旧路线图。**多数描述的是被归档的那座塔，未随树更新**——读的时候留意里面的路径还在不在 |
| `README.md` | 讲三层是什么；它的「Project layout」与「Bootstrap status」**已过时**（列着 `src/`、`std/` 和已删除的脚本） |

**已知失效但仍留在树里的东西**（见到不要以为是现状）：`.github/workflows/lain-bootstrap.yml`、根 `build.py`、`seed/tests/meta_conformance/CASES`（里面的路径还写着 `seed/lain/std/prelude.lain`，且它的 runner 已不在；`reject_nested_expr` 那条期望也不再成立，嵌套表达式早就支持了）。
