# Lain 编译器执行路线图

整理与核验日期：2026-09-13。本文只保留未完成工作、执行约束、依赖与验收条件。
已实现切片及验收记录统一见 [`历史索引`](../history/README.md)。
完成一个切片后先追加完成快照，再从本文移除；历史验收不代表当前产物已重新通过。
阶段编号保持原编码编号，删除完成项后不重编号，便于引用。

## 1. 术语与执行链

- RawAst：仅包含 Atom/Group、拓扑、文本位置、origin/hygiene 的无语义源码树。
- Meta：Lain 在编译期操作 AST 的语言定义层，承担 expand、elaborate、lower。
- LAINIR：确定物理类型和操作的 IR；`#eval` 表达编译期执行。
- LAINVM：执行已验证的物理 IR，提供 TCB、VSpace、预算和 Trap 原语。
- Trap：VM 的执行终止/失败状态；外部事件可由宿主或 OS 注入，编译器将编译期 Trap 转为诊断。
- 输入行 `?{}`：由调用环境供应的具名编译期输入；输出行 `!{}`：执行期间可能发出的 operation。
- 固定点：编译器生成下一代编译器后，连续两代规范化产物一致，并有代际执行验证。

```text
Lain source -> RawAst -> library Meta expand/elaborate/lower
            -> LAINIR verify + execute #eval -> runtime LAINIR -> backend
```

## 2. 实现归属与产物约束

| 位置 | 职责与修改约束 |
| --- | --- |
| `seed/` | C 物理 parser/verifier/VM 与宿主能力；仅补真实缺失的物理机制，不实现 Lain 语言规则 |
| `bootstrap/compiler/`、`bootstrap/std/` | 启动编译器与手写 LAINIR 库；本次语义迁移须把规则放进库阶段 |
| `src/lainc/`、`std/` | 正式 Lain 实现；通过同一 Meta ABI 组合，不能与 bootstrap 当成同一份代码 |
| `src/lainir/`、`src/lainvm/` | 活跃契约；正式 provider/VM 的运行能力提供方式待编码 5 决定 |
| `build/` | 所有生成产物、快照、差分报告；永不提交，不修改产物来通过验收 |

正式 provider/VM 的归档实现只用于追溯，不能直接回到源码闭包充当已验证实现。
能力由组合层显式注入，状态通过参数传递；compiler core 不导入具体 VM/provider 实现。

## 3. 未完成依赖与证明缺口

- bootstrap 的 AST/语义处理仍有 operation/module/effect/scalar 专用分派，需编码 1 迁移。
- 正式库与 lainc 尚缺完整签名语义及运行验证；库策略输入的完整正例尚未打通（编码 2/3）。
- `Eval` 的契约归属仍需从 LAINVM 迁到 LAINIR，正式组合层执行链需接通（编码 5）。
- `srclainc.l1` 缺约定可执行编译入口；两次构建一致只能证明源码闭包确定性（编码 6）。
- 现有 LAINIR 编译器自举 gate 不能证明 Lain 编译器固定点；旧 profile 脚本的入口假设需修订。
- native host ABI、剩余物理指令、alloca 生命周期及发布矩阵尚待收口（编码 7）。

## 4. 执行中的硬性边界

### 4.1 类型与函数规则

`std::type` 是标准根环境中的 Meta 值，不是 Parser 关键字或 `import("std::type")` 模块。
类型参数通过普通函数/工厂参数传递；不增加专用 generic Parser 节点、不按返回类型选择
工厂执行协议、不恢复裸 type、旧 comptime 修饰符或 generic policy。

```text
std::func(显式参数) ?{环境输入} -> 返回类型 !{输出 effect} {函数体}
```

`?{name}` 等价于 `?{name: _}`，约束由完整签名与库语义提供；输入/输出使用不同静态字段。
不得隐式捕获未声明自由变量，不自行引入运行时动态隐式输入。

### 4.4 物理执行边界

Meta 需要 AST 与语义对象的 opaque handle，不要求这些引用必须是编译期地址。
若实现以 `#addr` 承载 handle，地址的创建、持有与有效性检查归 LAINVM 执行环境，
对象引用的交付归 VM 执行接口与 Meta 对象协议；请求 VM 能力须由 LAINIR 表述，
不得从高层语言绕开 LAINIR。`#eval` 只负责已验证计算的编译期求值与产物结果，
不承担对象引用或编译期地址的交付。
执行 `#eval` 的 TCB 不自带 VSpace，也不隐式继承调用者地址空间；可访问的地址
及相应内存能力必须由执行请求显式传入。无地址输入与内存授权时，不能假设
临时 TCB 拥有可用的编译期地址。
当前 bootstrap 的 `meta_entry` 用 `#eval -> #addr` 交付 Meta handle，是待拆分的
过渡路径；不能据此规定 `#eval` 必须支持编译期地址结果。

库拥有函数、类型、模块、effect、operation/handler 等规则及语义对象；core/VM 只提供通用
AST/IR/执行机制。编译期计算经过显式 LAINIR `#eval`；其执行 handler 在编译组合边界安装。
按物理 bits/addr/unit 读取结果，Trap 转诊断；不得按 Meta 返回类别选择执行协议，不恢复
EvalResult、Meta object kind/owner/generation/sidecar 包装协议。

effect 的 TCB/CPS 实现策略由库 handler 在编译期决定。见
[`../stdlib/effect-system.md`](../stdlib/effect-system.md) 与
[`../03-meta-system.md`](../03-meta-system.md)。
当前 `std/effect.lain` 的 `Trap: Effect` 与 `effects.trap()` 是过渡中的库包装；
实现归属和运行验收要以 LAINVM Trap 能力为准。库可以决定何时请求终止，
VM 负责记录 Trap、停止 TCB、交付失败；宿主/OS 外部事件经能力边界注入。
不得用 effect 构造是否成功代替 VM Trap 的执行证据。

## 5. 剩余实施顺序

| 阶段 | 状态/依赖 | 剩余产出 |
| --- | --- | --- |
| 编码 1 | 1e 部分实现，剩余边界与 1f–1h 优先推进；在 bootstrap 链验证 | 库实现语言语义，统一生成 LAINIR 与 `#eval` |
| 编码 2（剩余） | 纳入编码 1 的库语义迁移；正式运行验收依赖编码 5 | 正式签名语义、类型检查与未覆盖形状一致性 |
| 编码 3（剩余） | 库符号/成员调用及完整策略输入依赖库语义能力 | `?{T, ord: Ord(T)}` 完整正例与严格类型约束验收 |
| 编码 5 | 先选 provider/VM 提供路径，接收编码 1 的共同契约 | 正式 lainc 的可运行编译期求值组合 |
| 编码 6 | 依赖编码 5 的可运行编译器 | Lain 编译器 gen1/gen2/gen3 执行及 gen2 == gen3 固定点 |
| 编码 7 | 物理后端/ABI 工作可独立推进；最终 native/发布验收依赖编码 6 | 后端语义覆盖、native matrix、CI 与发布 |

编码 2/3 的剩余库语义工作与编码 1 一并安排，避免重复建设；正式编译器行为验证在编码 5
运行组合接通后执行。每片具有独立执行验收，完成后从本表及正文移除对应工作。

## 7. 编码 1：将语言语义归还库 Meta 层，统一生成 LAINIR 与 `#eval`

计划修订：2026-09-13。设计与迁移清单见
[`../implementation/meta-callable-unification.md`](../implementation/meta-callable-unification.md)。
本节跟踪未完成的 1e–1h。基础与路径审计证据见完成归档。

### 7.0 目标与边界

Meta 是 Lain 在编译期操作 AST 的语言定义层，负责 expand、elaborate 和 lower。
函数、类型、模块、effect、operation 和 handler 的识别、构造、检查及 lowering 规则由库拥有。
需要编译期计算时，库 Meta 生成 LAINIR `#eval`；LAINIR 定义编译期执行，LAINVM 提供执行
已验证物理 IR 的能力。不得绕过 `#eval` 建立另一套源语言函数求值协议。

compiler core 只组合 SourceApi、AstApi、IrApi、物理执行能力及通用的阶段结果、诊断和生命周期
机制。不得提供 make-effect、make-module 等语言专用宿主原语，也不得按 Meta 返回类别选择
artifact builder 或 VM 执行协议。签名与库内部类型表示用于语义检查；物理执行只处理
Artifact、Procedure、bits/addr/unit、VSpace、预算与 Trap。

本阶段先在 bootstrap 链证明上述边界。`bootstrap/std/` 与 `std/` 共用 Meta ABI 契约，
但不是同一份实现；Lain 语言语义不得通过修改 C seed 实现。正式 lainc 的运行组合仍属编码 5。

### 7.1 当前债务与迁移范围

以下实现虽然位于 `bootstrap/compiler/`，实际属于 `stdlib.l1`，不在 compiler core 中。
`program_collect_meta_call_candidate` 依次尝试 effect operation、module factory、effect
factory 和 scalar function。scalar 路径使用简化表达式 writer；module/effect 路径扫描函数体，
构造引用语法与环境的描述值，并未完整 lower 原函数体；这些路径已有显式 `#eval`，
债务是库语义与生成覆盖不足，不能按目录误判为 core 越界。通用 writer 又通过
`program_function_is_meta` 跳过相关函数。这些是需要替换的启动实现。

剩余阶段边界债务：完整宏绑定、attribute/跨模块展开与正式库的 elaborate/签名语义
仍需补齐；跨阶段失败、非法 payload 与更广的
origin/hygiene 保留继续验收。具体调用与当前实现证据见 1e 部分实现快照。

重点审计并按真实依赖更新闭包：

```text
bootstrap/compiler/meta_call.l1
bootstrap/compiler/meta_call_vm.l1
bootstrap/compiler/meta_eval_vm.l1
bootstrap/compiler/meta_values.l1
bootstrap/compiler/meta_collect.l1
bootstrap/compiler/meta_module.l1
bootstrap/compiler/meta_record.l1
bootstrap/compiler/lower_func.l1
bootstrap/compiler/lower_program.l1
bootstrap/std/entry.l1
bootstrap/std/core_forms.l1
std/meta.lain
std/bootstrap/abi_entry.lain
```

文件移动、函数改名或加一层委托不能作为迁移证据。库实现只能依赖通用能力与库内部规则，
不得通过回调 compiler core 的 effect/module builder 保留原有语义依赖。

### 7.2 新实施顺序

编号使用 1e–1h。每片独立提交、可执行验收后再进入下一片。

| 片 | 工作 | 完成条件 |
| --- | --- | --- |
| 1e：剩余库阶段边界 | 补齐完整宏绑定/attribute、正式库阶段规则与跨阶段失败/资源/metadata 验收 | core 仅保留通用 AstApi；库阶段结果消费、origin/hygiene、诊断及失败资源路径有完整执行证据 |
| 1f：effect 完整迁移 | 在库实现 effect 构造、operation、调用检查和一个 handler 的 lowering；替换对应 compiler 专用路径 | 含 operation/handler 的程序实际运行正确；缺失 effect、签名不匹配等反例有稳定诊断和 span；库不回调旧 builder |
| 1g：显式编译期求值 | 库 Meta 为所需计算生成 `#eval`；通过共同 LAINIR 验证/求值边界执行；库消费物理结果或诊断 | 求值前可验证产物含所需 `#eval`；求值后结果正确、backend 输入不含 `#eval`；Trap 不伪装成功 |
| 1h：其余迁移与清理 | 沿同一边界迁移 module/type 等规则，退役 scalar 最小 writer、函数体扫描分派及专用 artifact builder | 全部行为回归通过；无语言类别驱动的执行协议；无替代命名的专用捷径 |

1f 复用现有生成显式 `#eval` 的物理链，建立真正消费 effect/operation 与 handler 的运行证据。
1g 补齐完整计算覆盖与结果/Trap/生命周期验证；不能因为现有 fixture 返回 42 就认定
effect 行为已被执行。新增路径不得绕过 `#eval`。

### 7.2.1 旧堵塞的重新归属

- `meta_value_kind = 4` 同时用于类型值和 effect：审计其调用者，在库内部确定表示与类型检查
  规则。不得给 compiler core 增加 effect kind 来选择执行协议。
- `formal_meta_effect_factory.lain` 声明 `-> Module` 却返回 effect：由库返回类型规则验证。
  正例签名依据 `std/allocation.lain` 的 `-> effects.Effect`，其中 `effects` 显式导入
  `std::effect`；旧 `-> Module` 保留不匹配反例，不得靠专用分支容忍矛盾。
- 旧 1b-5 的 scalar writer 退役：纳入 1h，由库 lowering 与普通物理 IR 表达式生成覆盖，
  不再按“Meta 函数”类别另建表达式语言。

effect 返回类型不匹配仍被接受，需补齐库侧返回类型校验、诊断 span 与正式侧一致性。
operation/handler 的验收必须运行并消费其结果。
`std::effect("Trap")` 导入失败是当前库收集的兼容缺口；它不决定物理 Trap 的归属。
1f 要把普通 effect operation/handler 与 VM Trap 终止路径分别验收，
并审计 `std/bounds.lain` 对 `effects.Trap` 的包装，避免让该包装成为 VM 的终止协议。

现有 ABI 是否足以传递库阶段结果、生成 IR 与消费 `#eval` 结果，按阶段交接快照与实际调用链验证，不从字段存在推断语义完成。
仅缺通用能力时提出最小契约补充；遇到 §15 的条件仍须最小复现与两个方案，停止相关实现。
本次计划修订已经确定语言语义归属，不再把“库只识别名字还是完整实现 effect”列为待决定项。

局部绑定切片已合入库 lowering，默认编译器十五项运行及独立编译一致性通过。
仍需完成更广的类型环境、成员/调用绑定及正式组合验证，并执行当前完整 47 项门禁。
原型 46 文件闭包通过 verifier 只能证明其产物有效，不能证明正式编译器执行或固定点。
2026-09-15 默认 47 项门禁：1–16 已通过；17/18 的旧 stderr 判定与 CLI
完整诊断输出不兼容，改为精确匹配完整错误码行后分别通过。第 18 项仍有
既有一项正式库检查缺口（畸形 effect 行之前的非法返回类型），不能记作通过。
第 19 项起继续执行，结果记录在 `build/local-address-review/baseline-18-47.json`；
当前尚无全部 47 项通过证据。

类型侧 `meta_function_type_value` 仍使用大写名条件及 `T`/payload 恢复分支，
需统一真实环境绑定，不得把普通值读取修复推断为完整类型参数解析完成。

类型表达式末尾节点修复已进入默认 bootstrap 并通过七项运行探针，完成记录见
[`类型边界归档`](../history/roadmap-lain-type-terminal-2026-09-14.md)。
类型工厂越过 5103 后仍有 5108，工厂普通语义尚未完成。

同一工厂另以 `let Wrapped: std::type = Wrap(i64)` 绑定返回类型时，当前库
在根环境形式识别返回 3101；直接 `Wrap(i64) { x: 42 }` 则在 main 的构造器
调用验证返回 5108。`T`/`Element` 两版结果相同。需打通普通工厂求值、返回
类型绑定和构造调用，不能按返回类别增加另一套执行协议。完整复现保存在
`type-binding-names/factory-alias-*.lain` 与 `factory-*.lain`（均在 `build/`）。

普通编译期函数体反例：`let answer: i64 = compute()`，`compute` 仅
`return 42` 时运行到 42；加入 `let value: i64 = 41; return value + 1`
或带 `if` 的同等函数体时返回 3101。旧 scalar 支持检查只接受 return
起始体，专用 builder 只输出单表达式；需复用普通函数体 lowering，通过
同一 LAINIR `#eval` 执行。独立控制及反例见 `build/local-address-review/comptime-bodies/`。

隔离原型的 scalar 支持检查和体输出改用共同普通 validator/writer，保留
`meta_entry` 的 `#eval` 包装；直接返回、局部声明和 `if` 三版均编译并运行到
42，主产物消费编译期结果。参数、输入行、普通调用依赖及失败诊断仍需
接入共同绑定/类型路径，原型未合入，不能据此宣称 scalar builder 已退役。

参数编号与参数局部化接入隔离普通体原型后，参数更新、循环更新和遮蔽
三项均编译并运行到 42（参数/返回均 i32）。i32 参数直接用于 i64 返回的
探针仍落到 VM artifact verifier 拒绝，共同 validator 的完整返回类型检查
未完成。证据见 `build/local-address-review/comptime-bodies/parameters/`；输入行
与调用依赖仍待接通，不改变 module/effect 的执行协议。

共同体原型的显式 scalar 输入行三项均运行到 42，但负例发现未声明
`?{bias}` 仍会隐式读取调用模块的 bias；原型禁止合入，须先区分定义环境
和显式输入绑定。双调用环境探针另返回 5113，尚未定位，不能宣称环境
隔离通过。复现及结果见 `build/local-address-review/comptime-bodies/inputs/`。

源码追查确认普通操作数校验的 `program_find_constant` 只按名字扫描整个
编译单元的常量链，不检查函数定义环境、命名空间或显式输入行；普通值输出
另外先查函数 Meta 环境，再回退该常量链。下一步须统一校验与输出的绑定来源，
保留定义环境及已声明输入，消除调用模块常量的隐式可见性；不能只屏蔽 bias 负例。

隔离探针将普通操作数校验改为仅查函数 Meta 环境后，未声明输入负例
由成功变为 3101，说明全局常量回退确实影响可见性；三项显式输入正例也
全部返回 3101，确认该改法尚未接通
显式输入行的物理参数，不能作为完整修复合入。候选与结果位于
`build/local-address-review/comptime-bodies/environment-compiler.l1` 和
`inputs/function-environment-results.json`。

继续将显式 scalar 输入行接入普通 operand 校验及 `%输入名` 输出后，三项
正例恢复编译且未声明输入负例保持 3101；运行产物中的 runtime 函数却未
声明该物理输入参数，体内残留 `%bias`，产物执行检查失败。须同时统一
函数签名、调用参数和编译期体输出，不能只补临时 `meta_target` 的输入。
该原型仍未合入；运行诊断记录在 `inputs/function-environment-runtime-results.json`。

进一步在原型函数签名追加显式输入，并由现有 `lainvm_meta_resolve_inputs`
为普通调用解析输入后，三项编译期正例及新增直接调用均运行到 42，未声明
输入负例仍为 3101。该探针调用输出目前只消费 scalar 值，非 scalar 分支及
解析失败的完整诊断/无产物路径尚未实现，不得将它合入或称为统一输入完成。
双环境探针仍返回 5113，定义环境的常量绑定及其他输入类别仍需真实回归。
默认编译器在类型边界修复后通过 `check_meta_pipeline_audit.py` 五项。

双环境 5113 已定位为 `program_collect_constant_candidate` / literal 分支
使用全单元常量链检查重复名字。原型改用 `meta_env_lookup_local` 后，两个
模块的 bias 分别产生 41、43，组合正例运行到 42；独立模块同名常量正例也
运行到 42。同作用域重复声明仍报 5113，同模块重复仍报 3013。证据见
`inputs/constant-scope-results.json` 与 `inputs/function-environment-results.json`。
该修复已独立进入正式 bootstrap 库切片，默认三项作用域正反例、Meta module
validation 与 pipeline audit 通过，见[完成归档](../history/roadmap-lain-constant-scopes-2026-09-14.md)。常量读取的旧全单元回退
不能因重复检查修复而保留为语言可见性规则。

当前普通体/环境隔离原型通过十五项词法绑定运行正例、九项 5108 负例和
独立编译一致性。适配 `check_input_effects.py` 的正例编译入口与负例直调入口
均显式使用该原型后，既有输入行回归及十一项失败诊断通过；这些正例主要
检查编译期折叠结果，不能证明非 scalar 普通 runtime 调用输出已经实现。
适配器见 `build/local-address-review/check_environment_inputs.py`。

新增 Module 输入普通体探针：编译期调用运行到 42，直接调用却在原型中
发布不完整 `#call ...(`，执行解析报 1001；见 `inputs/module-input-body-results.json`。
原型的非 scalar 调用输出分支提前返回而未报告失败，不能以 addr 宿主 handle
常量写入 runtime 产物补洞。`lain_std_emit_program` 也只在输出前读取 status，
未在 writer 后重新收取失败。须将输入与物理参数验证前移至 elaborate，
并验证 lower 失败的返回状态、诊断与产物发布生命周期；不能静默成功。

隔离原型继续为未完成的非 scalar 输出报告临时 5112，并在 writer 结束后
重新读取 unit status，关闭输出后用现有诊断写入路径替换部分文本。
Module 直接调用现编译失败且输出为诊断，原解析损坏产物不再作为成功结果；
scalar/双环境正例保持 42。该临时拒绝不满足非 scalar 调用的完成条件，
诊断归属、编译 API 无产物及捕获/native sink 生命周期仍须验收，原型未合入。

为消除 runtime 产物对编译期 handle 的依赖，新增隔离输入特化探针：调用
沿现有输入解析建立定义环境子环境，将声明的输入绑定为编译期捕获，再用
原 descriptor 物理字段复制生成无输入行的 runtime 特化。Module 直接调用
越过原输出缺口，但 verifier 报 2004（特化调用标签未进入物理过程列表），
须统一函数去重/注册与环境特化键；不能补虚假 extern 或写入宿主地址。
该探针尚无缓存与完整生命周期验收，未合入；见
`build/local-address-review/build_input_specialization.py`。

2004 的直接原因已确认是复制 descriptor 时保留了原函数 linked 标记，
注册因而被跳过。原型清除 linked 后，Module 编译期及直接调用都运行到 42。
仍需特化复用、多调用环境、类型输入、失败路径与递归验收，未合入。

输入特化原型以既有 `program_find_function_in_environment` 复用相同环境键
的 descriptor，修复重复调用的 2021 重复过程错误。Module 重复调用、scalar
runtime 捕获与两个 runtime 调用环境都运行到 42；两个环境分别检查 41、43。
递归探针在 elaborate/收集阶段报 5108，尚需定位；见
`inputs/specialized-call-results.json`。类型输入、错误路径及完整闭包仍待验证。

递归探针改用明确减法 `n - 1` 后运行到 42；此前无空格 `n-1` 未进入减法
表达式路径。生成 IR 中特化 compute 递归调用自身同一标签，未无限创建
新特化。输入特化原型的递归、重复 Module 调用、scalar 捕获和双 runtime
环境四项均通过；类型输入、故障与完整闭包仍未证明完成。

新增 `a:T ?{T} -> T` 的 i32 runtime 调用正例也运行到 42，输入特化原型现
五项通过；仍需不同实际位宽的同函数特化对照及故障/完整闭包验收。
默认类型边界和常量作用域修复后的 `build_formal_stdlib.py` 已完成，生成
正式标准库及 manifest；该构建通过不证明正式编译器运行或固定点。

同函数 i8/i32 两模块 runtime 对照在类型预收集报 5117，尚未到达输入特化。
`program_collect_types_group` 递归进入模块时保持同一 source_index，
`program_collect_type_candidate` 又用 `program_find_type_in_source` 检查同名 T，
因而把独立模块类型当成重复声明。须给类型绑定保留真正词法/模块作用域，
同步迁移类型预收集、别名查找及函数类型解析；不得借用 source_index 当作用域
编号或跳过重复检查。探针见 `inputs/two_type_widths_runtime.lain` 及
`inputs/specialized-call-results.json`。

新增类型作用域原型按 `ast_find_parent(root, binding)` 区分声明的实际父作用域，
重复检查限当前作用域，别名查找向祖先作用域寻找。i8/i32 两模块 runtime
调用分别检查 44、300，组合返回 42；嵌套 Base=i8 遮蔽根 Base=i32 的别名
正例返回 42，同作用域重复类型仍报 5117。特化 IR 两个 idf 签名实际为
bits8 和 bits32。原型仍保留旧全局 alias fallback，尚需迁移完整名字解析；
完整编译器闭包验证已启动，未合入。脚本与结果见
`build/local-address-review/build_scoped_types.py`、`inputs/scoped-type-call-results.json`。

兄弟模块 Private 类型负例原被错误接受：除 global alias fallback 外，
`meta_type_is_nominal` 把大写首字母和点号直接当成合法类型。strict 隔离原型
在类型 alias 收集中改查真实父作用域绑定，移除全局回退及任意大写名豁免，
兄弟模块泄漏及同作用域重复均报 5117；七项别名/类型位宽/调用正例保持 42。
普通作用域版本完整闭包已编译并通过 verifier；strict 版本闭包另行启动，
两者不能混用验收。结果见 `inputs/strict-scoped-types-call-results.json` 和
`build/local-address-review/scoped-types-closure.json`。

strict 完整闭包已结束，编译失败 5117；四项正式可复用作用域回归在 strict
原型通过（`scripts/check_type_scopes.py --compiler ...`），尚未加入主基线。
预收集发生在顶层模块 shells 的预绑定之前，严格解析不能依赖尚未建立的
导入环境；下一步定位具体失败声明，将合法别名的解析安排到环境建立之后，
并保留兄弟模块泄漏的拒绝。不得恢复大写名豁免或全局查找来通过闭包。

已补失败节点诊断并另行启动 strict 闭包定位。独立最小正例证实同一问题：
`types.Item` 经模块绑定 `dep` 的合法别名 `Alias=dep.Item` 在预收集报 5117，
诊断指向 dep.Item，Meta steps=0（模块环境尚未收集）。该正例加入
`check_type_scopes.py`，因此 strict 原型尚不能通过全部作用域验收。
下一步应保留 alias 的 RawAst 与词法环境，待模块绑定建立后解析真实类型，
同步移除 `program_bind_module_type` 对未知名字创建合法 type placeholder 的行为。

诊断版 strict 闭包已结束，首个失败是 `std/allocation.lain` 的 effects.Effect。
延后别名原型在预收集保留未解析声明但不把它绑定为合法 root Meta type，
模块收集改用实际环境解析，并拒绝未解析且不是已知 builtin/constructor 的
名字。`check_type_scopes.py` 五项（含合法 dep.Item）通过；完整闭包另行启动。
未解析 root alias 的最终检查、跨模块前向引用及 metadata 生命周期尚待补齐，
不能凭五项通过合入。实现见 `build/local-address-review/build_deferred_aliases.py`。

延后解析原型补齐根声明的实际环境处理：未知 Missing 编译失败 5117，
合法根 types.Item 运行到 42。默认实现允许本模块前向 Alias=Later，因此原型
在模块收集前先绑定实际 builtin/constructor 类型声明，前向 Later=i32 正例
恢复 42。八项可复用类型作用域检查现已覆盖这些情形；尚须多级别名、循环、
跨模块前向引用及完整闭包验收，未合入。正在运行的闭包使用较早延后版本，
后续须对当前版本另行验收，不能把旧版本结果当成当前实现证据。

别名预绑定原型现按新增绑定反复解析多级链，循环链无法建立类型而报 5117。
前向 Alias=Middle 遮蔽根 Middle=i32 的反例曾运行到 0，确认提前选中了祖先
绑定；原型检查同模块尚未解析的类型声明，等待该声明建立后再读取别名。
修复后本模块 Middle=Later、Later=i8 链运行到 42（run=44）。十一项类型作用域
回归已覆盖链、循环及前向遮蔽，尚未加入主基线；原型仍未合入。

跨模块前向 types.Item 正例在当前延后原型报 5117，新增为第十二项作用域
验收，目前该项未通过。需要预绑定模块里的类型声明后再解析跨模块别名；
同时 `program_collect_source` 用 `meta_env_count > 0` 推断模块已完成收集，
不能让预绑定类型触发该跳过路径。须改用明确收集完成证据，保持 module
成员完整收集、导入缓存与构造中的生命周期，不能以非空环境等同完成。

2026-09-15：跨模块类型预绑定原型恢复前向 types.Item 正例；模块复用条件
改查已存在的 materialized 完成状态，不再以环境非空跳过成员收集。十二项
类型作用域、十五项绑定正例、九项作用域错误及输入行回归/十一项诊断通过。
较早延后版本闭包已结束且产物通过 verifier，但其运行期间共享 bundle 被
后续原型重建，报告只记录结束摘要，不能绑定为当前版本的完整验收。
当前版本使用独立 `alias-lifecycle-compiler.l1` 启动完整闭包，记录开始/结束
摘要及是否一致，结果尚待收取；未合入。后续还须验证完整导入缓存与
构造中状态，不能把 source materialized 状态扩展为任意模块的完成证明。

当前独立 lifecycle 原型的模块类型预绑定后，常量+函数成员、嵌套模块成员
和重复成员调用三项运行到 42，补入 `check_type_scopes.py`（共十五项）。
这些单源执行正例证明成员未因预绑定被跳过；完整导入缓存与构造中失败仍
需另外验收，闭包正在运行。证据见 `inputs/module-lifecycle-results.json`。

新增多文件重复导入正例：两个别名导入同一模块，通过 first.Item 建立根类型
别名，并调用两个别名的函数，实际运行到 42。已加入 `check_type_scopes.py`
第十六项；验证实际类型/成员可用及过程没有重复定义，不证明全部缓存资源
与构造中失败路径。独立结果见 `inputs/imports/result.json`。

导入失败探针揭示未完成路径：经 `run_lain_compiler.py --stdlib-artifact`
重复导入含 Alias=Missing 且返回 Alias 的模块，调用 first.run 仍报告成功，
产物却仅含未定义 `#call run()`。该行为违反成功产物的物理验证要求；须修复
导入成员解析/类型检查并在发布前验证 LAINIR、失败时不保留程序产物，不能
只靠输入行正反例或文本 error 前缀判定成功。复现与输出见
`build/local-address-review/probe_lifecycle_failures.py`、`inputs/import-failures/result.json`。

Python 编译入口的物理验证已实现并通过两项专项、既有输入行回归；完成记录见
[产物验证归档](../history/roadmap-lain-cli-artifact-verification-2026-09-15.md)。
该导入反例现报 2004 并移除输出；仍须修复导入成员语义及编译 API 内部的
验证/诊断，不能以驱动脚本拒绝无效程序取代正确 lowering。

导入失败已定位：`program_materialize_import_target` 两条收集分支在 status
非零时仍设置完成状态 2，并未设置 unit status。隔离修复传播原错误、清除
失败缓存并恢复未收集状态，调用方收取 unit status；Missing 导入反例现
报告 5117。Python 驱动进一步在执行失败/诊断输出时保留 stderr 诊断并移除
输出文件，三项产物专项和输入行回归通过，见[完成归档](../history/roadmap-lain-cli-failure-cleanup-2026-09-15.md)。库修复尚需独立回归与合入；见 `build/local-address-review/build_import_status.py`。

import-status 隔离版本已通过十六项类型作用域回归和既有输入行正反例。
同一 unit 内连续两次 materialize 失败模块，均返回 nil、诊断 5117、缓存状态 0；
执行探针见 `build/local-address-review/check_import_failure_cache.py`。
这证明该反例不复用失败模块，尚不能证明全部循环导入、资源及诊断生命周期。
该版本完整源码闭包验证失败 3101，诊断节点为 std（start=1581），context
source-index=0 与首文件 std/allocation.lain 的长度不符，真实失败来源待定位；
不能直接使用该 context 索引归属诊断。运行前后编译器摘要一致
（首次失败版本 a1e0c0bff808eacc21899d6631fd67e1769b645e44a44affab5d9e1be5a98d82）。
须定位旧导入路径吞掉的收集诊断与真实库声明依赖，不能恢复忽略 status 来通过闭包。
结果见 `build/local-address-review/import-status-closure.json`，当前修复禁止合入。
独立最小复现已确认：只导入 std::effect 并令 main 返回 42，也失败 3101；
按 validation_source 匹配单元来源后，节点索引为 1，对应 std/effect.lain 的
Trap = std::effect("Trap")（start=1581）。库识别了 effect 构造调用，但当前
program_collect_meta_call_candidate 未解析到构造函数；该缺口归编码 1f，
不能通过忽略导入错误或恢复语言专用宿主构造原语绕过。
稳定复现输入见 `scripts/fixtures/formal_effect_import.lain`；编译时同时传入
`std/effect.lain` 与 empty_source.lain。诊断见
`build/local-address-review/effect-import-minimal.json`。
加入来源匹配诊断后的完整闭包仍失败 3101，编译器前后摘要一致
（72eb01f96946a087fdaef6ae4226cf0ba10d3ae953778dd27de381176d19f0b0）；
当前 import-status-closure.json 对应该诊断版本，不再对应首次失败版本。

独立 alias-lifecycle 版本完整编译器闭包已编译且通过 verifier，运行前后摘要
一致（02e3ca39c092c968f2521354cc58448a3c0e87c60bf889b560328e9866de2abc）。
该版本不包含后续 import-status 修复，不能混用验收。仍需正式库构建、完整
阶段/生命周期和诊断检查，所有普通体/类型作用域原型尚未合入。

### 7.3 删除目标

共同普通体隔离版本新增前置失败：`formal_comptime_ordinary_call.lain` 中
calculate 在编译期调用 helper(41)，main 消费 answer，预期运行 42；当前
import-status 原型在临时 vm-artifact-parse 处拒绝 LAINIR。
隔离 trace 源码变体已保存求值前产物，lainir-print 确认 2004：meta_target
调用 helper 的物理标签，但 artifact 只有 meta_target/meta_entry，没有 helper。
产物见 `build/local-address-review/ordinary-call-captured.l1`；trace 只用于定位，
不得作为正式输出路径或绕过验证的实现。
须检查临时 artifact 的被调过程闭包、参数签名和标签，不能把普通体 writer
接入视作普通调用已完成。修复应复用共同物理过程生成及依赖收集，并只纳入
实际依赖，避免无关声明改变编译期计算的可执行性。
隔离 ordinary-dependencies 版本已实现 unit 内的临时依赖队列：共同标签 writer
记录实际目标，共同过程 writer 输出队列并继续收集传递依赖，结束后释放队列。
原 helper 案例、传递调用、递归、重复求值均编译并通过 verifier、运行到 42。
源码生成驱动见 `build/local-address-review/build_ordinary_dependencies.py`，
后三项结果见 `build/local-address-review/ordinary-dependencies-results.json`。
该版本尚未合入；新增 unit 私有槽须审计释放与失败路径，嵌套求值、不同物理
签名及全量回归尚待验收，不能据四个正例宣称共同编译期 lowering 已完成。
扩展探针中，i8 helper 参数截断到 44、i64 调用返回 42、互相递归返回 42，
均通过 verifier。既有输入行正反例和十六项类型作用域回归通过。
函数体局部 bias 供应 helper 的 ?{bias:i32} 仍无法解析；此前继续执行损坏
artifact 而报 parse failure，当前隔离版本在 lowering status 非零时结束 capture、
返回原诊断 5108，并由调用收集器拒绝绑定虚假 scalar 结果。
该修复只完成错误传播，局部编译期值的输入供应语义仍待实现，诊断 source-index
也仍有被其他调试字段覆盖的问题。扩展结果见 ordinary-dependencies-signatures.json。
进一步将本次 invocation 环境用于独立的 lowering 函数描述值，保留原函数
声明环境；临时描述值未进入 unit 过程列表时释放。既有输入行正反例通过，
但 calculate ?{bias:i32} 调用 helper ?{bias:i32} 的输入转发案例仍报 5108。
因此 invocation 环境接入不等于输入转发完成；须检查后续普通过程验证与生成
如何消费调用者声明输入，不能隐式捕获调用者未声明的环境名字。
阶段隔离探针已确认 declared-input 案例的源码收集 status=0、unit status=0，
answer 实际绑定为 scalar 42。完整编译的 5108 出现在之后的阶段；须定位
未特化原函数的验证/生成范围与物理依赖闭包，不能把该失败误归为本次
#eval 没有收到输入。探针见 `build/local-address-review/declared_input_collection_probe.l1`，
执行结果见 declared-input-collection.txt（0 / 0 / 1 / 42）。
当前隔离版本在物理输出中不生成具有非空编译期输入行的原模板，仍由
program_validate 检查原函数体；普通调用继续使用既有捕获特化过程。
declared-input 案例现完整编译、通过 verifier 并运行 42。未调用的合法模板
被接受，未调用模板中 absent 自由变量仍报 5108；既有输入行正反例通过。
该原型尚需完整闭包、模板输出契约及生命周期验收，不代表已合入默认实现。
十项可重复执行验收已进入 `scripts/check_comptime_function_calls.py`：传递调用、
递归、重复求值、i8/i64、互相递归、声明输入转发、未使用模板非法自由变量、
不同输入环境隔离及未声明输入转发拒绝。
显式指定当前 ordinary-dependencies 编译器时全部通过；默认实现尚未满足此
验收，因此暂不登记主 baseline，不能据主门禁通过推断该迁移已经完成。

对应库规则与运行验收通过后，逐项删除或拆出可复用的纯物理部分：

```text
program_collect_meta_call_candidate 中按 effect/module/scalar 分派的逻辑
lainvm_meta_scalar_function_supported
lainvm_meta_write_scalar_operand
lainvm_meta_write_scalar_expression
lainvm_meta_build_scalar_artifact
lainvm_eval_meta_scalar_call
lainvm_meta_module_factory_group
lainvm_meta_build_module_artifact
lainvm_eval_meta_module_call
lainvm_meta_effect_factory_node
lainvm_meta_build_effect_artifact
lainvm_eval_meta_effect_call
lainvm_meta_build_effect_operation_artifact
lainvm_eval_meta_effect_operation
```

`program_function_is_meta` 的跳过规则须由库的阶段处理取代；不得直接删除检查，让未消解的
语言对象泄漏进物理 IR。描述值布局属于库内部表示，迁移时审计全部消费者并保持 ABI 契约，
不把现有 kind/offset 布局升级为 compiler 或 VM 的公共协议。

删除前搜索全部源码引用，排除 `build/` 与 `.git/`；重建产物验证闭包完整。
文本搜索只用于确认删除，不替代行为验证。

### 7.4 验收与完成条件

以下独立检查入口仍待实现，登记到 `scripts/README.md` 与 baseline gate 列表：

- `scripts/check_library_effect_lowering.py`：运行含 effect operation/handler 的正例，验证结果；
  反例验证诊断码、source span；检查库闭包不依赖旧 compiler 专用 builder。
- `scripts/check_lainir_comptime_pipeline.py`：记录求值前后的可验证 LAINIR；覆盖整数计算、
  AST 复制/修改与 origin/hygiene、库内部语义数据的物理表示、含 let/if/普通调用的计算、Trap；
  证明计算确实经过 `#eval`，结果被消费且 backend 输入无 `#eval`。

每个新检查都要有能暴露绕行或错误实现的负对照。成功 artifact 必须经过 verifier，并实际
运行到预期结果；失败不得留下可继续发布的半成品。

保留并执行受改动影响的已有检查与构建：

```text
python scripts/build_lain_compiler.py
python scripts/check_bootstrap_consteval.py
python scripts/check_meta_pipeline_audit.py
python scripts/check_meta_stage_swap.py
python scripts/check_bootstrap_vm_api.py
python scripts/check_meta_form_swap.py
python scripts/check_lainir_boundaries.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
python scripts/check_lainc_lainir_api_baseline.py
```

最终完成条件：替换库 Meta 可改变 effect/module/type 等语言规则而无需修改 compiler core；
core 与 VM 没有语言专用构造或返回类别分派；编译期计算通过 LAINIR `#eval`；求值后 IR 可
独立验证、执行并进入 backend。该证据来自 bootstrap 链，不宣称编码 5 或编译器固定点完成。

## 8. 编码 2 剩余：正式签名语义与形状一致性

### 8.1 未覆盖项

1. 让正式库 elaborate 真正读取显式参数、输入行、返回类型、输出行、body 与声明环境，
   建立并检查完整函数语义；不能仅靠 `FunctionType.input_row` 字段或编译通过证明行为。
2. 将参数/返回类型解析及有效性检查归入库语义阶段，消除对 compiler core unit 类型表和
   descriptor builder 的语言规则依赖。迁移在编码 1e/1h 内安排。
3. 补齐 `check_function_shape_conformance.py` 剩余正式侧 SKIP：非法返回类型与畸形
   effect 行的诊断优先级，需要完整库类型环境。
4. 保持已约定的诊断优先级及 span；全量形状校验搬移不能抢先覆盖返回类型诊断。

Parser 继续使用普通 Atom/Group；箭头是相邻 `-` 与 `>`，不得添加函数/行/类型工厂节点。

### 8.4 剩余验收

- 剩余 SKIP 转为 bootstrap/正式库一致性断言，并补齐诊断优先级与 span 反例。
- 经完整库阶段读取并消费签名，不是只写字段；编码 5 接通后由正式编译器实际编译正反例。
- 以下现有 gate 继续回归，构建成功只证明表示与闭包，不代替正式语义执行。

```text
python scripts/check_meta_ast_conformance.py
python scripts/check_function_signature.py
python scripts/check_function_shape_conformance.py
python scripts/build_formal_stdlib.py
python scripts/build_srclainc.py
```

## 9. 编码 3 剩余：完整库策略输入与类型约束

### 9.1 剩余工作

在库侧定义有明确契约的 `Ord(T)` 与所需成员，打通输入值的成员解析和调用，建立完整正例：

```lain
let max = std::func(a: T, b: T)
    ?{T, ord: Ord(T)}
    -> T {
    return ord.max(a, b);
};
```

上述正例需实际库符号、复合类型与策略值成员调用，并由策略改变运行结果。

在编码 1 的语义迁移中检查现有类型位置推导与位宽匹配是否满足完整库类型规则，补齐约束
冲突、复合类型不匹配和未知宽度的检查边界。不得把“出现于类型位置”或“kind/位宽相同”
当成全部类型约束求解的证明。

### 9.2 保持的语义约束

输入按精确名称在调用者词法环境从内向外查找，最近绑定遮蔽外层绑定；不按类型扫描环境。
所有约束一致才推导成功；无约束或冲突应诊断，不能凭名称大写猜类型。
约束来自显式参数、返回类型、同行类型表达式及 body 已确证的类型约束。

输入在调用前提供，输出 operation 在执行期间发出；输入不自动进入输出行，输出 handler
不自动产生同名输入。调用者缺少所需输入时显式传播或显式建立环境，不自动捕获。

### 9.4 剩余验收

- 完整策略正例编译并运行，覆盖整数类型及至少一次策略值改变结果的因果对照。
- 缺少输入、无法推导、约束冲突、复合类型不匹配、策略成员错误分别检查码与 span。
- 正式编译器的相同用例在编码 5 接通后执行；原有输入行及签名 gate 继续回归。

```text
python scripts/check_input_effects.py
python scripts/check_function_signature.py
```

## 11. 编码 5：接通正式 lainc 的 LAINIR 编译期求值组合

### 11.0 前置决定：provider 与执行能力由谁提供

正式 Lain provider/VM 的归档实现不在构建闭包。先选择并记录运行能力提供方式：

| 路径 | 工作 | 限定 |
| --- | --- | --- |
| A：使用 C seed | 用现有宿主物理能力实现契约适配，组合层提供 LAINIR 编译期求值 handler | 子执行、VSpace、预算、Trap 由宿主承担，必须做真实契约测试 |
| B：重写 Lain provider/VM | 按物理契约实现并实际执行，再组合正式 lainc | 覆盖完整调用路径，不以能编译或通过 verifier 证明可运行 |

选择前不实施本节运行组合。两条路径均不得把 effect 等语言语义加入 seed/VM。

### 11.1 契约归属与现有接口适配

- 先区分 `#eval` 的产物求值接口与 VM/Meta 的编译期地址交付接口；审计
  `bootstrap/compiler/meta_call_vm.l1` 的 `meta_entry` 和 `bootstrap.vm-eval-addr`，
  给地址交付定义 LAINIR 可表述的 VM 边界，再迁移现有 `#eval -> #addr` 混用。
  迁移前不得用该 bootstrap 路径证明 `#eval` 的地址结果契约已经成立。

- 将 `Eval` 从 `src/lainvm/api_contract.lain` 的 ExecutionShape 迁到 LAINIR 求值契约，
  在编译组合边界安装 handler；LAINVM 只保留物理执行原语。
- 审计并适配 `src/lainc/meta.lain` 中现有 `perform Vm.eval(...)`，使用编码 1 形成的
  显式 `#eval` 链；不能把直接 procedure 执行 API 当成另一套编译期语义。
- 审计 `bootstrap.eval_source` / `bootstrap.eval-next` 的传源码、fold、侧信道接口及全部
  消费者；统一到 LAINIR `#eval` 语义，移除无用途的旧接口或明确物理适配职责。
- 处理两个 `api_contract.lain` 的 import 末段匹配歧义；按 §15 先提供最小复现与方案，
  不假定路径前缀能消除冲突。

### 11.2 实现与验收

1. 组合 Memory、IR provider、执行能力、compiler driver 与所需通用 handler。
2. 使用临时执行流，显式传入所需地址值与内存能力并检查生命周期；
   成功恢复产物结果，Trap 转编译诊断，不把调用者 VSpace 当作隐式输入。
3. core 只依赖契约，具体实现由组合层选择；重建正式闭包并检查契约一致性。
4. 新增 `scripts/check_formal_lainc_eval.py`（待实现）：运行生成的正式编译器，编译确实
   需要 AST 处理与编译期计算的程序；记录 `#eval` 求值及结果消费，程序运行结果正确。
5. 实际运行含完整签名、输入行和库 effect 的正反例，完成编码 2/3 的正式行为验收。

现有 `check_lainvm_boundary.py` 只证明源码契约边界，不能替代 handler 被实际执行的证据。
provider 记录、函数名字或构建通过也不能作为运行证明。

现有回归命令：

```text
python scripts/check_lainvm_boundary.py
python scripts/check_bootstrap_consteval.py
```

新增入口实现后必须注册 baseline 并实际执行，明确区分计划项和现有命令。

## 12. 编码 6：建立真正的 Lain 编译器固定点

依赖编码 5 的可运行组合与已验证编译入口。

### 12.1 先定义可执行入口

`src/lainc/lainc.lain` 当前只导出 `lainc.API` factory，不能被 seed 当作命令行编译器入口。
在开始代际构建前，先建立并测试正式组合入口。入口名称、参数和宿主 capability ABI 必须
写入 `src/lainc` 的当前文档和独立 fixture；不得沿用脚本中不存在的
`compiler_compile` 假设。

先查清既有 ABI；建议入口名 `lainc_compile` 尚未确认，名称、参数和能力须有共同契约。
当前 `composed_compiler_sources()` 的 provider/VM manifest 只列契约，未选择运行实现；
须按编码 5 的组合补齐运行闭包，不把库构建清单当成可执行编译器闭包。

### 12.2 修复固定点驱动

新增或重写专用于 Lain 编译器的脚本，例如 `scripts/run_lainc_self_host.py`（待实现）：

1. 依据编码 5 的所选组合建立共同 source manifest，修订闭包驱动后用 bootstrap 得到 gen1；
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

## 13. 编码 7：剩余 backend、native 与发布验收

### 13.1 native 编译器的求值与宿主 ABI

重建当前 bootstrap/backend 产物，区分真实 `#eval` 指令与字符串中的 `#eval`。
backend 输入必须经过求值阶段；残留真实 `#eval` 属编码 1/5/6 的执行链问题，不由后端透传。

审计 manifest 中全部外部能力与 native host 的实现/声明；已有报告指出缺少
`bootstrap.vm-*` 及 artifact capture 适配，确切符号以重建后的产物为准。
只补 prologue 声明会将错误转为未定义链接符号，必须实现完整 ABI 并执行 host 契约测试。

### 13.2 剩余物理构造与语义覆盖

- 浮点运算及跨调用表示/ABI尚需实现和执行验证；支持 float 类型拼写不证明浮点运算正确。
- `#bitcast`、`#proc_addr` 的 C lowering 尚需实现；按规范逐项审计，不以编译器闭包未使用为由省略。
- `#call_indirect` 两个后端覆盖不同；不能用只比较共同子集的差分 gate 证明正确。
  为未共有构造增加独立运行 oracle 或与物理解释器对照，并核验完整物理调用签名。
- 扩大有符号/无符号、窄位宽与转换的边界值覆盖；未知构造必须显式失败且不发布产物，
  不用合法 C 注释或逗号表达式伪装支持。

### 13.3 `#alloca` 的物理语义与生命周期

当前两个 C 后端的分配方式不同：seed 使用零初始化复合字面量，Lain 后端使用 malloc。
需要解决：

1. 先确认规范是否要求零初始化或如何处理未写内存；给出成本和方案，不能用读未初始化
   内存的 fixture 强制引入新语义。
2. 满足规范的 activation 返回时释放全部 alloca，修复 malloc 路径的释放与泄漏问题。
3. 明确 activation 地址不得逃逸的验证/执行边界，覆盖嵌套调用、返回地址与错误路径。

生命周期要求已有规范，不把“是否释放”列为新的语言决定。实现前确认具体物理机制和检查
策略；零初始化尚未规定的部分先讨论。见 [`../01-lain-ir.md`](../01-lain-ir.md) 的内存和地址计算一节。

### 13.5 发布验收

- native compiler matrix：完整源码闭包与程序正反例在生成的 native 编译器上运行。
- source span、Trap source mapping 的执行验证。
- determinism、snapshot、release packaging，从干净 checkout 重建，产物只落 `build/`。
- 真实 CI runner 执行 bootstrap、stdlib、Lain 编译器 fixed-point 与 native smoke。
- 更新 baseline，新增真实 gate，移除失效架构形状断言；明确区分 LAINIR 自举与 Lain 固定点。

物理后端/ABI工作可独立推进；最终 matrix、CI 与发布验收依赖编码 6。

现有回归入口（不代表本节剩余项目已经实现）：

```text
python scripts/check_backend_c_shape.py
python scripts/check_backend_differential.py
python scripts/check_native_backend_migration.py
python scripts/check_native_backend_canonical_diff.py
python scripts/build_default_lainc.py
python scripts/check_lainc_lainir_api_baseline.py
```

差分测试比较两个后端的实际程序结果，仍需独立预期值防止两边同错。对齐问题使用能暴露
错误的构建模式；检查只在已有产物上运行时，先确认其由当前源码重建。

## 14. 每个提交的执行规则

1. 修改前运行 `git status --short`，不要覆盖其他人的未提交文件。
2. 只编辑当前切片列出的源码；发现需要跨层修改时，先说明依赖。
3. 先运行该切片的最小真实测试，再运行阶段验收；不要每次都运行耗时数分钟的全量 gate。
4. 文本搜索只检查禁用名称，不能证明行为正确。
5. 测试产物放入临时目录或 `build/`。
6. `git diff --check` 必须通过。
7. 每个切片保持独立可审查的改动与验收记录，不混入下一切片。
8. 切片完成后写入新的完成快照，再从当前路线图移除；保留历史归档，不覆盖旧快照。

## 15. 必须停止并讨论的情况

出现下列问题时，不得自行补充语言规则：

- `?{}` 是否需要运行时动态输入，而当前编译期 Meta 输入模型无法满足需求；
- 输入环境按名称解析与实际需要的类型导向搜索发生冲突；
- `std::type` 需要同时充当 Module；
- 一个 Meta 值无法通过现有物理 bits、addr 或 unit 传入 LAINVM；
- Trap 需要变成普通返回值才能继续实现；
- 固定点入口需要不同于已确认 ABI 的宿主能力；
- import 末段匹配导致同名契约绑定歧义，需要改变名字解析规则；
- 需要把函数、输入行、类型工厂或 effect 行加入 Parser 语义节点；
- 需要恢复旧裸 `type`、`comptime` 或 generic policy 才能通过测试。

发生这些情况时，应提供最小复现、涉及文件、当前行为和两个可选方案，再请求决定。
