> 历史记录。原路径：`docs/REFACTOR_PLAN.md`。归档日期：2026-09-21。
> 本文保留整理前的内容；其中的状态、命令、语法和结论不作为现行依据。
> 当前文档从 [文档索引](../../../README.md) 阅读。

# seed 的 LAINIR / LAINVM 边界重构计划

状态：计划，2026-09-16。本文规定工作顺序和验收证据；它不表示代码已经迁移。

## 重构完成后，seed 能做什么

### Meta 机制的位置与使用 seed 的路径

Meta 的**语言机制**在 Lain 层：`std/meta.lain` 及其 policy/AST 模块定义正式 Meta ABI 和语言规则；`src/lainc/meta.lain`、`elaborator.lain`、`lower.lain` 组织正式编译器的展开、语义处理和生成 LAINIR。当前启动编译器另有手写的 `bootstrap/std/*.l1` 与 `bootstrap/compiler/*.l1`：前者包含宏等库规则，后者包含 RawAst/AstApi、编译流程以及目前尚未迁完的语言专用分派。`bootstrap/compiler/ast_runtime.l1` 是对 `raw_ast.l1` 的 `lain_ast_v1_*` ABI 包装。正式编译器的 AST Store 在 `src/lainc/syntax.lain`。两套代码不能被当成同一份实现；seed 重构不负责把 bootstrap 的语言分派迁入库，路线图编码 1 单独负责此事。seed 不判断 `let`、type、module、effect 等规则，也不内建 AST 节点语义。

一次 Meta 调用的目标链路如下。每个箭头都需要可测试的接口；bootstrap 和正式编译器的代码分别验收，不用其中一套的通过来证明另一套已经接通。

```text
Lain source -> RawAst
             -> Meta.expand / elaborate / lower（Lain 层选择规则和操作 AST）
             -> LAINIR 可表述的 Meta procedure 与 #extern VM/AST 操作
             -> seed 的 LAINIR parse + verify
             -> seed 的 VM execute（已验证 Artifact、procedure、物理参数、授权）
             -> 物理返回值或 Trap
             -> Meta 按自己的 AST Store/对象协议解释返回引用，继续改写 AST
             -> LAINIR Artifact -> #eval fold -> runtime Artifact
```

这里有两条用途不同的执行路径。**Meta procedure 调用**执行的是已经 lowering、经 verifier 接受的物理 LAINIR procedure；它可以在已授权的 AST Store 中取得对象引用，返回引用留在编译上下文中。此入口不解析或直接解释 Lain 源语言函数，也不为函数/类型/模块再建立一套 seed 求值规则。**`#eval` fold** 是 LAINIR Artifact 的编译期求值阶段，只把可固化的物理结果写回 Artifact。两条路径共用 seed VM 的执行原语和 Trap/授权规则；Meta 对象引用如何通过 LAINIR 物理接口交付，须在第 1 阶段以用例固定，不能凭这段文字视为已设计完成。

Meta 使用 seed 有三个具体入口：

1. **执行 Meta 函数：** Meta 编排方把已 lowering 的 procedure 放在 Artifact 中，经过 LAINIR verifier 后，通过 LAINIR `#extern` 所表示的 VM 执行请求传入物理参数、AST Store 授权和预算；seed 建立 TCB 并执行，交回物理结果或 Trap。
2. **操作 AST：** Meta 函数调用 LAINIR 表示的 AST Store 操作（现有启动 ABI 是 `lain_ast_v1_*` 的 `#extern`/`@foreign`，其 bootstrap 实现在 `ast_runtime.l1`）。这些操作代码或经明确注册的 provider 执行节点读写；seed VM 检查执行请求、内存访问、对象能力的授权和生命周期。节点的语义仍由 Meta 库判断。对象引用可以由 Store 索引/handle 表示，是否以物理 `#addr` 承载须在第 1 阶段确定。
3. **生成运行期 IR：** Meta lowering 交给 LAINIR Artifact 阶段。seed 的 LAINIR provider 验证、fold `#eval` 并输出规范文本；Meta 不通过 VM 私有结构改写 Artifact。

当前正式编译器的 `src/lainc/meta.lain` 直接声明 `Vm.eval` effect，bootstrap 的 `meta_call_vm.l1` 生成 `meta_entry` 中的 `#eval -> #addr`，并用 `bootstrap.vm-eval-addr` 读取结果。它们是第 1、4 阶段需要审计和迁移的现状；不能把“Meta 直接调用一个高层 Vm.eval effect”当作最终稳定接口。最终接口要明确展示 Meta 调用怎样降低为 LAINIR 的物理 procedure、参数和 `#extern`，seed 只实现该物理调用及对象能力。若现有 Lain effect handler 仍用于编排，handler 也须落到这条 LAINIR 可表述的协议，不能绕过它。

验收至少包括：一段 Meta 宏创建带 origin/hygiene 的 AST 节点、另一段 Meta 函数读取并改写节点；记录其 LAINIR 调用文本、seed provider 入口、物理返回值和对象引用的生命周期；用无 AST Store 授权的调用证明 Trap；证明这些对象引用不出现在 backend 输入中。再用一个普通 `#eval` 例子证明两条路径的结果交付确实分开。

seed 是自举用的 C 实现，不是 Lain 源语言语义的定义者。完成后它提供两组可组合的能力：

| 消费者 | seed 提供的能力 | 输出 |
| --- | --- | --- |
| 编译器的 LAINIR 阶段 | 解析、验证、执行并消除 Artifact 中的 `#eval`、重新验证、规范文本输出 | 不含真实 `#eval` 的物理 Artifact，或带位置的诊断 |
| 编译器的 Meta 阶段 | 经 LAINIR 可写的 VM 调用执行一个已验证过程；在明确的对象协议下交付编译期对象引用 | 物理返回值或 Trap；对象引用留在编译上下文中 |
| 解释器/自举驱动 | 给已验证 Artifact 的过程传入物理参数和授权，启动/分片执行 TCB，读取返回值或 Trap | 执行结果、阻塞/耗尽状态或 Trap |
| backend 与工具 | 查询已验证 Artifact 的物理结构；消费已完成的规范文本 | IR 结构或文本，不依赖 VM 内部布局 |

`#eval` 的结果写入 Artifact 时只接受可在运行期独立解释的结果。`#addr` 可以是一次 VM 过程调用的返回值；临时 activation 地址、编译期对象引用和其他只有编译上下文才有效的地址都不能变成 `#eval` 的产物常量。无地址输入的 TCB 可使用本次执行创建的 `#data`/`#alloca` 区域；它没有调用者的 VSpace。文件、控制台、Meta 对象存储等外部能力必须显式授权。

不以“通过 C 函数返回一个裸 `void *`”作为内存授权。每个跨执行边界的地址须伴随可检查的区域/对象授权、访问权限与生命周期；VM 对 `#lea`、`#load`、`#store` 和外部调用参数做检查。物理 `#addr` 的数值本身不赋予权限。

## 对外接口：目标契约，名称和 C 布局待第 1 阶段固定

接口分为 **C provider ABI** 与 **LAINIR 调用 ABI**。下列签名是供第 1 阶段制作 contract test 的目标形状；不是已存在的函数，也不是未经验证就冻结的 ABI。正式定稿时逐项记录头文件、所有权、错误码和 LAINIR extern 签名。

### C provider ABI

| 接口组 | 建议公开操作 | 调用方与责任 |
| --- | --- | --- |
| Artifact (`lainir/artifact.h`) | `parse(bytes, diagnostic) -> Artifact`、`verify(Artifact, entry?, diagnostic)`、`emit_canonical(Artifact, writer, diagnostic)`、`release(Artifact)`、只读 procedure/data/类型查询 | LAINIR 工具、编译器与 backend；Artifact 持有解析树，查询值仅在其生命周期内有效 |
| Eval fold (`lainir/eval_fold.h`) | `fold_evals(Artifact, VmExecutor, authority, diagnostic) -> FoldStatus` | LAINIR 阶段遍历 `#eval`，构造执行请求、验证返回类型、替换节点、重新验证；失败不发布部分 Artifact |
| VM 执行 (`lainvm/execute.h`) | `execute(VerifiedArtifact, Procedure, PhysicalArgs, ExecutionAuthority, Budget) -> PhysicalValue | Trap`、`execute_child(parent, narrowed_authority, narrowed_budget, ...)` | LAINIR fold、Meta 的物理调用适配器、自举驱动；VM 不解析源语言、不改写 Artifact |
| VM 控制 (`lainvm/control.h`) | TCB `create/start/run_slice/suspend/resume/take_result/take_trap/release`；Endpoint `send/receive/cancel` | 调度与执行控制；TCB 不拥有 VSpace，对象为 opaque handle |
| 授权 (`lainvm/authority.h`) | 建立/收窄/撤销执行授权：可访问区域、权限、外部 capability 集合、预算 | 宿主创建根授权；子执行只能收窄；地址值和授权分别传递 |

目标输入/输出的语义应具体到以下字段：`VerifiedArtifact` 指向经当前 verifier 接受且在执行期不可变的物理 IR；`Procedure` 必须属于该 Artifact；`PhysicalArgs` 每项携带类型/位宽和值；`ExecutionAuthority` 指明区域或 VSpace view、read/write 权限、CSpace 与生命周期；`Budget` 至少限制 steps、call depth、allocation bytes 和 eval 数；返回为已声明的物理类型及值，或带分类、过程位置和 source span 的 Trap。C 指针只用于 provider 内部持有对象；跨 LAINIR 边界不能靠裸 C 指针推断这些字段。

当前对应的入口是 `parse.h`、`verify.h`、`emit.h`、`interpreter.h`、`eval_source.h` 中的 `lainir_module_*`、`lainir_fold_module*`、`lainir_eval_block`、`lainir_run` 及 `lainir_vm_control_*`。迁移时先做调用方清单和兼容适配，逐项替换；不因文件搬迁就改动公开语义。最终 `interpreter.h` 不再同时公开 LAINIR fold 和 VM execute，`eval_source.h` 不再将“fold 后 Artifact”与“直接运行 procedure”作为同一无权限请求。

### LAINIR 调用 ABI：VM 操作怎样写在 IR 里

编译器生成的 IR 通过受 CSpace 管控的 `#extern #proc` 调用 VM provider；callee 名、物理参数和返回值均由 LAINIR 表述。如下是目标形状的示意，`#addr` handle 只能作为 VM 验证过的临时句柄，不能被 `#load/#store` 当作内存使用：

```lain-ir
#extern #proc vm.artifact_parse(#addr %bytes, #bits<64> %length) -> #addr;
#extern #proc vm.procedure_find(#addr %artifact, #addr %name, #bits<64> %length) -> #addr;
#extern #proc vm.arguments_new() -> #addr;
#extern #proc vm.arguments_append_bits(#addr %args, #bits<64> %value, #bits<64> %width) -> #unit;
#extern #proc vm.arguments_append_addr(#addr %args, #addr %value, #addr %grant) -> #unit;
#extern #proc vm.request_new(#addr %artifact, #addr %procedure, #addr %args,
                            #addr %authority, #bits<64> %steps) -> #addr;
#extern #proc vm.execute(#addr %request) -> #addr;
#extern #proc vm.result_kind(#addr %result) -> #bits<32>;
#extern #proc vm.result_bits(#addr %result) -> #bits<64>;
#extern #proc vm.result_addr(#addr %result) -> #addr;
#extern #proc vm.result_trap(#addr %result) -> #addr;
#extern #proc vm.result_release(#addr %result) -> #unit;
```

这里的 `vm.execute` 返回的是 **执行结果句柄**，不是源语言的 Meta 值。调用者按已验证 procedure 的物理返回类型读取结果；Trap 是独立终态，不能从 `result_bits`/`result_addr` 读作成功。结果句柄与 Artifact、参数向量、授权句柄均有显式释放或上下文托管规则。`vm.arguments_append_addr` 的 `%grant` 说明这个地址的可用范围和权限；对象 handle 若不支持内存访问，只能传给被授权的对象操作 extern。正式接口可改用几个定型的结果入口，但不能再由 Meta 返回类别选择不同执行协议。

LAINIR 自身的 `#eval` 是 Artifact 中的原语；fold 调用上述 VM 执行能力后替换 `#eval`，不把 VM 控制对象编码成新的 LAINIR 指令。Meta AST 操作通过对象协议的 `#extern #proc meta.*` 或可执行的 Meta procedure 表达，其物理参数/返回值由同一个 VM 机制运送。**Meta 对象引用的 LAINIR 物理编码尚未定稿**：若 `#bits/#addr/#unit` 加上授权句柄仍不足以表达它，依仓库路线图 §15 给出最小复现和两个方案，先讨论再改指令语义。

### 对外 CLI 与宿主入口

`lainir-print` 继续提供解析/验证/规范文本能力；`lainir-seed` 继续提供启动与执行能力；`lainir-vm-control-test` 验证控制面。`native_compiler.c`、`native_lainc.c` 和 Python 自举脚本仍以 seed provider 链接。名称是否调整由第 5 阶段决定；验收以输入输出行为和链接闭包为准。`bootstrap.*` 是启动期宿主 capability，不是稳定的通用 VM ABI；迁移后编译器应使用明示的 VM/Meta 调用契约。平台 I/O 仍由注入的 capability 提供，seed 不内建 Lain 的模块、类型或 effect 规则。

## 用例：接口应当回答的具体问题

1. **普通 `#eval`：** `#eval { #return #add(20, 22) }`。LAINIR verifier 接受物理返回类型；fold 请求 VM 执行，取得 `#bits` 结果 42，写回常量并再验证；规范文本中没有该 `#eval`。
2. **执行过程并读取 Meta 引用：** Meta procedure 在被授权的 AST Store 中创建节点，返回对象引用。VM 返回物理值，调用方用 Store 协议解释引用并继续改写 AST；这个值不经 Artifact 的 `#eval` fold，也不写入运行期 Artifact。
3. **传入外部地址：** 根调用方创建一个只读区域授权，把地址值与 grant 一同放入执行请求；过程 `#load` 成功、`#store` 产生权限 Trap。只给数值不授予 grant 时两种访问都被拒绝。
4. **activation 地址逃逸：** 过程返回 `#alloca` 的地址，VM 在跨 activation 交付时给出生命周期 Trap；不能靠折叠为 `#data_addr` 或把宿主指针写入 Artifact 绕过。
5. **子 TCB：** 根请求含预算 100 steps、只读区域 A；子请求上限 30 steps、区域 A 的一段只读 view。子执行耗尽或越权分别返回稳定 Trap，父请求的授权不被扩大。
6. **失败原子性：** 第二个 `#eval` Trap 时，编译器不发布已经折叠了第一个 `#eval` 的部分 Artifact；diagnostic 指向失败的 IR/source 位置。

第 1 阶段先把这些用例写成 contract test，并据此固定签名；第 2 阶段验证拆文件不改变既有可观察行为；第 3、4 阶段按新授权与对象协议更新用例。第 5 阶段同时测试 CLI、C API、LAINIR extern ABI 三种消费者，不能只凭文本搜索声称接口稳定。

## 目标与已确定的边界

seed 是 C 启动实现，同时承载 LAINIR 和 LAINVM；目录名不决定架构归属。

- LAINIR 负责解析、验证物理 IR，定义 `#eval` 的编译期求值语义，并在求值后改写产物、消除 `#eval`。
- LAINVM 负责执行已验证 IR：TCB 状态、activation、预算、Trap、capability 与地址访问检查。VM 返回物理执行结果或 Trap，不改写 IR。
- TCB 是执行上下文，不拥有 VSpace，也不因执行 `#eval` 隐式取得调用者的地址空间。执行请求显式传入所需的物理参数、地址值及对应的内存能力；没有授予的外部地址不能被使用。
- Meta 操作 AST 和语义对象。它需要对象引用，引用的物理表示不必是 `#addr`。编译期对象引用与地址生命周期的交付属于 VM 执行接口和 Meta 对象协议，并须经 LAINIR 可表述的边界；`#eval` 不承担该交付。
- `#eval` 结果不能携带仅在编译期有效的 `#addr` 到运行期产物。结果类型、静态验证和失败诊断须在改变执行代码前定成一份可测试的契约。不得凭现有 `#data_addr` 路径推定原始地址设计。

## 当前混用的最小定位

| 位置 | 当前行为 | 应归属 |
| --- | --- | --- |
| `src/interpreter/interpreter.c` 的 `interp_eval_block` / `lainir_eval_block` | 建临时执行流、执行块、交付结果或 Trap | LAINVM |
| 同一文件的 `fold_value_expr` / `fold_expr` / `lainir_fold_module` | 遍历、执行并替换 `EXPR_EVAL`，消除节点 | LAINIR |
| `src/interpreter/eval_source.c` | 混合 parse/verify/fold/emit 与直接运行 procedure 的入口 | 拆成 LAINIR 产物管线和 VM 执行适配 |
| `include/lainir/interpreter.h` | 同时声明执行、求值和 fold，fold 注释与现有代码不符 | 按两个公开边界拆分并修正注释 |
| `bootstrap/compiler/meta_call_vm.l1` 的 `meta_entry` | 用 `#eval -> #addr` 交付 Meta handle | 待迁移的 VM/Meta 交付路径 |

当前 seed 在 `#eval` 子执行器中复制调用者参数和局部值，并沿用调用者的 `caps`。现有测试证明这条旧路径可运行，不证明显式执行输入的边界已实现。

## 实施顺序

实施时每阶段提交一张调用方清单与一份行为报告。报告列出新增/移除的公开函数、C 链接目标、LAINIR extern 名称、正反用例、首个失败点；不把“已有脚本退出 0”当作新接口的全部证据。机械拆分与语义变更分开提交，便于区分链接故障和权限故障。

### 1. 先固定两个相邻契约

具体交付物：`lainir/artifact.h`、`lainir/eval_fold.h`、`lainvm/execute.h`、`lainvm/authority.h` 的接口草案及 contract test；一份 LAINIR `#extern` 对应表；物理参数、结果句柄、Trap、授权 grant 的所有权表。这里先通过最小程序确认 `#addr` 在执行请求中如何对应区域授权，以及 Meta 对象引用是否能由现有物理值承载。若不能，停止 seed 语义修改并按路线图 §15 提出两个可执行方案。

写出 LAINIR 求值阶段提交给 VM 的执行请求：已验证的 Artifact/块、物理参数、显式地址输入、内存及外部能力授权、预算。VM 的响应只含物理值或 Trap；请求不得携带 Meta 对象类别，TCB 不隐式持有 VSpace。另写 `#eval` 的合法结果类型、求值后的产物表示及失败规则。用一个无地址输入的例子、一个显式传入地址和权限的例子、一个权限拒绝的例子校验该契约。

同时定义编译期 Meta 对象引用的交付协议属于哪条 LAINIR 可表述的 VM 接口。该协议与 `#eval` 产物求值分开。若现有物理类型或能力边界无法表达它，提供最小复现和两个方案，先讨论再改 seed 语义。

验收：契约文档和独立行为用例明确区分“VM 内部可返回地址值”与“`#eval` 可留在产物中的结果”；任何失败产生稳定诊断或 Trap，不伪装成普通结果。

### 2. 保持行为的机械拆分

先给 `fold` 和 `execute` 各建独立目标，把当前 `eval_source.c` 的编译产物入口和直接执行入口移到各自边界；逐个迁移 `print_main.c`、`bootstrap.c`、`native_compiler.c`、`native_lainc.c`。C provider 的依赖方向为 `parse/verify/Artifact -> eval_fold -> VM execute`；VM execute 可依赖已验证 Artifact 的只读视图，不回调 fold 或 emitter。比较拆分前后同一组成功输出、规范文本、错误码和 Trap 位置。

将 `fold_value_expr`、`fold_expr`、`fold_block`、求值类型传播、节点计数和 `lainir_fold_module*` 从 `src/interpreter/interpreter.c` 移到 LAINIR 侧源码，例如 `src/core/eval_fold.c`。将 `lainir_eval_block` 与 TCB 执行留在 VM 侧。新增分开的头文件；`eval_source.c` 的 parse/verify/fold/emit 入口调用 LAINIR fold，直接运行 procedure 的入口调用 VM。先保持可观察行为，避免在搬文件时同时改变 `#eval` 语义。

更新所有显式 C 源码闭包：`seed/build.zig`、`scripts/run_lainir_self_host.py` 的 `SEED_C_SOURCES`、`scripts/build_lainc_native.py` 的 `IN_PROCESS_SOURCES`，以及实际引用这些函数的 CLI/host 目标。检查 `lainir-print`、`lainir-seed`、native 编译器和自举驱动各自所需的链接依赖；不要为了链接方便把 fold 重新并回 VM 库。

验收：`python scripts/build_seed.py`、`python scripts/check_lainir_api_behavior.py`、`python scripts/check_eval_tcb.py`、`python scripts/check_lainir_physical_safety.py`、`python scripts/check_lainir_compiler.py` 和 `build/seed/bin/lainir-vm-control-test` 全部通过；机械拆分前后的规范文本和失败诊断一致。

### 3. 实现显式执行请求

先完成区域登记和 grant 检查，再改变执行入口与子 TCB 继承规则，最后接入 `#extern` capability 的参数/结果检查。每次调用记录输入 Artifact、入口、参数、授权 view 与预算；返回值按物理类型及生命周期检查。CLI 和宿主调用方逐一改为创建明确的根授权；新执行入口不接受旧式“只给 caps 就隐式取得调用者内存”的请求。需要在迁移中保留的旧函数只作为拆文件前后行为对照，不能进入最终公开 ABI 或被编译器继续调用。

按第 1 步的契约替换 `lainir_eval_block(..., caps, ...)` 中含糊的输入。VM 接收独立的执行请求；参数/地址按值传递，内存能力单独授权；TCB 不从调用者继承 VSpace。嵌套执行不能扩大授权和预算。VM 在地址访问、外部调用和返回处检查能力与生命周期。LAINIR fold 只负责调用、消费结果和改写，不直接读取 VM 的 TCB/VSpace 内部结构。

验收：无显式授权时外部地址访问被拒；显式传入有效地址及能力时计算成功；嵌套调用仍遵守收窄后的授权；Trap 记录稳定分类。旧 `check_eval_tcb.py` 用例在此阶段更新为新契约用例，不以旧行为继续通过作为完成证据。

### 4. 拆掉 bootstrap 的地址交付混用

审计生成的 `meta_target`/`meta_entry` 文本、`bootstrap.vm-eval-bits/addr/unit` 及 `vm-artifact-*`、`vm-arguments-*` 消费者。先使普通过程调用走统一 `vm.execute` 结果协议，再使 Meta 对象引用经对象 Store 的授权协议交付，最后移除为引用交付生成的 `#eval -> #addr`。检查编译上下文关闭后的引用失效、对象操作 extern 的授权拒绝、异常返回时句柄释放，以及 backend 输入中不存在真实 `#eval`。

审计 `meta_entry`、`bootstrap.vm-eval-addr` 和所有消费者。Meta 对象引用经第 1 步确定的 LAINIR→VM 接口交付；需要形成产物值的计算继续使用 `#eval`。删除用 `#eval -> #addr` 交付编译期 handle 的路径，同时验证对象引用仍在所属编译上下文内有效、不会被写入运行期产物。只修改真实缺失的物理机制；语言规则仍留在 Meta 库。

验收：至少一个 Meta AST 创建/改写用例能取得并消费对象引用，且没有用 `#eval` 返回 `#addr`；至少一个普通 `#eval` 用例在 backend 前被消除；两条路径各有失败用例。

### 5. 收束与发布门禁

交付 `seed/README.md` 的目标依赖图、公开 ABI 表、CLI 使用说明和迁移记录；删掉已无调用者的旧函数及 capability 名。对 C 头文件编译、每个 Zig 静态库/可执行目标、Python 脚本中手写的 C 源码列表、LAINIR extern 链接做一致性检查。将三个接口消费者的 contract test 纳入常规门禁，再跑自举固定点；固定点失败须标出首个不同的 Artifact/procedure，而不只报告“输出不同”。

修正 `seed/README.md` 的依赖图和文件职责、公开头文件的注释，以及同一契约在 LAINIR/LAINVM 规范中的表述。运行 seed 构建、规范文本/验证、VM 控制面、物理安全、`#eval` 和自举固定点的相关门禁。记录首个失败点和具体层次；只有行为用例证明接口一致后才删除旧适配。

完成条件：LAINIR 的 fold 代码与 VM 执行代码有单向、明确的调用边界；执行请求无隐式 VSpace；Meta handle 不经 `#eval` 交付；所有进入 backend 的 IR 不含真实 `#eval`；seed 的 C 源码闭包和相关门禁通过。
