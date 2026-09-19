# Repository Guidelines

Lain 是一个原生语言实验（版本见 `VERSION`，当前 `0.1.0-alpha.1`）。核心问题：编译器能否只保留稳定的物理底座，把函数、类型、模块、泛型、effect 等高层语言规则放进可替换、可自举的标准库 Meta 层。三层组成：**LAINIR**（结构化物理 IR）、**编译期执行**（`#eval`）、**Meta 函数**（自举前端，RawAst → LAINIR）。

## Architecture & Data Flow

官方流水线（`docs/00-intro.md`、`docs/README.md`）：

```text
Lain source -> RawAst -> Meta expand -> Meta elaborate -> Meta lower
            -> LAINIR -> verify + execute #eval -> runtime LAINIR
            -> interpreter / native backend
```

- **Parser 只建源码拓扑**：`tokenizer.lain` 不分关键字，`syntax.lain` 只存 Atom/Group、span、`origin`/`hygiene`。`let`、`std::func`、`import` 的含义全部由 Meta 决定。
- **Meta 是语言定义层**：负责宏/attribute 展开、名字/类型/effect 检查、comptime 传播、layout、closure conversion、生成 LAINIR。
- **LAINVM 只执行物理 IR**：拥有 TCB、VSpace、Trap、activation 生命周期。`src/lainir/api/` 只做 IR 的构造/验证/打印，**不得 import 或构造 VM**；需要执行的一方显式组合 `lainvm.Interpreter`。**`Eval` 是 LAINIR 的概念，不是 LAINVM 的**——LAINIR 定义「这段已 lowering 的代码在编译期执行」，LAINVM 只提供实现它所需的执行原语（`execute`/`execute_child`、VSpace、预算、Trap）。当前 `Eval` effect 与 `eval_handler` 仍住在 `src/lainvm/interpreter.lain`，属待迁移的过渡状态。
- **Backend 只消费已完成 lowering 的 LAINIR**：`#eval` 必须在 backend 产物前执行并消失。
- **能力（capability）注入而非全局状态**：`compiler.Compiler(Memory, Ir, Vm)` 是按编译期参数特化的工厂；平台 I/O 通过 `std::platform::api` 的 effect 提供（`src/lainc/compiler_driver.lain`）。调用链：`compiler_core.compile` → `Sources`/`Syntax.parse` → `Meta.expand` → `Elaborator.elaborate` → `Lower.program` → `Artifact.verify`/`write_canonical_text`。

**自举塔（三套实现共用一个契约，不可混为一体）：**

```text
seed/lainir-seed + bootstrap/compiler/*.l1 + bootstrap/std/*.l1
  -> build/bootstrap/lainc.l1
  -> 编译 src/lainc/*.lain
```

语言语义**不得**通过改 `seed/` 偷渡；只有 LAINIR/VM 指令语义或宿主能力确实缺失时才修改 C seed（`docs/roadmaps/lain-roadmap.md` §2）。

## Key Directories

| 路径 | 职责 |
| --- | --- |
| `seed/` | C 最小运行时：`core <- text <- interpreter <- host <- cli`；LAINIR parser/verifier/解释器 + 宿主能力。另有 LAINIR 写的 LAINIR→C 编译器 `seed/lainir/compiler.l1` |
| `bootstrap/` | 手写 LAINIR 文本（`.l1`）启动编译器与临时 stdlib；启动工具，非长期架构 |
| `src/lainc/` | 正式 Lain 编译器：`tokenizer` → `syntax` → `meta` → `elaborator` → `lower`；`backend_c.lain` 为 Lain 写的 C 后端 |
| `src/lainir/` | **只剩契约** `api_contract.lain`（provider 必须满足的接口）。第二版实现已归档：`docs/history/formal-implementations/` |
| `src/lainvm/` | **只剩契约** `api_contract.lain`（`ExecutionShape` / `Eval`）。Lain 写的 interpreter 已归档，见上 |
| `archive/` | 已归档的第一代实现：`archive/src/**`（C 运行时，14003 行）、`archive/{include,lainir}/**`、`archive/std/**`（绑在第一代宿主 ABI 上的 `meta.lain` 与 `abi_entry.lain`，2026-09-20 从 `std/` 移入）。**只读参考，不进构建闭包** |
| `std/` | 正式 Lain 标准库（`std/core/*`、`std/platform/*`、`std/effect.lain`、`std/type_policy.lain` …），不含任何 `@foreign`。其第一代 Meta ABI 的两份已移入 `archive/std/` |
| `scripts/` | 全部构建/校验/固定点驱动（Python，扁平目录，无包） |
| `docs/` | 设计规范、实现说明、当前路线图、历史归档 |
| `build/` | 所有 bundle/snapshot/报告/可执行产物；**永不提交** |

**形式实现已暂停**（2026-09-12）：`src/lainir` 的 provider 与 `src/lainvm` 的 interpreter 是用
Lain 写的第二版实现，能编译、通过 verifier，但没有真正的执行路径（详见
`docs/history/formal-implementations/README.md`）。它们已移出构建闭包，等 Lain 成熟后用 Lain
重写。**契约保留**——`src/lainc` 只用契约里的类型，因此归档实现不影响它编译。
C seed 与 bootstrap 各有独立实现，**不受影响**（`seed/` 的 `lainir_*` 函数名与 `bootstrap/`
的 `lainvm_*` 函数名只是命名巧合）。

## Development Commands

**前置：编译 seed（Zig 0.16.0）。** 产物在 `build/seed/bin/`（被 gitignore）。

```text
python scripts/build_seed.py   # 产出 build/seed/bin/{lainir-print,lainir-seed,lainir-vm-control-test}
```

**生成编译产物：**

```text
python scripts/build_lain_compiler.py     # build/bootstrap/{lainc,compiler_core,stdlib}.l1 + stamp/manifest
python scripts/freeze_lainc_bootstrap.py  # 追加 build/bootstrap/lainc.l1.snapshot.json
python scripts/build_formal_stdlib.py     # build/lainir/formal_stdlib.l1
python scripts/build_srclainc.py          # build/lainir/srclainc.l1
python scripts/build_default_lainc.py     # build/lainc.exe + build/lainc-native.c
```

**编译/运行：**

```text
python scripts/run_lain_compiler.py -o OUT [--library] [--stdlib-artifact P] src...
python scripts/run_lain_backend.py <canonical.l1> -o out.c
```

**测试（见 Testing & QA）：**

```text
python scripts/check_lainc_lainir_api_baseline.py   # 36 道 gate 顺序执行器
python scripts/capture_lain_bootstrap_baseline.py [--full]
```

**快照校验（README 记录）：**

```text
python scripts/check_lainc_bootstrap_snapshot.py build/bootstrap/lainc.l1
```

无 Makefile、无 pyproject、无包管理器。

## Code Conventions & Common Patterns

**声明语法（唯一合法形式，README「Bootstrap status」）：**

```lain
let NAME [: EXPECTED] = INITIALIZER
```

构造器 `std::func` / `std::struct` / `std::module` / `import("path")`；`@` 只引入 attribute（`@export`、`@foreign`、`@abi_export`、`@compiler_intrinsic`），不是第二套声明语法。历史上的裸 `fn`/`struct`/`module`/`import` 声明一律拒绝；`src/lainc/diagnostics.lain` 的 12001/12003/12004/12005 强制该顶层形式。

**命名：**

- 模块值绑定 `snake_case`，与 import 路径尾段一致：`let memory_model: Module = import("std::memory_model")`。
- 类型、类型/模块参数 `PascalCase`：`TypeId`、`CompileResult`、`Memory`、`Ir`、`Vm`、`T`。
- 函数、局部、字段 `snake_case`：`lower_expression`、`source_id`。
- 枚举/策略常量 `PascalCase`：`CheckedStrategy`、`Ok`、`Pure`。
- 外部链接名点分/短横线 C ABI：`bootstrap.source-data`、`backend.artifact_begin`。

**模块逻辑路径（`import` 解析）：** `std::X::Y` → `std/X/Y.lain`；`packages::lain::compiler::X` → `src/lainc/X.lain`；`packages::lain::lainir::api::X` → `src/lainir/api/X.lain`；`packages::lain::lainvm::X` → `src/lainvm/X.lain`。
解析实现在 `bootstrap/compiler/workspace.l1`：`std::` 走**精确路径**匹配（`workspace_std_path_matches`）；其余全部走**末段匹配**（`workspace_suffix_matches`），即 `packages::lain::...` 前缀**不参与比较**，只比较 import 串最后一个 `::` 之后的段与源文件路径的最后一段。因此闭包内非 std 源文件的 basename 必须唯一。详见 `docs/implementation/module-namespaces.md`。
已知冲突：`src/lainir/api_contract.lain` 与 `src/lainvm/api_contract.lain` 同名，两侧都走末段匹配 → `import("packages::lain::lainvm::api_contract")` 可能绑定到 LAINIR 契约。属路线图 §15「必须先讨论」项。

**模块与能力工厂：**

```lain
let Vec = std::func(T: std::type, Allocation: Shape, Bounds: Shape) -> std::type { ... };
```

Import 返回命名空间值，禁止非限定注入；结构约束用 `std::module_shape`；返回类型用 `std::type_with_namespace(...)`；effect 用 `std::effect(...)` / `std::effect_operation(...)` / `std::handler(Effect) {...}`。

**Effect 与错误：**

- 签名 `-> T ! {Effect1, Effect2}`，operation 用 `perform`，handler 用 `std::handler` + `resume`。
- 预期失败：`effects.Throws(E)` + `effects.throw(...)`；致命失败：`effects.Trap` + `effects.trap()`；值携带的失败用 `std/core/result.lain`。
- **无 `async` 关键字**：异步 = `Suspend` effect + VM `Endpoint`/scheduler。
- **effect 实现策略（TCB / CPS）是编译期决策**，由 handler 决定：`Remaining` 槽记录「本 handler 还需要什么」，控制原语露在 `Remaining` 里就是可见的。LAINVM 只实现 TCB 一条路径；CPS 路径变换后不涉及任何 VM 操作。现有全部 handler 的 `resume` 都在尾位置或根本不 `resume`，故 `suspend_tcb`/`resume_tcb`/`run_slice`/Endpoint 目前无调用者——那是「消费者未出现」，不是「实现缺失」。详见 `docs/stdlib/effect-system.md`。
- 诊断码约定：**0 = 成功，非 0 = 稳定诊断码**；结构化诊断见 `std/diagnostic.lain` 与 `compiler_context.Diagnostic{code,message,source_id,start,end}`。

**Meta：** 生成节点必须带 `origin`/`hygiene`（`generated_syntax.lain`）；phase 显式编号（meta=1/comptime=2/runtime=3）；Meta 值对编译器是不透明 handle。跨层只允许通过 `Vm.eval`。

**DI / 状态：** 无全局可变状态；能力按编译期 Module 参数注入；状态以 `&State` / `&mut State` 线程传递（`std/core/arena.lain`、`std/platform/memory.lain`、`compiler_context.lain`）。

**LAINIR 风格：** 位宽/符号显式——用 `#sdiv`/`#udiv`、`#slt`/`#ult`、`#zext`/`#sext`/`#trunc`；没有 `#div`/`#lt`/`#field`/`#primitive`。物理类型仅 `#bits<N>`、`#float`、`#addr`、`#unit`、`#never`。

**LAINIR 书写陷阱（bootstrap 编译器，2026-09-12 实测）**：**给参数赋值不会跨 `#continue` 保留。**
若把循环游标写成被赋值的过程参数——

```lainir
#proc f(#addr %node) -> #bits<1> {
  #loop siblings {
    #if #call raw_is_nil(%node) { #return 0 }
    ...
    %node: #addr = #call raw_node_next(%node)   // ← 不生效
    #continue siblings
  }
}
```

游标**不前进**，循环永不终止或反复读同一节点。必须改用局部变量：

```lainir
#proc f(#addr %node) -> #bits<1> {
  #let %cursor: #addr = %node
  #loop siblings {
    ...
    %cursor: #addr = #call raw_node_next(%cursor)
    #continue siblings
  }
}
```

该缺陷曾在 `lainvm_meta_type_mentions` 中潜伏：它的注解都是**单节点**，首次迭代即命中返回，
所以从未暴露；一旦扫描 `Vec(T)` 这类多节点类型表达式就会立刻挂死。
**审计结论**：用「参数名在自身 proc 体内被重新赋值」这一模式扫过 `bootstrap/compiler/*.l1`，
修复前恰有 1 处（即上述那处），修复后为 0。

## Important Files

| 文件 | 用途 |
| --- | --- |
| `README.md` / `VERSION` | 概览、设计原则、布局、声明语法、bootstrap 状态、版本 |
| `docs/00-intro.md` … `docs/04-lain-vm.md` | 语言总览 / LAINIR 规范 / LAIN-AST 契约 / Meta 系统 / LAINVM 设计 |
| `docs/roadmaps/lain-roadmap.md` | **当前唯一有效路线图**（基线 2026-09-11；**2026-09-20 调整**：载体为第二代、四项可执行证明 P1–P4、里程碑 M0–M5，见其 §5）：术语、实现归属、不可违反的边界、编码 1–7 的边界原则与验收命令 |
| `docs/implementation/architecture-review-2026-09-20.md` | 架构评审与路线合并结论：两套 Meta ABI 的实测对照、证据分级（R/D/A/U）、四项证明的验收设计、`std/` 归属决定、差异台账。路线图 §5 的依据 |
| `docs/implementation/architecture-review-astra-2026-09-20.md` | 同一轮的外部评审（未运行任何命令，读代码得出）；合并版的差异台账逐条对照它 |
| `src/lainc/compiler_core.lain` | 无平台依赖的编译流水线 |
| `src/lainc/compiler_api.lain`、`compiler.lain` | 公开 API 与 `Compiler(Memory, Ir, Vm)` 工厂 |
| `src/lainc/meta.lain`、`elaborator.lain`、`lower.lain` | Meta 展开 / 语义处理 / lowering 引擎 |
| `src/lainir/api_contract.lain` | 编译器 ↔ provider 的能力契约 |
| `src/lainvm/interpreter.lain` | 纯 Lain VM 求值器与 `execute_child` |
| `archive/std/meta.lain` | **第一代** Meta ABI（178 个 `@abi_export`、61 个 `@foreign`，全部指向第一代宿主能力名）。2026-09-20 从 `std/` 移入 `archive/`：第二代宿主只有 11 个能力，两边交集为 0，属两套 ABI |
| `src/lainc/COMPILER_SOURCES.txt`、`src/lainir/api/SOURCES.txt`、`src/lainvm/SOURCES.txt` | 源码闭包清单（语义顺序） |
| `archive/lainir/compiler_parts/SOURCE_ORDER`、`src/lainc/LAIN_SOURCE_ORDER` | 分段顺序，必须能逐字节重建原文件 |
| `scripts/lainc_sources.py`、`scripts/backend_manifest.py` | 共享闭包解析 / 确定性 manifest 生成 |
| `docs/implementation/module-namespaces.md` | `packages::lain::` 逻辑命名空间、import 解析规则与 basename 歧义风险 |

## Runtime / Tooling Preferences

- **Zig 0.16.0**（`seed/build.zig.zon` 的 `minimum_zig_version`；CI 固定 0.16.0）。仓库根没有顶层 `build.zig`；`seed/build.zig` 由 `scripts/build_seed.py` 驱动。
- **Python 3.12**（CI；本地 3.11 兼容），仅标准库，唯一可选依赖是 `capture_lain_bootstrap_baseline.py` 的 `psutil`。
- **无 Node/Bun/Deno、无包管理器**（无 `package.json`/`pyproject.toml`/`pixi.toml`/`Cargo.toml`/`Makefile`）。
- Python 层中只有 `scripts/build_seed.py` 调用 `zig build`（固定输出前缀与缓存目录）；其余脚本需要本地 C 编译器时直接调用 `zig cc`（`build_lainc_native.py`、`run_lainir_self_host.py` 等）。
- 脚本一律 `ROOT = Path(__file__).resolve().parents[1]` 定位仓库根；可执行文件路径**必须**经 `scripts/toolchain.py` 的 `seed_exe()` / `selfhost_exe()` 取得，不要在脚本里硬编码 `seed/zig-out`。
- Zig 构建的唯一入口是 `python scripts/build_seed.py`（内部设安装前缀并固定 `ZIG_LOCAL/GLOBAL_CACHE_DIR=build/zig-cache{,-global}`），取代原先的 `cd seed && zig build`；`seed/` 本身不再产生任何生成物。
- `.gitattributes` 强制 `src/lainc/lainc.lain` 为 `eol=lf`——CRLF 会污染 gen2 输出。`.pre-commit-config.yaml` 为空文件，无钩子。
- 生成物只落 `build/`（seed 二进制在 `build/seed/bin/`，自举编译器在 `build/selfhost/bin/`）；**永不**写进 `src/`、`std/`、`bootstrap/`、`seed/`。

## Testing & QA

**没有测试框架**：无 pytest/tox/Makefile/conftest。"测试"就是独立校验脚本，逐个运行，退出码约定 **0 = 通过，1 = 失败，2 = 前置产物缺失**。

**类别：**

1. `scripts/check_*.py`（54 个）——契约/一致性/固定点门禁；其中 36 个进主入口。
2. 构建门禁脚本：`build_lain_compiler.py`（内含边界检查）、`build_formal_stdlib.py`、`build_srclainc.py`、`freeze_lainc_bootstrap.py`。
3. `seed/src/cli/vm_control_test.c` → `lainir-vm-control-test`（C 控制面自检）。
4. 自举/固定点：`run_lainir_self_host.py`（gen1→gen2→gen3，要求 gen2 C == gen3 C）、`prove_lainc_fixed_point.py`（规范文本/extern/过程标签/每过程体 SHA-256）。
5. Fixtures：`scripts/fixtures/`（97 个 `.lain`、21 个 `.l1`、1 个历史 C 基线）+ `lainc_api_migration/` 的 3 个 canonical `.l1` 快照。
6. seed 手写 Meta 的端到端回归：`scripts/check_seed_meta_bootstrap.py`（84 条，跑 `seed/bootstrap/*.l1`
   降级出的 LAINIR，并真的链成可执行去跑）。需要本地 C 编译器，默认 `zig cc`，可用 `--cc` 覆盖；
   产物落 `build/tmp-probe/`（`seed/tests/emit_c.c` 的路径写死在那里）。**不在 CI 里。**
7. 架构命题验收：`scripts/check_seed_meta.py --suite {mainline,canonical,replaceability}`——
   路线图 §5.1 的 P1「Meta 可替换」。语料在 `seed/tests/meta_conformance/`
   （`CASES` + 期望规范文本 + `meta_min.l1` 这份**独立**的极简第二 Meta）。
   `canonical` 档比较规范化 LAINIR 文本并验可复现；`replaceability` 档只换 Meta 清单、
   不重编驱动。驱动侧入口：环境变量 `LAIN_META_MANIFEST`（默认 `seed/bootstrap/SOURCE_ORDER`）
   与模式 `canonical`。**不在 CI 里。**

**执行顺序依赖：**

```text
python scripts/build_seed.py             # 先决：所有运行时检查
python scripts/build_lain_compiler.py    # 先决：build/bootstrap/* 相关检查
python scripts/build_formal_stdlib.py    # 先决：conformance/policy/provider 检查
python scripts/check_lainc_lainir_api_baseline.py   # 36 道 gate 编排入口
python scripts/capture_lain_bootstrap_baseline.py   # 基线报告 build/baselines/<ts>/
```

**CI（`.github/workflows/lain-bootstrap.yml`，唯一 workflow）**：push main/master 与 PR，ubuntu-latest、Python 3.12、Zig 0.16.0、30 分钟超时。9 步：`build_seed.py` → `check_lainvm_boundary.py` → `check_eval_tcb.py` → `check_lainir_physical_safety.py` → `build/seed/bin/lainir-vm-control-test` → `run_lainir_self_host.py`（外加 checkout 与两个 setup）。**CI 只跑 4 个检查，不构建 bootstrap bundle、不跑固定点，也不跑 `check_lainir_compiler.py`。**

**当前预期的不可用项**（README「Bootstrap status」、`docs/roadmaps/lain-roadmap.md` §3.2）：**重建已可用**（bootstrap 源可重新生成 `build/bootstrap/lainc.l1`），但 **native 编译器矩阵与 gen2/gen3 固定点仍未完成**——`srclainc.l1` 是库产物、缺可供 seed 调用的 `compiler_compile` 入口，两次构建一致**不等于**编译器固定点。

**验收标准：** 每个路线图阶段必须以可执行检查、contract test 或固定点比较作为完成条件（`docs/roadmaps/README.md`）；文本搜索只能证明旧拼写消失，不能替代执行命令。

## 贡献者硬性约束（违反即错）

- 不得把 `seed/`、`bootstrap/`、`src/` 当作同一份实现；不得为兼容旧源码保留裸 `type`、`comptime` 或旧 generic policy。
- `std::type` 是标准根环境中的 Meta 值，不是关键字，也不是 `import("std::type")` 的 Module；不得让两种含义共存。
- 不允许 `generic` 专用 Parser 节点，不允许按函数返回类型选择 type/module/AST factory 协议。
- 不得恢复 `EvalResult`、object kind、owner、generation、sidecar 等 Meta 包装；不得按 Meta 返回类别选择 VM 执行协议。
- 不得隐式捕获未声明的环境输入（函数用到的输入必须出现在自己的 `?{}` 行）。
- 修改 `build/**/*.l1` 以通过构建是禁止做法；不得修改 `docs/history/`。
- 遇到路线图 §15「必须先讨论」的情形（如 `?{}` 需要运行时动态输入、Meta 值无法作为物理 bits/addr/unit 进入 LAINVM、固定点入口需要超出 ABI 的宿主能力），停止实现并提供最小复现 + 两个方案。
