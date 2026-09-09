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
8. 冻结的 `bootstrap/lainc.l1` 及其 snapshot 暂时保留为引导工具。它们可以包含待替换
   的旧实现，但不再定义架构，也不得作为新增接口的依据。
9. 项目按 `seed -> bootstrap -> src` 分层：`seed` 是最小可信执行底座，`bootstrap` 是
   启动正式编译器所需的 LAINIR 源码和冻结产物，`src` 只放用 Lain 编写的正式实现。
   正式实现继续分为 `src/lainc`、`src/lainir` 和 `src/lainvm`；LAINIR 定义并验证
   物理程序，LAINVM 执行已经验证的物理程序。

## 当前进度

| 领域 | 当前状态 | 下一步 |
| --- | --- | --- |
| LAINIR 物理语义 | 基线可用 | 保持物理边界，不加入 Meta 返回分类 |
| LAINVM 基础 | 错误结果传输协议已删除；C seed 已有 TCB、VSpace、Trap 和预算实现；Lain 实现仍与 LAINIR API 混放 | 将 Lain VM 独立为执行层，并补齐同一份 `#eval` 约定 |
| `#eval` | C seed 已通过临时 TCB 执行并在 fold 前消除 | C3 实现同一份 Lain VM 语义 |
| Meta 编译期求值 | 旧求值路径已删除，暂不可用 | 在 LAINVM 路径完成后重新接入 |
| 自举 | 冻结产物暂时可用 | 新路径完成后重新生成并恢复固定点 |
| 项目结构 | `seed`、`bootstrap` 与 `src` 已分离；`src/lainvm/` 已建立边界 | C3 完成 VM 源码迁移与执行入口 |
| C backend 与发布 | 非当前主线 | 纠偏完成后继续收口和 CI 验证 |

当前实施顺序：

```text
C3 分离 LAINVM，并让 Lain 解释器实现相同语义
  -> C4 Meta 重新通过 #eval 执行编译期计算
  -> C5 恢复自举并替换冻结产物
```

## C3：分离 LAINVM，并让 Lain 解释器实现相同语义

目标：把“定义 LAINIR”和“执行 LAINIR”变成两个可独立维护的正式组件；让 Lain 写的
解释器与 C seed 使用相同的 `#eval` 行为。

这里的分离是职责和源码所有权的分离：`src/lainir/` 定义、解析、构造、验证和打印
LAINIR；`src/lainvm/` 保存执行状态，并执行已验证的 LAINIR。LAINIR 不依赖某个具体 VM
实现，LAINVM 依赖 LAINIR 的已验证程序表示。

工作：

1. 将解释器、TCB、VSpace、Trap、预算、调度和指令执行代码归入 `src/lainvm/`，并由
   `src/lainvm/SOURCES.txt` 纳入正式编译器源集合。
2. 从 `src/lainir/api/` 删除解释器和求值 API。该目录只保留 LAINIR 的表示、构造、解析、
   验证和打印 API。
3. 定义 LAINVM 的执行入口：输入为已验证的 LAINIR 过程与物理参数；正常结束产生普通
   物理 `Value`，失败产生 `Trap`。`Value` 只能是 `#bits<N>`、`#addr` 或 `#unit`；
   不得恢复 `EvalResult` 或任何等价包装。
4. 定义临时子 TCB 入口：LAINIR lowering 将 `#eval` 的隐式捕获物化为临时根过程的参数，
   VM 用共享 VSpace 和独立 TCB 执行该过程，并共享步数和分配预算。临时 TCB 的栈上地址
   不得作为 `#eval` 结果逃逸。
5. 固定 LAINIR 与 LAINVM 的单向接口：LAINIR 不见 TCB、VSpace、调度器或 Trap 的内部
   布局；LAINVM 不解释类型、模块或 AST 的语言含义。
6. 使用同一组 fixture 比较 C seed 与 Lain VM 的普通结果和 Trap；覆盖外部局部变量捕获、
   nested `#eval`、栈地址逃逸、预算，以及 `#eval` 消除。

完成条件：

- 正式源目录中不存在 `src/lainir/api/l1_interpreter.lain`；解释器只由 `src/lainvm/`
  提供。
- `src/lainir/api/` 不再公开执行状态或旧 `Eval`/`Result` 协议。
- C seed 的临时 TCB fixture 和 Lain VM 的源码/API boundary check 都通过；最终 LAINIR
  中不存在 `#eval`。

当前冻结的 `bootstrap/lainc.l1` 属于已删除的旧 Meta/Eval 实现，不能用于证明 C3 的 Lain
源码可编译，也不能为此恢复旧源码或旧接口。新 Lain VM 的实际编译、同一组 fixture 的
行为对照和新的 bootstrap 固定点统一放入 C5，使用新的 Meta 路径完成。
- 本阶段完成后单独提交，提交信息为 `lainvm: separate execution from lainir`。

阶段提交：

```text
lainvm: separate execution from lainir
```

## C4：重新建立 Meta 编译期求值

目标：让 Meta 生成 `#eval`，并通过 LAINVM 执行；不修补旧 `EvalResult` 求值器。

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

目标：新 Meta 和 LAINVM 路径能够重新生成编译器，旧冻结产物退出临时引导角色。

工作：

- 重新生成 `bootstrap/lainc.l1` 和 snapshot；
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
