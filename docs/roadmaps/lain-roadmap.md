# Lain 当前路线图

基线日期：2026-09-09。

本文只追踪尚未完成的工作。已经完成并有可执行证据的内容归档到
[`../history/`](../history/)；历史文档不定义当前架构。

C0 的完成记录见
[`../history/roadmap-eval-c0-2026-09-09.md`](../history/roadmap-eval-c0-2026-09-09.md)。

C1 的完成记录见
[`../history/roadmap-eval-c1-2026-09-09.md`](../history/roadmap-eval-c1-2026-09-09.md)。

C0.5 的完成记录见
[`../history/roadmap-project-structure-c0.5-2026-09-09.md`](../history/roadmap-project-structure-c0.5-2026-09-09.md)。

C2 的完成记录见
[`../history/roadmap-eval-c2-2026-09-09.md`](../history/roadmap-eval-c2-2026-09-09.md)。

C3 的完成记录见
[`../history/roadmap-eval-c3-2026-09-09.md`](../history/roadmap-eval-c3-2026-09-09.md)。

当前最高优先级是纠正 `#eval` 与 LAINVM 的实现方向。正确性优先于兼容性：错误的
抽象直接删除，允许旧 Meta 和自举链在迁移期间暂时不可用。

## 已确认的设计决定

1. `#eval` 是 LAINIR 的编译期执行表达式。正常结束时返回块所声明的普通 LAINIR 值；
   执行失败时由 LAINVM 产生 Trap。
2. 每次 `#eval` 使用一个临时 TCB。TCB 保存这次计算的执行位置、调用过程、挂起状态和
   Trap 状态。
3. 临时 TCB 默认共享调用者的 VSpace。VSpace 是执行流能够访问的地址空间；创建 TCB
   不意味着创建新的 VSpace。
4. LAINVM 只处理 `#bits<N>`、`#addr`、`#unit` 等物理值。类型、模块和 AST 的含义属于
   Meta，不能进入 LAINIR 或 LAINVM 的返回协议。
5. `EvalResult` 抽象被禁止。不得用 status、kind、scalar、object、owner、generation 或
   sidecar 组成一套 `#eval` 返回包装。
6. Trap 与普通返回值分开。解释器可以在宿主调用边界使用执行报告来区分成功和 Trap，
   但该报告不属于 `#eval` 的值语义，也不能携带 Meta 分类。
7. `#eval` 必须在交给最终 backend 前执行并从产物中消失。
8. compiler bundle、snapshot 和 native executable 都是构建产物，只能写入 `build/`。
   `bootstrap/` 只保存手写 LAINIR 启动源码，`src/` 只保存正式实现源码。
9. 项目按 `seed -> bootstrap -> src` 分层：`seed` 是最小可信执行底座，`bootstrap` 是
   启动正式编译器所需的手写 LAINIR 源码，`src` 只放用 Lain 编写的正式实现。
   正式实现继续分为 `src/lainc`、`src/lainir` 和 `src/lainvm`；LAINIR 定义并验证
   物理程序，LAINVM 执行已经验证的物理程序。
10. `src/lainvm` 是正式实现中的独立运行时层。它拥有 TCB、VSpace、执行预算、Trap 和
    过程执行；`src/lainir` 只拥有 IR 的构造、验证、打印和物理语义；`src/lainc` 只拥有
    源语言与 Meta 的编译工作。lainc 只能通过 LAINVM 的执行接口约定（API contract）请求执行，不能导入
    解释器状态或管理 TCB。

## 运行时分层与验收边界

`src/lainvm/` 不是 `src/lainir/` 的一个工具目录。它是将来可以有参考解释器、native
lowering 和裸机 lowering 的共同语义所有者。当前的 Lain 解释器已经在该目录中；C seed
保留为最小可信执行底座。

| 层 | 负责的内容 | 不得负责的内容 |
| --- | --- | --- |
| `src/lainir` | IR 模型、builder、verifier、printer、物理指令语义 | 执行器、TCB、VSpace、Meta 值 |
| `src/lainvm` | 执行已验证的 IR、TCB、VSpace、Trap、预算、`Eval` operation | 源语言类型、模块与 AST 的解释 |
| `src/lainc` | 解析、Meta、elaboration、lowering，以及发起 `Eval` 请求 | 解释器内部状态、TCB 创建和 VSpace 管理 |

执行接口约定（API contract）是三层之间唯一的执行接缝。它只公开 artifact、procedure、物理 `Value`、参数
向量和 `Eval` operation；不公开解释器结构体，也不把类型、模块或 AST 包装成 VM 返回值。
每次改变该 contract，都必须同时验证：LAINIR provider 不导入 LAINVM，LAINVM 不导入 lainc，
lainc 不依赖 LAINVM 的私有实现。

## 当前进度

| 领域 | 当前状态 | 下一步 |
| --- | --- | --- |
| LAINIR 物理语义 | 基线可用 | 保持物理边界，不加入 Meta 返回分类 |
| LAINVM 基础 | C seed 与 Lain 源码边界均已采用 TCB、VSpace、Trap 和预算模型；Lain VM 已独立为 `src/lainvm/`，并已公开执行 API contract 与 `Eval` operation | 将 lainc 工厂和编译入口接到该 contract 与 handler |
| `#eval` | C seed 已通过临时 TCB 执行并在 fold 前消除；Lain VM 已提供子 TCB 原语；lainc Meta 已改为发出 `Vm.eval` effect | 在正式编译器执行入口安装 handler，并运行真实编译 fixture |
| Meta 编译期求值 | `Ir.Eval`、`Eval.Result` 和伪造的 status/value 返回已经从正式 Meta 源码删除；语法 callable 已改用 LAINVM 物理 `Value` | 接通 handler 后，从整数与基础物理运算开始恢复真实执行 |
| 自举 | 启动源码仍在；旧生成产物已移出源码树，当前 C4 未完成所以暂时不能重建 | C4 完成后在 `build/bootstrap/` 重新生成并恢复固定点 |
| 项目结构 | `seed`、`bootstrap` 与 `src` 已分离；`src/lainvm/` 已拥有执行实现 | 保持职责边界并在 C5 恢复新自举 |
| C backend 与发布 | 非当前主线 | 纠偏完成后继续收口和 CI 验证 |

当前实施顺序：

```text
C4 Meta 重新通过 #eval 执行编译期计算
  -> C5 恢复自举并替换冻结产物
```

## C4：让 lainc 通过 LAINVM 重新建立 Meta 编译期求值

目标：让 Meta 生成 `#eval`，由 lainc 通过 LAINVM API contract 发起执行，且由 LAINVM 的
effect handler（接收 `Eval` 请求并启动子 TCB 的代码）在子 TCB 中执行；不修补旧 `EvalResult`
求值器。

先完成运行时接线，再恢复语言能力：

1. **C4.1：固定执行接缝。** 已完成 LAINVM `Eval` effect operation 和 API contract；
   该 contract 只表达物理参数与物理返回值。
2. **C4.2：接通编译入口。** `Vm` capability 已传入 lainc 的 compiler、API、driver 和
   Meta factory；Meta 已通过 `perform Vm.eval(...)` 发出执行请求。剩余工作是在执行正式
   编译器的 LAINVM 边界安装 `eval_handler`，让请求使用活动 TCB 运行子过程。
3. **C4.3：逐项恢复 Meta 语义。** 下面的恢复顺序每完成一项就加入真实编译 fixture 并提交。

恢复顺序：

1. 整数和基础物理运算；
2. 普通过程调用；
3. 类型值和模块值；
4. AST 值与宏展开；
5. nested `#eval`、预算和 Trap 诊断。

类型、模块和 AST 的解释和检查始终留在 Meta。若 Meta 求值需要把它们暂存在内存中，
LAINVM 只会操作相应的物理地址；它不会把这些对象识别为 `#eval` 的返回类别，也不会为
它们引入专门的返回协议。

每恢复一个切片就提交并加入真实编译 fixture，不等待整个 Meta 一次性恢复。

阶段提交按切片命名，最终收口提交为：

```text
lainc: route compile-time evaluation through LAINVM
```

## C5：恢复自举并替换冻结产物

目标：新 Meta 和 LAINVM 路径能够从 bootstrap 源码重新生成编译器。

工作：

- 生成 `build/bootstrap/lainc.l1` 和 snapshot；
- 验证新产物不含 `EvalResult` 及衍生接口；
- 完成 gen1 -> gen2 -> gen3 固定点比较；
- 运行 native compiler matrix、LAINIR API baseline 和 release gate；
- 把纠偏阶段的完成证据归档，主 roadmap 只留下后续工作。

阶段提交：

```text
bootstrap: replace the legacy eval artifact
```

## 后续工作

纠偏完成后恢复以下工作：

- compiler 类型诊断、source span 和剩余 backend 指令语义；
- 真实 CI runner 上的 bootstrap 与发布验证；
- 多 TCB 调度、Endpoint 和 CSpace；
- 参考解释器、native、Linux 和裸机的 LAINVM lowering；
- `#data` relocation、跨 backend alignment、浮点 ABI 和 Trap source mapping。

LAINIR 长期保持物理边界：不恢复 legacy 类型和模糊整数操作；字段访问展开为
`#lea + #load/#store`；不加入宽泛的 `#primitive`；AST、module、generic、effect 和
源语言类型不进入 LAINIR。

## 开发与提交规则

- 正确性优先于兼容性；不得为旧调用者保留错误接口、别名或适配层。
- 每个阶段完成后立即提交；可独立验收的切片也单独提交。
- 只提交当前阶段的文件，不混入工作区已有的其他修改。
- 测试必须验证真实执行路径，不能用 recording provider 或结构检查代替实现完成度。
- 暂时不可构建必须在提交说明和 roadmap 状态中明确记录，不能用兼容代码掩盖。
- 遇到会改变上述设计决定的问题时先讨论，再实施。
