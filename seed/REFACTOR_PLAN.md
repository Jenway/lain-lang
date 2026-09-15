# seed 的 LAINIR / LAINVM 边界重构计划

状态：计划，2026-09-16。本文规定工作顺序和验收证据；它不表示代码已经迁移。

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

### 1. 先固定两个相邻契约

写出 LAINIR 求值阶段提交给 VM 的执行请求：已验证的 Artifact/块、物理参数、显式地址输入、内存及外部能力授权、预算。VM 的响应只含物理值或 Trap；请求不得携带 Meta 对象类别，TCB 不隐式持有 VSpace。另写 `#eval` 的合法结果类型、求值后的产物表示及失败规则。用一个无地址输入的例子、一个显式传入地址和权限的例子、一个权限拒绝的例子校验该契约。

同时定义编译期 Meta 对象引用的交付协议属于哪条 LAINIR 可表述的 VM 接口。该协议与 `#eval` 产物求值分开。若现有物理类型或能力边界无法表达它，提供最小复现和两个方案，先讨论再改 seed 语义。

验收：契约文档和独立行为用例明确区分“VM 内部可返回地址值”与“`#eval` 可留在产物中的结果”；任何失败产生稳定诊断或 Trap，不伪装成普通结果。

### 2. 保持行为的机械拆分

将 `fold_value_expr`、`fold_expr`、`fold_block`、求值类型传播、节点计数和 `lainir_fold_module*` 从 `src/interpreter/interpreter.c` 移到 LAINIR 侧源码，例如 `src/core/eval_fold.c`。将 `lainir_eval_block` 与 TCB 执行留在 VM 侧。新增分开的头文件；`eval_source.c` 的 parse/verify/fold/emit 入口调用 LAINIR fold，直接运行 procedure 的入口调用 VM。先保持可观察行为，避免在搬文件时同时改变 `#eval` 语义。

更新所有显式 C 源码闭包：`seed/build.zig`、`scripts/run_lainir_self_host.py` 的 `SEED_C_SOURCES`、`scripts/build_lainc_native.py` 的 `IN_PROCESS_SOURCES`，以及实际引用这些函数的 CLI/host 目标。检查 `lainir-print`、`lainir-seed`、native 编译器和自举驱动各自所需的链接依赖；不要为了链接方便把 fold 重新并回 VM 库。

验收：`python scripts/build_seed.py`、`python scripts/check_lainir_api_behavior.py`、`python scripts/check_eval_tcb.py`、`python scripts/check_lainir_physical_safety.py`、`python scripts/check_lainir_compiler.py` 和 `build/seed/bin/lainir-vm-control-test` 全部通过；机械拆分前后的规范文本和失败诊断一致。

### 3. 实现显式执行请求

按第 1 步的契约替换 `lainir_eval_block(..., caps, ...)` 中含糊的输入。VM 接收独立的执行请求；参数/地址按值传递，内存能力单独授权；TCB 不从调用者继承 VSpace。嵌套执行不能扩大授权和预算。VM 在地址访问、外部调用和返回处检查能力与生命周期。LAINIR fold 只负责调用、消费结果和改写，不直接读取 VM 的 TCB/VSpace 内部结构。

验收：无显式授权时外部地址访问被拒；显式传入有效地址及能力时计算成功；嵌套调用仍遵守收窄后的授权；Trap 记录稳定分类。旧 `check_eval_tcb.py` 用例在此阶段更新为新契约用例，不以旧行为继续通过作为完成证据。

### 4. 拆掉 bootstrap 的地址交付混用

审计 `meta_entry`、`bootstrap.vm-eval-addr` 和所有消费者。Meta 对象引用经第 1 步确定的 LAINIR→VM 接口交付；需要形成产物值的计算继续使用 `#eval`。删除用 `#eval -> #addr` 交付编译期 handle 的路径，同时验证对象引用仍在所属编译上下文内有效、不会被写入运行期产物。只修改真实缺失的物理机制；语言规则仍留在 Meta 库。

验收：至少一个 Meta AST 创建/改写用例能取得并消费对象引用，且没有用 `#eval` 返回 `#addr`；至少一个普通 `#eval` 用例在 backend 前被消除；两条路径各有失败用例。

### 5. 收束与发布门禁

修正 `seed/README.md` 的依赖图和文件职责、公开头文件的注释，以及同一契约在 LAINIR/LAINVM 规范中的表述。运行 seed 构建、规范文本/验证、VM 控制面、物理安全、`#eval` 和自举固定点的相关门禁。记录首个失败点和具体层次；只有行为用例证明接口一致后才删除旧适配。

完成条件：LAINIR 的 fold 代码与 VM 执行代码有单向、明确的调用边界；执行请求无隐式 VSpace；Meta handle 不经 `#eval` 交付；所有进入 backend 的 IR 不含真实 `#eval`；seed 的 C 源码闭包和相关门禁通过。
