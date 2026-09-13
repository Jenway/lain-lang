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
- `scripts/check_lainvm_boundary.py` 检查的是**契约边界**（契约存在、lainc 只依赖契约而非任何
  实现、归档实现不得回到 `src/`），属源码级检查；它**不能**证明正式 lainc 的 `Vm.eval` 已经由
  真实 handler 执行。该 gate 在 2026-09-12 随形式实现归档而重写，不再检查实现形状。
- `bootstrap/compiler/meta_call.l1` 当前按 scalar、module 和 effect 返回类别选择不同路径；
  这只是过渡实现，不是目标架构。
- ~~`std::func` 还没有解释 `?{}` 输入行~~ **已解决（2026-09-12，编码 3）**：输入行在调用点
  解析、三类失败有诊断、§9.2 四个约束来源全部实现。由 `scripts/check_input_effects.py` 覆盖。
- ~~裸 `type`、generic policy、`ComptimeValue` 分类与 compiler-owned specialization 仍散布于
  `std/`、`bootstrap/` 与 `src/lainc/`~~ **已解决（2026-09-12）**：裸 `type` 由编码 0 移除，
  generic policy / `ComptimeValue` 分类 / specialization 由编码 4 移除。当前源码中
  `generic_policy`、`meta_generic_`、`ComptimeValue`、`Specialization*`、旧 `comptime` 修饰符
  **均无匹配**（仅注释里出现 `name : type` 这类文法说明文字）。
- `Eval` 目前定义在 `src/lainvm/`，但它是 LAINIR 的概念。迁移前 LAINIR 与 LAINVM 在契约
  层面仍然混着（见 §4.4）。
- C seed 的编译期求值使用「传源码 + fold」模型（`bootstrap.eval_source`，宿主重新 parse、
  verify 并原地折叠整个模块，返回值只是状态码，结果经 `bootstrap.eval-next` 侧信道取回，
  而该 capability 目前没有调用者）；lainc 使用「执行已验证 IR」模型
  （`Vm.eval(unit, procedure, arguments)` 返回物理 `Value`）。两者统一之前，编译期执行
  没有单一语义。
- 归档的 Lain LAINVM 实现（`docs/history/formal-implementations/interpreter.lain`）中的
  `suspend_tcb`、`resume_tcb`、`run_slice` 与 Endpoint 都没有调用者。这不是"缺实现"：现有
  全部 handler 的 `resume` 都在尾位置或根本不 `resume`，挂起机制的消费者尚不存在。
  见 [`../stdlib/effect-system.md`](../stdlib/effect-system.md)。该实现已于 2026-09-12 归档
  （§2），归档本身不影响这条判断。

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

### 3.5 已修复的静默误编译（2026-09-12）

前端有两个**静默误编译**：产出错误的 LAINIR，但**退出码 0、无任何诊断**。两者根因不同：

| 缺陷 | 现象 |
| --- | --- |
| `while` 的 `&&` 条件只保留**首合取项** | `while i < a && i < b` 只按 `i < a` 退出——算错或死循环。`if` 不受影响，因为条件走的是另一套 writer |
| 值表达式 writer **只支持单个算子** | `v * 10 + d` 编译成 `#mul(%v, 10)`，丢掉 `+ d`。加括号正确，所以一直没暴露；且不限赋值——`return` 与调用实参同样受影响 |

**验证方式是运行而非读码**：一个同时含 `f(3,4)` 与 `w(10,3)` 的程序返回 **37**；对修复前的
编译器同一程序返回 **40**（f 会得 30、w 会得 10）。新增 `scripts/check_operator_precedence.py`
断言该运行，并能把失败定位到单个用例（把它还原为修复前版本即报 `expected 37, got 40`）。

**一条值得记的实测**：编译器自己的源码闭包里就有一处。`srclainc` 产物只差 **1 行**——
从「测试 `meta_ast_kind(%body) == 2`」变为「同时测试它与 `meta_ast_delimiter(%body) == 123`」。
也就是说**一个分组定界符检查此前被编译掉了**。过程数不变（362），其余逐字节相同。

**教训**：这类缺陷不会让任何 gate 变红，只会让程序给出错误答案或让 verifier 在远离病因处报错。
因此凡是「表达式形状」相关的能力，验证必须落在**运行结果**上，文本断言只能定位不能证明。

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
| 编码 1 | 进行中：1a / 1c / 1b-1 / 1b-2 / 1b-3 已完成；1b-4、1b-5 受阻 | bootstrap 中只有一条 Meta callable 执行路径 |
| 编码 2 | **已完成 2026-09-12**（§8.4 四条验收全过；见 §8.5 的限定） | `std::func` 完整签名可被 Meta elaborator 读取 |
| 编码 3 | **基本完成 2026-09-12**：调用点解析、三类诊断、§9.2 四来源推导、scalar 路径、值宽度检查全部落地（§9.0.1）；仅余 §9.4 原文正例缺库符号 `Ord` | `?{}` 能推导并从环境解析输入 |
| 编码 4 | **已完成 2026-09-12**：旧泛型设施全部删除、源码已迁移（§10）；`srclainc` 过程数 358→353 | 正式 std 与 lainc 全部迁移，旧泛型设施删除 |
| 编码 5 | **受阻于归档**（§11.0 需先决定由谁提供 VM 与 provider） | 正式 lainc 通过真实 LAINVM handler 执行 Meta |
| 编码 6 | 阻塞于编码 5 | Lain 编译器达到 gen2 == gen3 固定点 |
| 编码 7 | 阻塞于编码 6 | native backend 和发布 gate 收口 |

**编码 2 的限定（2026-09-12）**：签名解析、存储与条目校验在 **bootstrap** 侧已完成并有
12 个用例的 gate（`scripts/check_function_signature.py`，含 span 断言的负对照）。**形式侧
只做到表示**：`src/lainc/effects.lain::FunctionType` 新增了 `input_row` 字段，但形式 elaborator
尚不读取签名（连箭头与返回类型都不读），所以该字段当前恒为空，属**仅有编译验证、无行为验证**。
详见 §8.5。

**编码 1 的内部顺序（2026-09-12 调整）**：

| 子阶段 | 状态 | 内容 |
| --- | --- | --- |
| 1a | 已完成 | Meta 调用的 LAINVM 执行管道统一（§7.0） |
| 1b | **部分完成** | 1b-1/2/3 已完成（sink 统一、三类构造器合一）；**1b-4 的前置已具备但需先定两个判据（§7.2 顶部）**；1b-5 受阻于设计决定 |
| 1c | **已完成 2026-09-12** | 形式识别移回库：编译器中的 24 处名字字面量降为 **0**，库新增 10 个谓词（§7.0.2） |

**归档的影响（2026-09-12）**：`src/lainir` 的 provider 与 `src/lainvm` 的 interpreter 已暂停
（§2），而编码 5 的原文依赖它们。编码 5 因此多了一个前置决定（§11.0），编码 6/7 顺延。
编码 0–4 不受影响。

**1b-1 已完成 2026-09-12**：宿主加了内存 artifact capture sink
（`artifact-capture-begin/data/length`）。它是**文件 sink 之上的一层**而非替代品——
编译期求值在发射阶段运行时会同时持有文件 sink，capture 必须能与它并存。
三个探针验证：缓冲增长（4104 字节越 4096 初始容量）、无静默截断、以及
「文件内容 `file1file2` 与 capture 内容 `mem` 互不混淆」。全部由
`scripts/check_bootstrap_vm_api.py` 覆盖。

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

> **1b-4 的前置已具备（2026-09-12 查实）。** 原先「按签名分类」缺两样东西，现在都有了：
> 编码 2 把签名（含返回类型节点，描述符 offset 24）存了下来，编码 1c 把形式识别移进了库。
>
> **分类依据确实在签名里**——库自己就是这么写的（`std/allocation.lain:15`）：
>
> ```lain
> let Alloc = std::func(Policy: Module) -> effects.Effect {
>     return std::effect("Alloc", Policy);
> };
> ```
>
> 所以 `-> effects.Effect` 是 effect factory、`-> Module`/`-> ModuleShape` 是 module factory、
> 标量类型是 scalar call。
>
> **但仍有两点必须先决定，不能猜：**
>
> 1. **如何判定「返回类型是 effect」**。`meta_value_kind` 里 **4 同时表示类型值与 effect**
>    （`meta_value_type` 与 `meta_value_type_with_namespace` 都用 4；而 effect 构造器在
>    artifact 里也写 4）。所以仅凭 kind 无法区分「返回一个普通类型值」与「返回一个 effect」。
>    可选判据：(a) 让库新增一个谓词，问「这个路径解析出的 Meta 值是不是 effect」——与 1c 的
>    分层一致；(b) 给 effect 值一个独立 kind——会动 Meta 值表示；(c) 保留 body 扫描作为
>    effect 与 module 的区分手段，只把 scalar 与其余分开——收敛幅度小。
> 2. **fixture `formal_meta_effect_factory.lain` 的声明是错的**：它写 `-> Module` 却返回
>    `std::effect(...)`。按签名分类后它会被判成 module factory。改成 `-> effects.Effect`
>    才与库的写法一致——但这是**改 fixture 的语义**，需要确认是 fixture 笔误还是有意为之。
>
> **在 (1) 定下之前不要开始 1b-4。**

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


先在 `scripts/check_meta_ast_conformance.py` 增加 fixture，证明下面的源码形状可以由现有
Atom/Group 表达，两个花括号都是普通 Group：

```lain
let f = std::func(value: T) ?{T: std::type} -> T !{IO} {
    return value;
};
```

**实测的 RawAst（2026-09-12，`lain_raw_ast_dump`）**：

```text
(root (atom let) (atom f) (atom =) (atom std) (atom :) (atom :) (atom func)
  (group ( (atom value) (atom :) (atom T))
  (atom ?) (group { (atom T) (atom :) (atom std) (atom :) (atom :) (atom type))
  (atom -) (atom >)
  (atom T) (atom !) (group { (atom IO))
  (group { (atom return) (atom value) (atom ;))
  (atom ;))
```

结论：

- `?` 与 `!` **已经是单个 Atom**，lexer 无需修改；
- 两处 `{...}` 都是普通 Group（delimiter 123）；
- **`->` 不是单个 Atom，而是相邻的 `(atom -)` 与 `(atom >)`** —— 本节早先假定它是一个
  Atom，**该假定错误**。Lain 的 tokenizer 不分关键字，因此没有 `->` token。
  签名读取器必须把「紧跟 `>` 的 `-`」识别为箭头，而不是期待单一 Atom。
- 参数组是普通 `(group ( ... )`，与 `{...}` 组以 delimiter 区分。

**因此本节的第一条验收（"`->` 是 Atom"）不可能成立，须改为上表的等价断言。**

除非该形状无法由现有 Atom/Group 表达，否则不得修改 Parser 数据模型。**不要**增加
Function、InputRow 或 EffectRow AST node；也**不要**为 `->` 添加 lexer 特例——相邻
`-` `>` 已经是可用的表示。

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

### 8.5 完成状态与限定（2026-09-12）

§8.4 的四条命令全部退出 0。实际落地的范围：

| 项 | bootstrap | 形式（`src/lainc`） |
| --- | --- | --- |
| 解析 `?{}` / `!{}` 并按固定顺序读取 | ✅ | ❌ 不读签名（连箭头与返回类型都不读） |
| 描述符存储（输入行 / effect 行 / 箭头位置） | ✅ offset 128/136/144 | ⚠️ 仅 `FunctionType.input_row` 字段 |
| 条目形状校验与 source span | ✅ 12 用例 + span 负对照 | ❌ |

**必须明说的验证限定**：`build/lainir/srclainc.l1` **从未被执行**——只有
`check_srclainc_artifact.py` 构建两次比较确定性。因此形式侧唯一的验证是
「`build_srclainc.py` 退出 0」。加了 `input_row` 字段后产物从 185791 变为 185770 字节
（过程数仍 358），证明它是真实结构而非被消除的空写；但**它的行为无从验证**，因为形式
elaborator 还不读签名，`std/meta.lain` 的 `meta_elaborate_status` 仍是边界桩。

形式侧的行为验证要等到 elaborate 真的读签名——那是编码 3 的工作。

#### 8.5.1 顺带发现：函数声明的形状校验住在错误的一层

编码 1c 把**形式识别**（`module`/`struct`/`effect` 等拼写）移回了标准库，
`bootstrap/std/core_forms.l1` 现在拥有词表与谓词。但**函数声明的形状校验仍留在编译器**：
`program_parse_function`（`bootstrap/compiler/lower_program.l1`）自己做 5102/5103/5104
这些判断。

对照之下，`module` 与 `struct` 的形状校验住在库里（`lain_std_meta_status`，诊断 3008/3013），
由 `check_meta_module_validation.py` 验证 bootstrap 与形式两套一致。

**同一类规则分居两层**，与 `docs/03-meta-system.md` §2「标准库定义语言规则」不一致。
函数签名的形状校验应当同样位于 Meta 层。

处置建议：**不在编码 2 里修**（那会把本阶段扩大成又一次边界迁移），而是在编码 3 或 4
处理——那时输入行的语义本来就要落进 Meta 层，可以顺带把函数声明的形状校验一并搬过去，
并照 `check_meta_module_validation.py` 的模式加一条 bootstrap/formal 一致性 gate。

#### 8.5.2 搬移的实测约束（2026-09-12）

编码 3 与 4 都已完成，本项未被吸收，因此现在单独处理。实测出形状校验的**顺序是行为的一部分**，
这决定了搬移不能做成「一个全量校验器」：

| 声明 | 现状诊断 |
| --- | --- |
| `std::func(a: i64) -> i64 !IO { return a; }` | **5102**（效果行形状） |
| `std::func(a: i64) -> bogus !IO { return a; }` | **5104**（返回类型合法性**优先**于效果行形状） |
| `std::func(a: i64) -> bogus { return a; }` | 5104 |
| `std::func(a: i64) -> NotAType !IO { ... }` | 5102（大写名被当作 nominal，类型合法，故形状错误胜出） |

`program_parse_function` 的既有注释已说明效果行检查刻意排在返回类型合法性之后；上表第三行证实
该顺序**可观测**。

而返回类型合法性（`program_type_valid_in_unit`）与参数解析（`program_parse_params`，填充描述符）
**必须留在编译器**——它们依赖 unit 类型表与描述符构造。

**因此可行的搬移形态是拆成两段**：签名头部形状（`= std::func`、params 组、`?` 行与条目、箭头、
返回/body 非空）与尾部形状（`!` 行、body 组），编译器在两者之间做自己的参数解析与类型校验。
单个全量校验器无法复现上表的优先级。

## 9. 编码 3：实现输入 effect

### 9.0 起点实测（2026-09-12）

编码 2 交付了签名的**语法与存储**；输入行的**语义**尚未开始。实测三个缺口：

| 场景 | 现状 |
| --- | --- |
| `std::func(a: T) ?{T} -> T { return a; }`，而 `T` 在任何地方都未定义 | **编译通过**，`T` 静默变成 `#addr`。§9.4 要求「缺少输入」诊断 |
| 同一个函数，**去掉** `?{T}`（`std::func(a: T) -> T`） | **行为与带 `?{T}` 完全相同**——输入行目前无任何语义 |
| `?{T}` 的 `T` 无约束可推导 | 无诊断 |

**基础设施已就位，缺的是接线**：被调函数的输入行已存在描述符 offset 128
（`program_function_input_row`），调用点构造 invocation environment 的函数是
`lainvm_meta_address_invocation_environment`（`bootstrap/compiler/meta_call_vm.l1`），
它绑定显式实参，但**从不读输入行**。

语法侧已完成并有 gate：`?{name}`、`?{name: type}`、空行、多条目、逗号分隔、以及
「缺名 / 冒号后缺类型 / 缺分隔符 / 重复行 / 行位置错误」的拒绝与 span，都由
`scripts/check_function_signature.py` 覆盖。**本阶段不要改这些语法行为。**

另注：§9.2 的完整类型推导（约束来自显式参数类型、返回类型、同行其它条目、body）若在本阶段
做不到，允许只实现能确证的部分，但必须在报告与本节中写明实现了哪些约束来源、哪些没有，
以及 §9.4 的原文正例 `?{T, ord: Ord(T)}` 是否因此不能通过。**不得假装完成。**

### 9.0.1 实现状态（2026-09-12）：本阶段各项已落地

调用点解析已落地：invocation environment 构造时会遍历被调函数的输入行，在**调用者**环境里
按名字查找、按声明类型检查、并绑定进 invocation environment。查找用既有的
`meta_env_lookup_path`，因此精确名、由内向外、最近绑定遮蔽的性质不变。

**可观测证据**：正例 fixture 把调用者本地的值经输入行传入，改调用者的 `41` 为 `7` 后程序
输出随之变为 `7`——值确实来自调用者环境，不是默认值。

三类失败都有诊断，都用既有码，且都指向引发错误的节点：

| 失败 | 码 | span |
| --- | --- | --- |
| 缺少输入（名字查不到） | 5108 | 条目的名称节点 |
| 类型不匹配 | 5108 | 条目的声明类型节点 |
| 无法推导（无类型裸名） | 5104 | 条目的裸名节点 |

#### 逐项状态（1–4 项均已修；第 5 项是已知的模型限制）

1. ~~值输入的类型匹配只做到 kind 级~~ **已修（2026-09-12）**：Meta 标量值现在在 payload 的
   offset 8（既有空槽，未扩结构、未加 kind）携带宽度；声明处从注解经既有宽度表取，Meta 调用
   结果取 callee 的返回宽度。比较**只在两侧宽度都已知时**进行——任一侧未知则保持旧的仅比 kind
   行为，避免让「无标注」或「宽度表不认得的注解」开始报错（有独立 fixture 锁定）。
   实测对照：旧编译器接受 `let x: i32` 对上 `?{x: i64}`（rc 0），新编译器报 **5108**（span 指向
   声明的 `i64` 节点）。
2. **§9.2 的约束来源已全部实现（4/4，2026-09-12）**：显式参数类型标注、返回类型标注、
   **同行其它条目的类型表达式**、**body 的 `let` 类型标注**。裸名 `?{T}` 若在任一位置出现，
   其 kind 推定为类型值；四处都不出现仍报 5104（「没有约束」这一支必须保留）。
   只认已确证的类型位置：body 只认 `let <名> : <类型>` 的标注，嵌套 `std::func` 形参表、return
   表达式、record 字段标注**不计入**（未能确证其必然是类型位置，计入会削弱 5104 反例）。
   §9.4 原文正例仍缺 `Ord`——那是**库符号**，全仓库不存在。
3. ~~裸名一律报 5104~~ **已修（2026-09-12）**：有类型位置约束时推导，无约束时才 5104。
   同时形参类型可经调用者环境解析：`std::func(a: T) ?{T: i32}` 现在可用，宽度由调用者提供的
   类型值决定（已用 i32→i64 的对照实测：形参从 `#bits<32>` 变为 `#bits<64>`）。
4. ~~scalar Meta 调用路径不读输入行~~ **已修（2026-09-12）**：scalar 路径现在解析输入行。
   实现要点：artifact 在显式形参之后按行序追加输入条目；调用点**复用**既有的
   `lainvm_meta_resolve_inputs`（未复制解析逻辑），从同一 invocation environment 取回值并按序
   追加实参。span 与码与 module/effect 路径一致。
   **一处刻意的偏离**：票据原要求所有条目不区分地作为 `#addr` 形参，实测不可行——
   `#proc meta_entry(#addr %local) -> #bits<32> { #return %local }` 被 verifier 以
   `2014 return type mismatch` 拒绝，且 `#addr` 携带的是 Meta 句柄而非数值。因此**标量类型**的
   条目按其声明宽度作 bits 形参（与既有显式标量形参同法），只有非标量条目才用 `#addr`。
5. **只报第一条失败条目**（与编译器 first-error-abort 模型一致）。

#### 顺带查实的两个既有缺陷（2026-09-12，均已修）

1. ~~`meta_builtin_size` 的四个 `#bits<N>` 分支不可达~~ **已修（2026-09-12）**：
   它们把整个标注与字面量 `"#bits<8>"` 这类整串比较，而**源码里的 `#bits<32>` 根本不是单个
   Atom**——实测 `lain_raw_ast_dump` 得到 `(atom #bits) (atom <) (atom 32) (atom >)` **四个**
   Atom，所以按整串比较永不命中，长度参数取值多少都无救。**我最初把它记成「长度参数写错
   （2/3 而非 8/9）」是不准确的——词法拆分才是根本原因。**
   现在宽度解析识别这四 Atom 序列并读十进制宽度，死分支已删除。**单位保持分离**：
   `meta_builtin_size` 仍返回字节数（`#bits<32>` → 4，与 `i32` 一致），位宽由同一 helper ×8
   得到，因此 record 布局与对齐无需其它改动。`#bits<32>` 的 RawAst 形状**未改**（前后 dump
   逐字节相同）。
   **行为对照**：修复前 `#bits<32>` 形参报 5104；修复后产物为 `(#bits<32> %x)`，且位宽真的驱动
   物理类型——`#bits<8>` 产 `(#bits<8> %x)` 并把实参 300 截断为 44，`#bits<32>` 保持 300。
   作为入口返回类型时 `#bits<32>` 通过、`#bits<64>` 被拒。
   回归由新增的 `scripts/check_bits_type.py` 守住。
2. ~~program 模式恒失败~~ **已修（2026-09-12）**：`program_validate` 原先把 `main` 的返回类型
   节点与字面量 `"#bits<32>"`（长度 3）比较，任何源码拼写都无法满足，故恒 **5112**。
   现改为比较**解析后的宽度**（`program_type_width_in_unit` == 32），契约因此变成语义的：
   `i32`/`u32`/`#bits<32>` 都能编译并真实运行，`i64`/`i8`/`#bits<64>` 仍报 5112。
   回归由 `scripts/check_program_entry.py` 守住——该检查有鉴别力：把编译器还原为修复前版本，
   它在第一个正例即失败。

**gate 总数因此从 18 增至 19**（新增 `check_bits_type.py`）。

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

**实测修正（2026-09-12）**：上述核心正例**当前不可表达**，有两个独立原因，都与本阶段的编译器
工作无关：

1. **`Ord` 在整个仓库里不存在**——`std/`、`src/`、`scripts/fixtures/` 全仓库搜索无任何
   `Ord` 定义。`Ord(T)` 是类型级函数应用，依赖该库符号；创建它属于**库**的工作，不是编译器。
2. `ord.max(a, b)` 要求对输入值做成员解析与调用，属另一层能力。

另外该正例还要求 **`a: T` 这种依赖形参类型**（形参宽度由调用者提供的类型值决定）。实测该能力
**至今不支持**：`std::func(a: T) ?{T: i32} -> i32 { return a; }` 的调用直接以 **3101** 失败，
因为 artifact 生成器解析不出 `T` 的宽度。

因此本节的验收**不能**以该正例为准。已交付的替代覆盖是
`scripts/check_input_effects.py`，它锁定：两种被接受的调用形式、三类失败诊断（含 span 与
负对照）、以及「值真的来自调用者环境而非默认值」的因果证据。

依赖形参类型（`a: T`）正在作为独立片推进；`Ord` 需要先在库里创建。

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
src/lainc/*.lain
```

这些文件中的类型参数都是普通 `std::type` 参数；需要环境供应的依赖放入 `?{}`；由调用者
明确传递的策略继续作为显式参数。不要仅为了减少实参就擅自把 Allocation、Bounds 或
Memory 移入输入行。

`src/lainvm/interpreter.lain` 原在此列，但该文件已暂停并归档
（`docs/history/formal-implementations/`，见 §2）。重写它时同样适用本节要求，但那是
归档恢复后的工作，不阻塞本阶段。

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

**完成状态（2026-09-12）**：§10.1 的清单已全部删除并同步调用者，§10.3 的六条命令全部退出 0，
文本搜索无结果。

实测到的两处值得记下：

1. **`ComptimeValue` 不能按名删除**，这与 §10.1 的预判一致：effect 参数向量仍在用它。
   被删掉的是**旧的泛型参数分类**（按 kind 分成四路 payload）与它的四个 constructor；保留的
   是单一 tagged 编译期值（`kind`/`type_id`/`payload`/`valid`），它不含「哪些 Meta 值可作为
   泛型参数」的策略，也不含按返回类型分派的协议。effect store 改用 `types.MetaValue` +
   `same_meta_value`，排序仍为 kind 再 payload。
2. **删除是真实的代码缩减**：`srclainc.l1` 的过程数从 **358 降到 353**。因此本阶段**不能**用
   「产物逐字节不变」作为验证判据——那是前面几个阶段的判据。本阶段的判据是两侧标准库一致性
   （`check_policy_conformance`）、`check_stdlib_conformance`、两个构建、确定性 gate
   （`check_srclainc_artifact`）、以及 19 道 baseline 全绿。

**残留的合法提及**（不是遗漏）：`std/effect.lain:3` 的注释 "Generic Meta facilities"、
`std/meta.lain` 的 `callable_phase_comptime`（phase 常量，与旧 `comptime` 模型无关）、以及
`std/core/vec.lain` 里 "specialization" 的普通用法（按类型特化存储）。

## 11. 编码 5：接通正式 lainc 的 LAINVM handler

> **状态（2026-09-12）：受阻于归档。** 本节原文假定 `src/lainvm/interpreter.lain` 提供
> `eval_handler`，且组合层实例化「默认 LAINIR provider」与 LAINVM。这两者都已暂停并归档
> （§2）。要推进本节，必须先决定 LAINVM 与 LAINIR provider 由谁提供——见下方 11.0。

### 11.0 前置决定：由谁提供 VM 与 provider

归档后有两种可行路径，选择权在项目负责人：

| 路径 | 内容 | 代价 |
| --- | --- | --- |
| A. 先用 C seed 兼作 provider 与 VM | 组合层直接调用宿主已有的 `bootstrap.vm-*` 能力（bootstrap 的 Meta 路径已这样工作）。`eval_handler` 由**组合层**提供，不在任何 VM 内部。 | 与 §4.4「`Eval` 归 LAINIR 契约、handler 在编译边界」一致；但 Lain 侧没有 TCB 可见，`execute_child` 语义由宿主承担。 |
| B. 先重写 Lain 版 LAINVM 与 provider | 补齐归档版本缺的部分（浮点、表达式覆盖、成员别名 lowering），再按本节原文推进。 | 工作量大；且归档版实测贡献 0 个物理过程，重写前需先确认它能被真正执行。 |

**在做出选择前不要开始本节。** 路径 A 更接近当前可运行状态；路径 B 符合「用 Lain 重写」
的长期方向，但应先把「什么条件下算写对了」定义清楚（那正是归档 README 里记的缺口清单）。

### 11.1 当前缺口（原文，保留供追溯）

`src/lainc/meta.lain` 已经写出 `perform Vm.eval(...)`，`src/lainvm/interpreter.lain` 已经提供
`eval_handler`，但 `scripts/check_lainvm_boundary.py` 只确认这些文本存在。必须建立一个
可运行的正式编译器组合，实际安装 handler。

### 11.2 实现顺序

1. 在组合层实例化 Memory、LAINIR provider、LAINVM 和 compiler driver。
2. 安装 Memory、Bounds、Platform、VM Allocation、VM Eval 和 Trap 所需 handler。
3. 让 `Vm.eval` handler 调用 `execute_child`，使用临时 TCB 和调用者 VSpace。
4. 把普通 Value 恢复给 continuation；Trap 进入 compiler diagnostic。
5. 不允许 compiler core 导入任何 VM 或 provider 的实现文件；只有组合层选择具体实现。

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

> **状态（2026-09-12）：间接受阻。** 固定点要求 lainc 能编译自己并运行，因此依赖编码 5
> 提供的可运行组合（VM 与 provider）。编码 5 的选择（§11.0）决定本节何时可开始。

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

### 13.0 backend 行内 else 缺陷：已修（2026-09-12）

`src/lainc/backend_c.lain` 的 `emit_line` 曾把 `} else {` 当**行首前缀**匹配后直接返回，
**丢弃该行剩余内容**。于是 `} else { %r = 2 }` 只发射 `} else {`，丢掉 `%r = 2` 与结尾的 `}`，
函数少一个闭合大括号、下一个函数嵌套进去，`zig cc` 报 `function definition is not allowed here`。

全量产物 `build/lainc-native.c` 因此有 **3 处**未闭合（都在 `program_std_type_member` 里形如
`} else { #return #call meta_value_nil() }` 的三行）。

**修复**：三个前缀分支（`} else {`、`else {`、单独 `}`）不再丢弃行尾，改为消费完整逻辑行；
行尾若是 `}`，先把其前的内容作为语句发射，再单独发射 `}`。递归起点严格前进，故有界。

**独立复验**（括号扫描前先用正则剥掉字符串/字符字面量与注释——不剥会误判，我踩过）：
`final_depth` 从 **+3 → 0**，出现在 `depth>0` 处的顶层函数定义从 **33 → 0**；
最小复现 `scripts/fixtures/backend_inline_else.l1` 的产物现在能被 `zig cc` 编译；
`check_native_backend_canonical_diff.py` 与 `check_native_backend_migration.py` 未回归。

#### 13.0.1 native 构建仍失败：缺口已收窄到两类

三个缺陷修完后（行内 else、`#data`、`#data_addr`），`build_default_lainc.py` 仍失败，
错误数 **40 → 37**，只剩两类，**都不是**上述三项：

| 类别 | 条数 | 内容 |
| --- | --- | --- |
| `#eval` 被原样发射 | 1 | `uintptr_t root= #eval {;`。按设计 `#eval` 必须在 backend 之前消除，故属编码 5/6 |
| native host 缺能力符号 | 36 | 13 个符号未声明（见下） |

**已修掉的那 1 条**是 `#data_addr` 的 `-Wint-conversion`：先前按 seed 的指针形式发射，
而本后端把 `#addr` 建模为 `uintptr_t`；改为 `((uintptr_t)NAME)` 后消失（提交 `10a0a1d`）。

13 个缺失符号：`bootstrap_vm_{arguments-new,arguments-release,arguments-append-addr,
arguments-append-bits,artifact-parse,artifact-release,procedure-find,eval-bits,eval-addr,eval-unit}`
与 `bootstrap_artifact_capture_{begin,data,length}`。
`seed/src/host/native_lainc.c` 只提供 prologue 已声明的那 15 个；`emit_extern_decl` 又对
`bootstrap.` 前缀一律跳过（假定 prologue 已覆盖），于是这些符号无人声明。

**把 13 个声明补进 prologue 不是修复**：`native_lainc.c` 并未实现它们，错误只会从编译期
"未声明"变成链接期"未定义"。真正的修复是实现这 13 个宿主函数。
**且它单独也解不开**：`#eval` 那条仍在。两项都要具备，与 §3.2 的结论一致。

**追责说明**：`bootstrap_vm_*` 的依赖**早于**本轮改动（改动前的
`bootstrap/compiler/meta_eval_vm.l1` 已有 12 处引用），属既存缺口；
`bootstrap_artifact_capture_*` 由 1b-1 引入，使所需符号从 10 个增至 13 个。
同一性质：宿主未覆盖 artifact 实际使用的 ABI。

#### 13.0.2 后端覆盖面审计（2026-09-12）：编译器闭包内无缺口，但闭包外有

把 `build/bootstrap/lainc.l1`（20661 行、362 过程的完整编译器）交给后端，统计产物中的
`/* unsupported L1: ... */` 标记：**真正的未支持构造为 0 条**
（早先看到的数十条标记全是**源码注释**被透传，不是构造）。输入 artifact 用到的构造已被全覆盖，
含 `#call` 7341、`#addr` 4344、`#if` 2867、`#let` 2530、`#return` 2306、`#eq` 916、`#lea` 906、
`#store` 478、`#add` 466、`#load` 414、`#ne` 386、`#continue` 368、`#loop` 312、`#break` 307、
`#sge` 180、`#sub`、`#zext`、`#mul`、`#slt`、`#sgt`、`#sle` 等。

**但这只说明「编译器闭包用到的构造」被覆盖，不等于「LAINIR v1 被覆盖」。** 后者我在接着
逐条测试时发现两处真实缺口（都不被编译器闭包触发，因此前面所有 gate 都看不见）：

| 缺口 | 实测 |
| --- | --- |
| **`emit_c_type` 的宽度/浮点映射不全** | 只处理 `#unit`、`#addr`、`addr`、`#bits<32>`、`#bits<1>`，**其余一律回落 `uint64_t`**。故 `#bits<8>` 得 `uint64_t`（应为 8 位）、`#float<32>`/`#float<64>` 也得 `uint64_t`（应为 `float`/`double`） |
| **未知表达式被原样透传** | `#fadd(%x, %y)` 直接写进 C（`return #fadd(x, y);`），**既不支持也不报 `unsupported L1`**，静默产出非法 C |

**关键差别**：seed 的发射器遇到不认识的类型**返回失败**（`seed_emit_c_type` 的兜底是
`#return 0`），而 Lain 后端**静默替换成 `uint64_t`**。「失败」与「静默给出错误答案」是两回事，
后者正是 §3.5 那条教训的同一形态。

**已修（2026-09-12，提交 `5c0a5ff`）**：

- `emit_c_type` 补齐 `#bits<8>`→`int8_t`、`#bits<16>`→`int16_t`、`#float<32>`→`float`、
  `#float<64>`→`double`，显式写出 `#bits<64>`→`uint64_t` 与 `#never`→`void`；
  兜底改为**标记并失败**（不发 artifact，seed 驱动报 status 1）。
  **`#bits<32>` 仍是 `uint32_t`、`#bits<1>` 仍是 `uint8_t`**：实测若把 `#bits<32>` 改成 seed 的
  `int32_t`，闭包产物会有 125 处差异，其中 14 处是 host ABI 的 `extern` 原型（真实 C 签名是
  `uint32_t`/`int`），改动会扩散到 ABI 边界，故保留。
- `emit_expr` 的未知 `#` 构造不再透传，改走既有的 `/* unsupported L1: ... */` 通道。
  **另外三个同源泄漏一并修掉**，均无任何 gate 覆盖：
  - `#sdiv` 曾被匹配但落在 else 链之外，仍落入兜底 → 除法产出 `L1_sdiv#a, b)`，
    **即除法在后端里本来就是坏的**；
  - `#call_indirect` 与 `#call` 共享前缀，尾部被泄漏；
  - `#alloca(<类型>)` 被透传成 `L1_alloca(#bits<8>)`。
- **float 运算有意未实现**：本后端把一切值（含地址）都当 `uintptr_t` 传参，浮点值没有跨调用边界的
  表示；正确实现需要位转换 helper 与调用约定改动，属新特性而非修复。边界由 fixture 钉住
  （断言出现 marker），不伪造 lowering。

**修正结论**：native 构建当前的障碍仍只在 `#eval` 与宿主 ABI（对编译器闭包而言成立），
但后端**并非「LAINIR v1 全覆盖」**——float 运算是未实现（现已显式标记而非静默产出非法 C），
整数宽度与类型映射已补齐。

#### 13.0.3 前端表达式形状审计（2026-09-12）：第二轮无缺口

上一节记录的两个静默误编译修好后，又对易错形状做了两轮穷举性检查，**均正确**：

| 形状 | 产物 |
| --- | --- |
| `1 + 2 + 3` | `#add(#add(1, 2), 3)` |
| `v * 2 * 3` | `#mul(#mul(%v, 2), 3)` |
| `v + d * 10` | `#add(%v, #mul(%d, 10))` |
| `v - d - 1` | `#sub(#sub(%v, %d), 1)` |
| `v * 2 + d - 1` | `#sub(#add(#mul(%v, 2), %d), 1)` |
| `(v + d) * 2` | `#mul(#add(%v, %d), 2)` |
| `v / 2 + d` | `#add(#sdiv(%v, 2), %d)` |
| `a - -b` | `#sub(%a, #sub(#trunc(0), %b))` |
| `(a + 1) * (a - 1)` | `#mul(#add(%a, 1), #sub(%a, 1))` |
| `a + a * a` | `#add(%a, #mul(%a, %a))` |
| `while i < a \|\| i < b` | 单条 `#eq(#ne(#add(#zext(#slt…), #zext(#slt…)), 0), 0)` |
| `while i < a && i < b` | 两条 `#break` |
| `if i < a && i < b` | `#eq(#add(#zext(#slt…), #zext(#slt…)), 2)` |

**另一个值得单独处理的问题**：backend 产出畸形 C 时**退出码为 0**，没有任何诊断，失败只在
`zig cc` 阶段以级联错误暴露。已由 `scripts/check_backend_c_shape.py` 补上（括号平衡 +
无嵌套函数定义），该 gate 有负对照。

本节各项与编码 5 的 A/B 选择无关，因此不受其阻塞。

#### 13.0.3b backend load/store 宽度误编译：已修（2026-09-12）

与 §13.0.2 的「静默替换」同类，但后果是**内存安全**：

| 源 | 修复前 | 应为 |
| --- | --- | --- |
| `#load[#bits<32>]` | `L1_load64`（**越界读 4 字节**） | `L1_load32` |
| `#load[#bits<16>]` | `L1_load64`（**越界读 6 字节**） | `L1_load16` |
| `#store[#bits<16>]` | `L1_store64`（**破坏相邻 6 字节**） | `L1_store16` |

根因：load 分支只区分 `#bits<8>`、其余**全部回落** `L1_load64`（`L1_load32` 在 prologue 定义了
却从未被选中）；store 分支覆盖 8/32，**16 落入 `L1_store64`**；prologue 没有 16 位宏。

**已修**：两条路径按宽度分派，prologue 补 `L1_load16`/`L1_store16`，**未知宽度改走 unsupported
标记而非默认 64 位**。

**运行验证**（比文本强，且我独立复跑过）：

- 64 位 store 在 `p+2` 埋 `0xFF` 哨兵 → 16 位 store 写 1 → 读回哨兵字节：
  **修复后返回 65（哨兵存活）**，修复前返回 **7（哨兵被 8 字节写清零）**。
- 全 1 缓冲做 16 位 load：**修复后得 65535**，修复前得 **4294967295**（多读了 6 字节）。

**顺带发现（与 §3.5 同一形态）**：同一份 `srclainc.l1` 经两个版本的后端，产物只差 **3 处**——
两个新宏，外加**两处 load 从 64 位纠正为 8 位与 32 位**（偏移 8 的 1 字节 tag、偏移 24 的 4 字节
字段，此前都在越界读）。也就是说**编译器自己的产物里就有被误编译的 load**。

#### 13.0.4 已交付：两个 C 后端的差分测试

本轮定位到的后端缺陷里，**最有诊断力的一步是拿两个后端对同一输入做对照**——
`seed` 的发射器与 Lain 后端都能把同一份 LAINIR 翻成 C，而它们的产物本应语义一致。实测例：

| 输入 | seed | Lain 后端 |
| --- | --- | --- |
| `#load[#bits<32>]` | `lainir_load_i32` | **`L1_load64`** ← 越界读 |
| `#load[#bits<16>]` | `lainir_load_i16` | **`L1_load64`** |
| `#store[#bits<16>]` | 16 位写 | **`L1_store64`** ← 破坏相邻字节 |

**文本比对不可行**（两者命名与 addr 表示不同：`lainir_*` + `uint8_t *` 对 `L1_*` + `uintptr_t`），
所以差分必须是**语义级**：两份 C 各自编译成可执行文件，对同一组 fixture 断言**相同输出或相同
退出码**。

这与 §11.0 那条「Lain VM 与 C seed 的行为差分」是同一手段，可用同一套 fixture 语料。
**已交付（2026-09-12，提交 `26d80ef`）**：`scripts/check_backend_differential.py` 实现了上述语义差分，
并已进入主入口（gate 总数 37）。两个 fixture：`differential_load_store.l1`（窄 load/store 宽度）、
`differential_arithmetic_control.l1`（优先级、`&&` 条件、除法）。

**它第一次运行就找到一个真缺陷**：Lain 后端的 load/store helper 是**解引用强转指针**，
而 seed 走 `memcpy`。`#alloca(n)` 给出的是字节数组，故偏移 2 上的 32 位访问**真的未对齐**——
seed 能处理，Lain 后端则 panic（`load of misaligned address ... requires 4 byte alignment`）。
helper 已改为 memcpy 形式并接收 `uintptr_t`。

**关于这个 gate 自身的一条教训**：用 `-O2` 构建时，**即使后端未修复它也通过**——
优化会去掉对齐检查，而 x86 容忍未对齐读，两者只在 Debug 构建下才显现差异。
因此该 gate **不启用优化**，原因写进了脚本注释。

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
