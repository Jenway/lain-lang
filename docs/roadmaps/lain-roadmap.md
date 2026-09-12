# Lain 编译器执行路线图

基线日期：2026-09-11。

本文是当前开发工作的执行规格。读者可以假定自己第一次接触仓库，但不能仅凭本文猜测
新的语言语义：遇到本文“必须讨论”的问题时，应停止相关实现并询问项目负责人。

本文只保留当前基线和后续工作。完成一个阶段后，应把完成证据移出本文，并更新这里的
当前基线与下一个阶段。

## 1. 术语

- **RawAst**：Parser 输出的无语义树。它只包含 Atom 和 Group；Atom 是一段 token，Group
  是由 `()`、`{}` 等分隔符包围的子节点序列。
- **Meta**：读取 RawAst、决定 Lain 源码含义并生成 LAINIR 的语言定义层。
- **Meta 值**：只在编译期间有意义的值，例如类型、模块、AST handle 和 effect 描述。
- **callable**：可调用的 Meta 值。`std::func` 构造 callable；普通函数和返回类型的函数
  使用同一种 callable 表示。
- **输入行 `?{}`**：函数要求调用环境提供的一组具名 Meta 输入。
- **输出 effect 行 `!{}`**：函数执行期间可能发出的一组 operation。
- **LAINIR**：只描述确定物理类型和物理操作的中间表示。
- **LAINVM**：执行已验证 LAINIR 的虚拟机语义层。
- **TCB**：一条 LAINVM 执行流的状态，包括当前位置、调用栈、预算和 Trap 状态。
- **VSpace**：TCB 可访问的虚拟地址空间。
- **固定点**：由一代编译器生成下一代编译器后，连续两代的规范化产物完全一致。

## 2. 仓库中的三套代码

执行者必须先区分以下目录，不能把它们当作同一份实现：

| 路径 | 当前职责 | 修改时机 |
| --- | --- | --- |
| `seed/` | C 实现的最小 LAINIR parser、verifier、解释器和宿主能力 | 仅在物理 VM/API 缺能力时修改 |
| `bootstrap/` | 手写 LAINIR 的启动 Lain 编译器和最小标准 Meta ABI | 新语言语义必须先在这里可执行 |
| `src/` | 用 Lain 编写的 lainc（正式编译器），以及 LAINIR/LAINVM 的**契约** | bootstrap 能编译新语义后同步实现 |
| `std/` | 用 Lain 编写的标准库与标准 Meta 定义 | bootstrap 支持新形式后迁移 |
| `scripts/` | 构建、测试和固定点驱动 | 每个阶段增加真实验收入口 |
| `build/` | 所有 bundle、snapshot、报告和 executable | 永远不作为源码提交 |

`src/lainir` 与 `src/lainvm` 的**实现**目前已暂停并归档到
`docs/history/formal-implementations/`（见 §2 下方说明），因此「用 Lain 编写的正式 LAINIR、
LAINVM」这一项当前只由契约代表。

正式实现的边界为：

- `src/lainir/` **只剩契约** `api_contract.lain`（provider 必须满足的接口）；
- `src/lainvm/` **只剩契约** `api_contract.lain`（`ExecutionShape` / `Eval`）；
- `src/lainc/` 解析、执行 Meta、检查源语言并生成 LAINIR。

**形式实现已暂停**（2026-09-12）：`src/lainir` 的第二版 provider（`l1_ir`、`l1_unit_builder`、
`l1_verifier`、`l1_printer`、`default_provider`）与 `src/lainvm/interpreter.lain` 已移到
`docs/history/formal-implementations/`，等 Lain 成熟后用 Lain 重写。

原因（实测）：它们能编译、通过 verifier，但没有真正的执行路径——LAINIR provider 只被调用过
一个返回常量的成员函数，LAINVM interpreter **零执行**。契约保留，是因为 `src/lainc` 只用契约
里的类型，把它当参数接收；因此归档不影响 `build_srclainc.py`（仍退出 0）。

**不影响 C seed 与 bootstrap**：它们各有独立实现（`seed/src/interpreter/`、
`bootstrap/compiler/`），`seed/` 的 `lainir_*` 与 `bootstrap/` 的 `lainvm_*` 只是命名巧合。

语言语义不能通过修改 `seed/` 偷渡进编译器。只有 LAINIR 指令语义、VM 控制语义或宿主
能力确实不足时，才允许修改 C seed。

## 3. 当前代码基线

### 3.1 已经可用的执行能力

- C seed 能验证并执行 LAINIR。
- `#eval` 使用临时 TCB，默认共享调用者 VSpace；普通结果是 `#bits<N>`、`#addr` 或
  `#unit`，失败产生 Trap。
- `#eval` 在最终 backend 产物前执行并消失。
- `src/lainvm/api_contract.lain` 定义 Artifact、Procedure、Value、ValueVector 和 `Eval`
  operation；lainc 不导入任何 VM 实现。其中 `Eval` 的归属已裁定为 LAINIR（§4.4），迁移尚未
  执行。
- bootstrap 已能通过 LAINVM 路径执行整数表达式、标量 Meta 调用、effect 构造、effect
  operation 构造和 module factory。
- `scripts/build_srclainc.py` 能生成可验证的 `build/lainir/srclainc.l1`；
  `scripts/check_srclainc_artifact.py` 能证明两次独立生成的源码闭包产物一致。

### 3.2 当前不能宣称完成的内容

- `srclainc.l1` 目前是库产物，没有可供 seed 调用的 `compiler_compile` 入口；两次构建
  一致不等于编译器固定点。
- `scripts/run_lainir_self_host.py` 自举的是 LAINIR 编译器，不能用作 Lain 编译器固定点
  证据。
- `scripts/profile_lainc_bootstrap.py` 仍假定旧的单文件输入和 `compiler_compile` 入口，不能
  原样用作当前固定点驱动。
- `scripts/check_lainvm_boundary.py` 主要检查源码和 API 形状，不能证明正式 lainc 的
  `Vm.eval` 已经由真实 handler 执行。
- `bootstrap/compiler/meta_call.l1` 当前按 scalar、module 和 effect 返回类别选择不同路径；
  这只是过渡实现，不是目标架构。
- `std::func` 还没有解释 `?{}` 输入行。
- 当前裸 `type`、generic policy、`ComptimeValue` 分类和 compiler-owned specialization
  仍散布于 `std/`、`bootstrap/` 与 `src/lainc/`。
- `Eval` 目前定义在 `src/lainvm/`，但它是 LAINIR 的概念。迁移前 LAINIR 与 LAINVM 在契约
  层面仍然混着（见 §4.4）。
- C seed 的编译期求值使用「传源码 + fold」模型（`bootstrap.eval_source`，宿主重新 parse、
  verify 并原地折叠整个模块，返回值只是状态码，结果经 `bootstrap.eval-next` 侧信道取回，
  而该 capability 目前没有调用者）；lainc 使用「执行已验证 IR」模型
  （`Vm.eval(unit, procedure, arguments)` 返回物理 `Value`）。两者统一之前，编译期执行
  没有单一语义。
- Lain 实现中的 `suspend_tcb`、`resume_tcb`、`run_slice` 与 Endpoint 都没有调用者。这不是
  "缺实现"：现有全部 handler 的 `resume` 都在尾位置或根本不 `resume`，挂起机制的消费者
  尚不存在。见 [`../stdlib/effect-system.md`](../stdlib/effect-system.md)。

### 3.3 编码 0：已完成（2026-09-12）

§6.2 名称冲突解除和 §6.3 类型宇宙绑定都已完成，并已通过 §6.5 的全部验收：

| 验收项 | 结果 |
| --- | --- |
| `python scripts/check_std_type.py` | 退出码 0，`PASS std::type binding and bare type rejection` |
| `python scripts/check_bootstrap_consteval.py` | 退出码 0 |
| `python scripts/build_formal_stdlib.py` | 退出码 0 |
| `python scripts/build_srclainc.py` | 退出码 0 |
| 搜索 `import("std::type")` | 无结果 |
| 搜索裸 `type` 标注（`std/`、`src/`） | 无结果 |

反例 `scripts/fixtures/formal_bare_type_rejected.lain` 有意保留裸 `type`，由
`scripts/check_std_type.py` 验证它必须被拒绝，因此它不计入"无结果"。

工作树中尚未提交、属于本阶段的文件：

```text
bootstrap/compiler/meta_bindings.l1
bootstrap/compiler/meta_type.l1
bootstrap/compiler/lower_program.l1
scripts/check_std_type.py
scripts/fixtures/formal_std_type_value.lain
scripts/fixtures/formal_bare_type_rejected.lain
```

这些改动已经过验收，可以按 §6.5 的切片提交；不要覆盖它们。

### 3.4 已修复的陈旧断言

编码 0 的 `std::type` 迁移改了源码拼写，但漏改了按字符串匹配源码的 gate。以下三处已修正，
改动前它们使对应检查恒失败：

- `scripts/check_lainvm_boundary.py`：`"let Flow: type"` → `"let Flow: std::type"`；
- `scripts/check_lainc_lainir_api.py`：`"let SourceResult: type"` → `"let SourceResult: std::type"`；
- `scripts/check_stdlib_swap.py`：三处源码清单中的 `std/type.lain` → `std/type_policy.lain`。

教训：这类 gate 用字面量匹配源码，**改名时必须同步搜索 gate**。新增此类断言前，先确认它
检查的是行为还是拼写。

## 4. 不可违反的设计决定

### 4.1 `std::type`

`std::type` 是标准根环境中的一个 Meta 值，表示类型值所属的类型。它不是 Parser 关键字，
也不是由 `import("std::type")` 加载的 Module。

```lain
let Box = std::func(T: std::type) -> std::type {
    return std::struct { value: T };
};
```

Parser 只记录 `std`、`:`、`:`、`type` 四个 Atom。Meta 通过普通路径查找获得该值。迁移
完成后，裸 `type` 必须诊断为未绑定名称，不提供兼容别名。

### 4.2 没有独立泛型机制

接收 `std::type` 或其他 Meta 值的函数就是普通 Meta 函数。`Vec(T)`、`Result(T, E)`、
`Ord(T)` 与普通函数调用使用相同的解析、参数绑定、执行和返回过程。

编译器不得：

- 创建 `generic` 专用 Parser 节点；
- 用 `comptime` 参数修饰符重新引入旧模型；
- 根据函数返回类型选择 type factory、module factory 或 AST factory 协议；
- 规定哪些 Meta 值“可以作为泛型参数”；
- 为兼容旧源码保留裸 `type` 或旧 generic policy。

相同调用结果的缓存可以以后加入，但缓存只优化执行，不能定义语言语义。当前路线图不以
缓存为完成条件。

### 4.3 函数签名

完整形式为：

```text
std::func(显式参数) ?{环境输入} -> 返回类型 !{输出 effect} {函数体}
```

- 显式参数由调用表达式提供。
- `?{name: TypeExpr}` 要求调用环境提供名为 `name`、类型为 `TypeExpr` 的 Meta 值。
- `?{name}` 等价于 `?{name: _}`，其类型由完整签名中的约束推导。
- `!{}` 记录函数可以发出的 operation。
- 空 `?{}` 和空 `!{}` 可以省略。
- 函数使用的环境输入必须在 `?{}` 中出现，不能自动捕获未声明自由变量。

本阶段的 `?{}` 只解析编译期 Meta 输入。不要自行扩展成运行时动态隐式参数；若实现中
确实需要这种语义，必须先讨论。

### 4.4 LAINVM 边界

LAINVM 只看到物理 Artifact、Procedure、Value 和参数。Meta 可以把类型、模块或 AST
保存在 VSpace 中，并把地址传给 VM；地址含义仍由 Meta 决定。

允许根据 LAINIR 的物理返回类型选择 bits、addr 或 unit 读取方式。禁止根据 Meta 返回
类别选择不同执行协议。不得恢复 `EvalResult`、object kind、owner、generation、sidecar
或其他 Meta 包装。

`Eval` 是 LAINIR 的概念，不是 LAINVM 的。LAINIR 定义并验证「这段已 lowering 的代码在
编译期执行」，LAINVM 只提供实现它所需的执行原语（`execute`/`execute_child`、VSpace、
预算、Trap）。因此：

- `Eval` effect 与它的 handler 归 LAINIR 契约，`src/lainvm/` 只保留原语；
- `eval_handler` 上的 `&mut LainVm` 参数是「本 handler 的恢复需要 TCB 机制」的未成型替身；
- LAINVM 的活跃契约收窄为「执行已验证的 LAINIR，并守住 VSpace、Trap 与预算」；
- effect 的实现策略（TCB 或 CPS）由 handler 在编译期决定，见
  [`../stdlib/effect-system.md`](../stdlib/effect-system.md)。LAINVM 只实现 TCB 一条路径，
  CPS 路径不涉及任何 VM 操作。

## 5. 实施顺序

| 阶段 | 状态 | 产出 |
| --- | --- | --- |
| 编码 0 | 已完成（2026-09-12） | `std::type` 可解析，旧名称冲突消失，裸 `type` 被拒绝 |
| 编码 1 | 进行中：1a 已完成；**1c 下一步**；1b 其余部分在其后 | bootstrap 中只有一条 Meta callable 执行路径 |
| 编码 2 | 等待编码 1 | `std::func` 完整签名可被 Meta elaborator 读取 |
| 编码 3 | 等待编码 2 | `?{}` 能推导并从环境解析输入 |
| 编码 4 | 等待编码 3 | 正式 std 与 lainc 全部迁移，旧泛型设施删除 |
| 编码 5 | 等待编码 4 | 正式 lainc 通过真实 LAINVM handler 执行 Meta |
| 编码 6 | 等待编码 5 | Lain 编译器达到 gen2 == gen3 固定点 |
| 编码 7 | 等待编码 6 | native backend 和发布 gate 收口 |

**编码 1 的内部顺序（2026-09-12 调整）**：

| 子阶段 | 状态 | 内容 |
| --- | --- | --- |
| 1a | 已完成 | Meta 调用的 LAINVM 执行管道统一（§7.0） |
| 1c | **已完成 2026-09-12** | 形式识别移回库：编译器中的 24 处名字字面量降为 **0**，库新增 10 个谓词（§7.0.2） |
| **1b** | **下一步** | sink 统一（1b-1/1b-2），及其余能力收敛（1b-3+） |

1c 的验收：`bootstrap/compiler/` 的名字字面量从 24 降到 0；
`scripts/check_meta_form_swap.py` 证明编译器真的经库询问（替换库谓词返回值则编译行为改变）；
15 道 baseline 退出 0。

调整理由：架构正确性优先。1c 恢复的是本来就写在设计里的边界（库拥有语言规则），
且不改变任何行为；1b 的剩余部分依赖 elaborate 与签名，晚做不会增加返工。

阶段必须按顺序推进。一个阶段内部可以拆成多个提交，但每个提交必须有独立的可执行验证。

## 6. 编码 0：建立 `std::type`（已完成 2026-09-12）

本节保留实施记录与验收命令，供追溯。编码 0 的所有条款均已落地并通过 §6.5 验收；后续阶段
以 §6.5 的命令作为回归基线。阶段完成后可按既有惯例把完成证据移入 `docs/history/`。

```text
python scripts/check_bootstrap_consteval.py
python scripts/check_lainvm_boundary.py
python scripts/check_srclainc_artifact.py
```

前两个检查应通过。第三个检查可能耗时较长；必须等待退出状态，不能把长时间无输出当作
成功。记录失败命令和第一个错误，不要顺手修复无关 backend 问题。

### 6.2 释放 `std::type` 名称

修改范围：

```text
std/type.lain
std/meta.lain
scripts/build_formal_stdlib.py
scripts/check_policy_conformance.py
scripts/fixtures/formal_meta_policy_probe.l1
bootstrap/std/policy.l1
```

步骤：

1. 把仍被 `std/meta.lain` 使用的严格转换规则移到 `std/type_policy.lain`，导出 Module 名
   `type_policy`。
2. 把 import 改为 `import("std::type_policy")`。
3. 审计旧 `std/type.lain` 的其他导出；没有当前调用者的策略直接删除，不复制到新文件。
4. 删除旧 `std/type.lain`，确保 `import("std::type")` 不再表示 Module。
5. 调整 policy probe，只保留仍属于当前设计的转换、effect 和 bounds 检查。generic 检查
   在编码 4 最终删除；在此之前不得给它增加调用者。

### 6.3 在标准根环境绑定类型宇宙

bootstrap 的首要修改位置：

```text
bootstrap/compiler/meta_values.l1
bootstrap/compiler/meta_bindings.l1
bootstrap/compiler/meta_type.l1
bootstrap/compiler/meta_collect.l1
bootstrap/compiler/stdlib_contracts.l1
```

实现要求：

1. 在 Meta 值系统中为类型宇宙建立稳定身份。可以继续使用 Meta 内部的 type-value kind；
   该 kind 不能进入 LAINVM API。
2. 在每个编译单元使用的标准根环境中创建 `std` Module，并把 `type` 绑定到该类型宇宙值。
   如果当前没有集中创建标准环境的函数，在 `meta_bindings.l1` 中新增一个，再由编译入口
   调用；禁止在路径查找函数里硬编码字符串返回值。
3. 修改类型标注判断：解析标注表达式，通过 `meta_env_lookup_path` 得到值，再以 Meta 值
   身份判断它是否为 `std::type`。不得继续用
   `meta_atom_equal(annotation, "type")`。
4. `program_is_type_binding` 若继续保留，必须接收环境并完成上述解析；同步更新
   `meta_collect.l1`、`lower_program.l1` 和 `stdlib_contracts.l1` 的所有调用点。
5. `i32`、`addr`、用户 record 等类型值的静态 Meta 类型都应是同一个 `std::type` 值。

### 6.4 原子迁移裸 `type`

当 bootstrap 能解析 `std::type` 后，将以下当前源码中的类型值标注改为 `std::type`：

```text
std/**/*.lain
src/lainc/**/*.lain
src/lainir/**/*.lain
src/lainvm/**/*.lain
scripts/fixtures/*.lain
```

不要修改 `docs/history/`。不要机械替换普通英文注释或 Python 的 `type[...]`。

新增 fixture：

```text
scripts/fixtures/formal_std_type_value.lain
scripts/fixtures/formal_bare_type_rejected.lain
scripts/check_std_type.py
```

正例至少覆盖：

```lain
let Alias: std::type = i32;
let main = std::func() -> Alias { return 42; };
```

返回 `std::type` 的 callable 留到编码 1 测试；编码 0 只证明类型宇宙绑定和类型标注解析。
反例 `let Alias: type = i32;` 必须得到稳定的“未绑定名称”诊断，不能被静默接受。

### 6.5 编码 0 验收

运行：

```text
python scripts/check_std_type.py
python scripts/check_bootstrap_consteval.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
```

附加搜索：

```text
rg -n -F 'import("std::type")' std src bootstrap scripts
rg -n --pcre2 ':\s+type\b|\bcomptime\s+[^:]+:\s+type\b' std src
```

第一个搜索必须无结果。第二个搜索必须无结果。

**这两个搜索的模式本身有坑，不要按早期版本的写法改回去：**

- 必须用 `\s+`（一个以上空白），不能用 `\s*`。`\s*` 允许零个空白，因此会把 `std::type`
  里的 `:type` 也匹配上——而 `std::type` 正是本阶段要引入的拼写。用 `\s*` 写这个搜索，
  它永远不可能为空，无法作为验收条件。
- 搜索范围必须排除 `scripts/fixtures`：反例 fixture
  `scripts/fixtures/formal_bare_type_rejected.lain` 有意保留裸 `type`，由
  `scripts/check_std_type.py` 验证它必须被拒绝。把它扫进"必须无结果"里是自相矛盾的。

搜索只证明旧拼写消失，不能替代四条执行命令。

建议提交切片：

```text
stdlib: release the std type name
bootstrap: bind the std type universe
source: migrate type annotations to std type
```

禁止做法：把 `std::type` 作为 Parser 关键字；保留裸 `type` 别名；让 `std::type` 同时表示
Module 和类型宇宙；为了通过构建而改写生成的 `build/**/*.l1`。

## 7. 编码 1：统一 bootstrap Meta callable

设计见 [`../implementation/meta-callable-unification.md`](../implementation/meta-callable-unification.md)。
该文档记录四道现状栅栏（§2）、已取证的关键约束（§3）、sink 统一与 Meta 值构造方案（§4）、
分片计划（§5）与两个待确认的边界问题（§8）。

### 7.0 已完成：编码 1a（2026-09-12）

Meta 调用的 LAINVM 执行管道已统一（提交 `bootstrap: unify the Meta call execution pipeline`）：

- 4 条路径重复的 parse → find → append → eval → release 收敛为 `lainvm_meta_run_artifact`，
  它按物理返回类型选择 bits / addr / unit 读取器；`vm-artifact-parse` 出现次数 4 → 1。
- 形参写入与实参追加改为按**物理参数类型**工作，取代按返回类别分的两套。
- 4 个 `lainvm_eval_meta_*` 的对外签名与行为逐字未变；artifact 构造逻辑未动。

验收：`check_bootstrap_consteval.py`、`check_lainir_boundaries.py`、`build_lain_compiler.py`、
`check_lainc_lainir_api_baseline.py` 均退出 0。

**剩余的 1b 部分不是机械收敛**：§7.2 步骤 4 的前提（「使用现有 lowering 设施把 callable body
lower」）今天不成立——通用 lowering 有意跳过 Meta 函数，两条路径写向不同 sink。详见设计文档 §2。

### 7.0.1 已记录的违规：构造器名硬编码

`bootstrap/compiler` 用字面量比较识别 `std::module`、`std::effect`、`std::handler`、
`std::effect_operation`、`std::type_with_namespace`、`std::handler_type`、`std::meta_type`、
`Module`、`ModuleShape`、`struct` 等名字（散落在 `lower_program.l1`、`meta.l1`、
`meta_collect.l1`、`meta_module.l1`、`meta_record.l1`、`module_meta.l1`）。

**`src/lainc` 一个都不认识**——它只区分「是不是 `import`」（需路径解析，见
`src/lainc/elaborator.lain:575`）与「有没有 `{...}` 体」（`:661`）。`std::module { ... }` 与
`std::func() { ... }` 走同一条路径，含义由库的绑定决定。

这符合 [`../00-intro.md`](../00-intro.md) 与 [`../03-meta-system.md`](../03-meta-system.md) §2
的分层：Parser 与 lowering 不判断这些名字的含义。

**判定：这是 `elaborate` 未完成（§3.2）的症状，不是设计选择。**

**但优先级已调整**（2026-09-12）：架构正确性优先，先把这个边界恢复，再继续 1b 的其余部分。
详见 §7.0.2 的**编码 1c**。

**编码 1b 不得新增任何名字匹配。** 详见设计文档 §4.2。

### 7.0.2 编码 1c：把形式识别移回库（优先级高于 1b 其余部分）

#### 目标

让「什么源码形式意味着什么」这个判断住在库里，而不是编译器里。删除编译器中的名字表，
改为经标准库 ABI 询问。

#### 依据

形式标准库**已经**用正确方式实现了这件事，只是没人调用：

| | 形式库 `std/meta.lain` | bootstrap 编译器 |
| --- | --- | --- |
| 名字识别 | 集中在 `meta_word_code` 一处，映射为整数编号 | 10 个名字字面量，散在 6 个文件 |
| 其余代码 | 比较编号：`meta_is_module` = `meta_word_code(...) == 11` | 每次逐字节比较 |
| 调用者 | 仅 `std/bootstrap/abi_entry.lain` 与测试探针 | 编译器自己 |

具体证据：

- `std/meta.lain:2360` `meta_word_code` 集中定义全部词→编号映射（`let`=2、`type`=4、
  `Module`=10、`module`=11、`struct`=12、`import`=13 等）；
- `std/meta.lain:2539-2551` 的 `meta_is_module` / `meta_is_struct` / `meta_is_import`
  全部比较编号；
- `std/meta.lain` 中**零处**字面量名字比较（全仓库搜索确认）；
- 但 `meta_module_status`、`meta_struct_status`、`meta_word_code` 的调用者为**零**。

#### 接缝已经存在

不需要新造机制，只需把已有的洞补上：

- `bootstrap/std/entry.l1:50` 的 `lain_std_expand` **已经在调**库函数
  `lain_std_meta_status`；
- `bootstrap/std/core_forms.l1` 的注释明确写着「核心暂时仍拥有 descriptor builder……
  这个标准库入口之后可由形式 std 实现替换」；
- 所以缺的是把**编译器里剩下的那些名字判断**也搬到这个接缝后面。

#### 知识与职责的划分

**这些构造器名不是库里的绑定。** 已核实：

- `program_bind_standard_root`（`bootstrap/compiler/meta_bindings.l1:87`）在标准根环境中
  **只绑定 `type` 一个名字**；
- `program_node_has_meta_constructor` 从**不查询绑定**，而是直接比较节点文本；
- 全仓库搜索：`module`、`struct`、`meta_type`、`handler_type`、`type_with_namespace`、
  `effect_operation` **没有任何 `let` 定义**，不可解析为值；
- 它们只被库代码**使用**（`std/bounds.lain:93` 写 `std::handler(Effect) {...}`），
  没有库侧定义。

因此正确的划分是：

| 归属 | 知道什么 | 理由 |
| --- | --- | --- |
| 编译器 | **有哪些种类**（模块、结构体、effect……）及每种的构造原语 | 原语由编译器/VM 提供，它必须知道自己的原语种类 |
| 库 | **每种叫什么名字**（哪个拼写对应哪个种类） | 语言特性定义，属于库 |

**编译器保留种类，库保留拼写。** 形式库已经是这个形状：把拼写集中在 `meta_word_code`
（`std/meta.lain:2360`），并导出 `meta_is_module`、`meta_is_struct`、`meta_is_import`
等**谓词**供按概念提问。

#### 工作项

1. 在 `bootstrap/std/` 新增谓词，名字识别集中在一处（照抄形式库编号，保持 1:1）：
   `lain_std_word_code`、`lain_std_is_module_declaration`、`lain_std_is_record_declaration`、
   `lain_std_module_group`、`lain_std_is_meta_constructor_path`、`lain_std_is_meta_return_type`、
   `lain_std_is_form_keyword`、`lain_std_is_effect_member`、`lain_std_is_std_module_value`。
2. 编译器用 `#extern #proc` 声明它们，把 24 处字面量比较换成按概念提问。
3. 库只能依赖 AstApi 底座（`raw_node_*`、`meta_atom_equal`），不得调用编译器函数。

**注意**：`program_collect_meta_call_candidate`（4 条 Meta 调用分派路径）不在此阶段，
它属于 1b。

#### 不做的事

- **不改变行为**：诊断码、判定结果逐字保持。
- **不新增名字**：只是搬家，不是扩表。
- **不在形式编译器里做**：`src/lainc` 的 elaborator 结构上不依赖名字比较
  （`src/lainc/elaborator.lain:575,661` 只区分 `import` 与「有无 `{...}` 体」），
  它没有这份债务。

#### 验收

```text
python scripts/check_bootstrap_consteval.py          # 5 类 Meta 路径
python scripts/check_lainir_boundaries.py            # 边界：core 不得含语义实现
python scripts/check_lainc_lainir_api_baseline.py    # 15 道 gate
```

加一条**新增**的关键验收，证明识别**真的**走了库而不是被绕过：

`scripts/check_meta_form_swap.py`，按 `scripts/check_stdlib_swap.py` 已有的模式实现
（取 `build/bootstrap/stdlib.l1` 文本、替换某函数返回值、用 `scripts/bundle_lainir.py`
重新拼、观察行为改变）：

1. 把库谓词 `lain_std_is_module_declaration` 的返回值从 1 换成 0；
2. 用含 `std::module` 声明的 fixture 编译；
3. **要求结果与未交换时不同**（该声明不再被识别为模块）；
4. 若结果相同，说明编译器仍在自己判断，退出码非 0。

该脚本必须登记进 `scripts/README.md` 的索引与 `scripts/check_lainc_lainir_api_baseline.py`
的 gate 列表。

外加文本验收（只能证明搬家，不能替代上面的执行命令）。当前清单是 **24 处、7 个文件**，
形状统一为 `"<名字>", <长度>`（是 `meta_atom_equal` 一类的比较参数，长度与名字同行）：

| 文件 | 处数 | 名字 |
| --- | --- | --- |
| `lower_program.l1` | 10 | `module`(×3)、`struct`(×2)、`effect`、`handler`、`effect_operation`、`type_with_namespace`、`handler_type`、`meta_type` |
| `meta_module.l1` | 4 | `module`(×3)、`struct` |
| `meta_collect.l1` | 3 | `effect`、`effect_operation`、`handler` |
| `meta.l1` | 2 | `struct`(×2) |
| `meta_call_vm.l1` | 2 | `effect`、`effect_operation` |
| `module_meta.l1` | 2 | `module`(×2) |
| `meta_record.l1` | 1 | `struct` |

按名字合计：`module` 7、`struct` 6、`effect` 3、`effect_operation` 3、`handler` 2、
`type_with_namespace` 1、`handler_type` 1、`meta_type` 1。

```text
rg -n --pcre2 '"(module|struct|effect|handler|effect_operation|type_with_namespace|handler_type|meta_type)"\s*,\s*[0-9]+' bootstrap/compiler/
```

必须无结果（`import` 不在其列——路径解析确实属于编译器，`src/lainc/elaborator.lain:575`
同样保留它）。

### 7.1 当前要替换的代码

主要文件：

```text
bootstrap/compiler/meta_call.l1
bootstrap/compiler/meta_call_vm.l1
bootstrap/compiler/meta_eval_vm.l1
bootstrap/compiler/meta_values.l1
bootstrap/compiler/meta_collect.l1
bootstrap/compiler/lower_func.l1
bootstrap/compiler/lower_program.l1
```

当前 `program_collect_meta_call_candidate` 依次识别 effect operation、module factory、effect
factory 和 scalar function。`lainvm_meta_build_scalar_artifact`、
`lainvm_meta_build_module_artifact` 等函数分别拼接临时 LAINIR。这些分支必须收敛。

### 7.2 目标调用算法

实现一个统一入口，名称可按现有风格确定，但职责必须完整：

1. 从环境解析 callee，确认它是 callable Meta 值。
2. 从 callable 的静态签名取得显式参数和返回类型。
3. 建立 invocation environment，并按顺序绑定实参；参数个数或类型错误在执行前诊断。
4. 使用现有 lowering 设施把 callable body lower 成临时 LAINIR procedure。不要为一种返回
   类别手写固定的对象构造 artifact。
5. 将 Meta 实参转换成签名要求的物理 bits、addr 或 unit，写入 VM argument vector。
6. 通过 `bootstrap.vm-artifact-parse`、`bootstrap.vm-procedure-find` 和一个共同执行函数
   调用 LAINVM。
7. 只根据 LAINIR 物理返回类型读取 bits、addr 或 unit。
8. Meta 根据 callable 的静态返回类型解释这个物理结果，并把 Meta 值绑定到调用环境。
9. Trap 直接转为编译诊断；Trap 时不得制造普通返回值。

`std::effect_operation` 本身也是 Meta callable。允许先保留它的语言层构造逻辑，但它的执行
必须进入上述共同入口。

### 7.3 删除目标

统一入口通过对应 fixture 后删除：

```text
lainvm_meta_scalar_function_supported
lainvm_meta_build_scalar_artifact
lainvm_eval_meta_scalar_call
lainvm_meta_module_factory_group
lainvm_meta_build_module_artifact
lainvm_eval_meta_module_call
lainvm_meta_effect_factory_node
```

如果实现仍需要物理 LAINIR buffer builder，应改为按 callable signature/body 工作，名称
不得包含 scalar、module、type、AST 或 effect 返回类别。

### 7.4 测试

扩展 `scripts/check_bootstrap_consteval.py` 或建立独立的
`scripts/check_meta_callable_execution.py`，真实编译并执行：

- `formal_meta_scalar_call.lain`；
- `formal_meta_module_factory.lain`；
- 新增返回 `std::type` 的 callable；
- 新增返回 AST handle 的最小 callable；
- effect factory 与 effect operation；
- 一个主动触发 Trap 的 Meta callable。

每个成功 artifact 必须由 verifier 接受、能够运行到预期结果，并且不含 `#eval`。失败用例
必须检查诊断码和 source span。

完成条件：以上用例全部经过同一入口；代码审查确认没有按 Meta 返回 kind 选择执行协议。

提交：

```text
bootstrap: unify meta callable execution
```

## 8. 编码 2：让 `std::func` 解释完整签名

### 8.1 Parser 约束

先在 `scripts/check_meta_ast_conformance.py` 增加 fixture，证明下面的 `?`、`!`、`->` 是
Atom，两个花括号都是普通 Group：

```lain
let f = std::func(value: T) ?{T: std::type} -> T !{IO} {
    return value;
};
```

除非这个 RawAst 形状无法由现有 Atom/Group 表达，否则不得修改 Parser 数据模型。可以修复
lexer 使 `?` 成为 Atom，但不能增加 Function、InputRow 或 EffectRow AST node。

### 8.2 callable 签名表示

bootstrap 表示与 `src/lainc` 正式表示都必须包含：

```text
declaration source and span
explicit parameter list
input row
return type
output effect row
body group
declaration environment
```

正式实现的主要修改点：

```text
src/lainc/meta.lain
src/lainc/elaborator.lain
src/lainc/effects.lain
src/lainc/types.lain
```

`src/lainc/effects.lain::FunctionType` 当前只有 `return_type` 和 `effects`；加入 input row 后，
输入和输出必须使用不同字段，不能把输入合并进 `effects`。

### 8.3 解析顺序和诊断

`std::func` Meta elaborator 按以下唯一顺序读取相邻 RawAst：

```text
parameter Group
optional ? + Group
required -> + return type expression
optional ! + Group
required body Group
```

必须拒绝：重复 `?{}`、重复 `!{}`、`!{}` 出现在返回类型之前、缺少 `->`、缺少 body、
输入条目缺名、输入条目在冒号后缺类型。每个错误保留引发错误的 RawAst source span。

空 `?{}`、空 `!{}` 与省略相应行语义相同。

### 8.4 验收

新增 `scripts/check_function_signature.py`，至少包含完整签名、两个空行、省略行及上述所有反例。
然后运行：

```text
python scripts/check_meta_ast_conformance.py
python scripts/check_function_signature.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
```

提交：

```text
meta: elaborate complete function signatures
```

## 9. 编码 3：实现输入 effect

### 9.1 本阶段语义

调用 `f(args)` 时按以下顺序处理：

1. 检查并绑定显式参数。
2. 根据完整签名推导所有 `?{name}` 的类型。
3. 从调用点的 Meta 环境按名称查找输入。
4. 检查找到的 Meta 值是否满足声明类型。
5. 把解析结果加入 invocation environment。
6. 完成 callable body 的 elaboration 和 LAINVM 执行。

查找使用精确名称，从最内层词法环境向外进行，最近的绑定遮蔽外层绑定；同一层同名绑定
在建立环境时就是重复定义。查不到名称产生“缺少输入”诊断。不得改成按类型扫描全局环境，
也不得从多个不同名称的值中任意挑选一个。

当前函数若调用需要输入的函数，但自身环境没有对应绑定，就必须在自己的 `?{}` 中声明并
向上传播，或者在函数体内显式建立 handler。编译器不得自动添加未声明输入。

### 9.2 推导规则

`?{T}` 中的 `_` 是待求解 Meta 类型变量。约束来自：

- 显式参数类型中对 `T` 的使用；
- 返回类型中对 `T` 的使用；
- 同一输入行其他条目的类型表达式；
- callable body 已有的类型约束。

所有约束得到同一类型时推导成功；没有约束或约束冲突时诊断。不得用名称大写、参数位置
或“看起来像类型”来猜测 `T: std::type`。

### 9.3 与输出 effect 的关系

输入行和输出行可以共享“有类型请求、handler、continuation”的底层数据结构，但静态方向
必须分开：

- 输入在调用开始前由环境提供；
- 输出 operation 在函数执行期间由 `perform` 发出；
- 输入不会自动出现在调用者的 `!{}` 中；
- 输出 handler 不会自动产生同名输入绑定。

### 9.4 Fixture 与诊断

新增：

```text
scripts/fixtures/formal_input_effect_max.lain
scripts/fixtures/formal_input_effect_explicit_type.lain
scripts/fixtures/formal_input_effect_missing.lain
scripts/fixtures/formal_input_effect_uninferred.lain
scripts/fixtures/formal_input_effect_type_mismatch.lain
scripts/check_input_effects.py
```

核心正例：

```lain
let max = std::func(a: T, b: T)
    ?{T, ord: Ord(T)}
    -> T {
    return ord.max(a, b);
};
```

检查真实编译结果与运行结果，并分别锁定缺少输入、无法推导和类型不匹配三类失败诊断。
同一作用域重复绑定继续由现有绑定诊断负责。最后运行
`check_function_signature.py`，确认输入解析没有改变 RawAst 或输出 effect 语义。

提交：

```text
meta: implement typed input effects
```

## 10. 编码 4：删除旧泛型设施并迁移源码

### 10.1 必须删除的设施

删除并同步所有调用者、ABI 列表和 fixture：

```text
std/generic.lain
std/meta.lain::meta_generic_parameter_valid
std/meta.lain::meta_generic_specialization_token
bootstrap/std/policy.l1 中对应过程
scripts/fixtures/formal_meta_policy_probe.l1 中对应 probe
scripts/build_formal_stdlib.py 中对应 ABI_SUPPORT_ENTRIES
src/lainc/elaborator.lain 中 SpecializationArgument、SpecializationKey、Specialization
src/lainc/diagnostics.lain 中只服务旧 specialization 的诊断
```

`ComptimeValue` 当前也被 `src/lainc/effects.lain` 用作 effect 参数。不能仅按名字删除：先把
仍然有效的普通 Meta 值用途迁到统一 MetaValue 表示，再删除只表达旧泛型参数分类的字段和
constructor。

不要在此阶段新增 replacement generic registry。类型函数直接执行；性能缓存留到固定点
之后根据 profile 决定。

### 10.2 迁移目标

重点检查：

```text
std/core/vec.lain
std/core/result.lain
std/core/slice.lain
std/core/string.lain
std/core/memory.lain
std/memory_model.lain
src/lainir/api_contract.lain
src/lainvm/api_contract.lain
src/lainvm/interpreter.lain
src/lainc/*.lain
```

这些文件中的类型参数都是普通 `std::type` 参数；需要环境供应的依赖放入 `?{}`；由调用者
明确传递的策略继续作为显式参数。不要仅为了减少实参就擅自把 Allocation、Bounds 或
Memory 移入输入行。

### 10.3 验收

运行：

```text
python scripts/check_policy_conformance.py
python scripts/check_stdlib_conformance.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
python scripts/check_srclainc_artifact.py
python scripts/check_input_effects.py
```

再搜索 `generic_policy`、`meta_generic_`、`SpecializationKey`、旧 `comptime` 语法。当前源码
和当前文档必须无结果；Python 自身的 `type[...]` 与历史文档不在此检查范围。

提交可以按标准库模块拆分，最后收口：

```text
stdlib: finish ordinary meta type factories
```

## 11. 编码 5：接通正式 lainc 的 LAINVM handler

### 11.1 当前缺口

`src/lainc/meta.lain` 已经写出 `perform Vm.eval(...)`，`src/lainvm/interpreter.lain` 已经提供
`eval_handler`，但 `scripts/check_lainvm_boundary.py` 只确认这些文本存在。必须建立一个
可运行的正式编译器组合，实际安装 handler。

### 11.2 实现顺序

1. 在组合层实例化 Memory、默认 LAINIR provider、LAINVM 和 compiler driver。
2. 安装 Memory、Bounds、Platform、VM Allocation、VM Eval 和 Trap 所需 handler。
3. 让 `Vm.eval` handler 调用 `execute_child`，使用临时 TCB 和调用者 VSpace。
4. 把普通 Value 恢复给 continuation；Trap 进入 compiler diagnostic。
5. 不允许 compiler core 导入 `src/lainvm/interpreter.lain`；只有组合层选择具体实现。

新增 `scripts/check_formal_lainc_eval.py`。它必须运行生成的正式 compiler artifact 编译一个
确实需要 Meta 执行的 fixture，并证明 `eval_handler` 被执行。记录 provider 或检查源码文本
都不能作为证明。

同时保留：

```text
python scripts/check_lainvm_boundary.py
python scripts/check_bootstrap_consteval.py
python scripts/check_formal_lainc_eval.py
```

提交：

```text
lainc: handle meta evaluation through lainvm
```

## 12. 编码 6：建立真正的 Lain 编译器固定点

### 12.1 先定义可执行入口

`src/lainc/lainc.lain` 当前只导出 `lainc.API` factory，不能被 seed 当作命令行编译器入口。
在开始代际构建前，先建立并测试正式组合入口。入口名称、参数和宿主 capability ABI 必须
写入 `src/lainc` 的当前文档和独立 fixture；不得沿用脚本中不存在的
`compiler_compile` 假设。

建议新入口名称为 `lainc_compile`。如果实现需要改变其 ABI，必须先讨论；不要让脚本和
源码各自选择一个入口。

### 12.2 修复固定点驱动

新增或重写专用于 Lain 编译器的脚本，例如 `scripts/run_lainc_self_host.py`：

1. bootstrap 编译 `scripts/lainc_sources.py::composed_compiler_sources()` 得到 gen1；
2. 用 gen1 和相同 source manifest 得到 gen2；
3. 用 gen2 和相同 source manifest 得到 gen3；
4. 每一代先用 verifier 检查约定入口；
5. 用 `scripts/prove_lainc_fixed_point.py` 比较 gen2 与 gen3；
6. 失败报告列出 extern、procedure label、signature 和 body hash 的首个差异；
7. 产物和报告全部写到 `build/selfhost/`。

不要使用 `scripts/run_lainir_self_host.py` 证明这一阶段。修复
`scripts/profile_lainc_bootstrap.py` 的 source manifest、默认 artifact 和入口后，它只能作为
性能报告，不作为唯一正确性证明。

### 12.3 固定点验收

- gen1、gen2、gen3 均能编译普通程序、`std::type` fixture 和输入 effect fixture；
- gen2 与 gen3 规范化一致；
- 三代产物都不含旧 generic ABI、`EvalResult` 或残留 `#eval`；
- 从干净 checkout 运行固定点脚本不会在 `src/`、`std/` 或 `bootstrap/` 写产物。

提交：

```text
bootstrap: reach the lain compiler fixed point
```

## 13. 编码 7：backend 与发布 gate

固定点完成后再处理：

- C backend 剩余物理指令和 ABI；
- native compiler matrix；
- source span 与 Trap source mapping；
- determinism、snapshot 和 release packaging；
- 真实 CI runner 上的 bootstrap、stdlib、fixed-point 与 native smoke。

最终更新 `scripts/check_lainc_lainir_api_baseline.py`，使它依次调用当前真实检查；删除已经失效
或只检查旧架构形状的 gate。全量命令必须从干净 checkout 返回 0。

提交：

```text
release: close the self-hosted compiler pipeline
```

## 14. 每个提交的执行规则

1. 修改前运行 `git status --short`，不要覆盖其他人的未提交文件。
2. 只编辑当前切片列出的源码；发现需要跨层修改时，先说明依赖。
3. 先运行该切片的最小真实测试，再运行阶段验收；不要每次都运行耗时数分钟的全量 gate。
4. 文本搜索只检查禁用名称，不能证明行为正确。
5. 测试产物放入临时目录或 `build/`。
6. `git diff --check` 必须通过。
7. 一个切片通过后立即提交，不把下一切片混入同一提交。
8. 阶段结束时更新本文的当前基线和状态表；已经完成的过程不继续堆在本文中。

## 15. 必须停止并讨论的情况

出现下列问题时，不得自行补充语言规则：

- `?{}` 是否需要运行时动态输入，而当前编译期 Meta 输入模型无法满足需求；
- 输入环境按名称解析与实际需要的类型导向搜索发生冲突；
- `std::type` 需要同时充当 Module；
- 一个 Meta 值无法通过现有物理 bits、addr 或 unit 传入 LAINVM；
- Trap 需要变成普通返回值才能继续实现；
- 固定点入口需要不同于已确认 ABI 的宿主能力；
- 需要把函数、输入行、类型工厂或 effect 行加入 Parser 语义节点；
- 需要恢复旧裸 `type`、`comptime` 或 generic policy 才能通过测试。

发生这些情况时，应提供最小复现、涉及文件、当前行为和两个可选方案，再请求决定。
