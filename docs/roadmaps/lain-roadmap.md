# Lain 当前路线图

基线日期：2026-09-09。

本文只追踪尚未完成的工作。已经完成并有可执行证据的内容归档到
[`../history/`](../history/)；历史文档不定义当前架构。

C0 的完成记录见
[`../history/roadmap-eval-c0-2026-09-09.md`](../history/roadmap-eval-c0-2026-09-09.md)。

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
8. 冻结的 `src/lainir/lainc.l1` 及其 snapshot 暂时保留为引导工具。它们可以包含待替换
   的旧实现，但不再定义架构，也不得作为新增接口的依据。

## 当前进度

| 领域 | 当前状态 | 下一步 |
| --- | --- | --- |
| LAINIR 物理语义 | 基线可用 | 保持物理边界，不加入 Meta 返回分类 |
| LAINVM 基础 | 错误结果传输协议已删除；已有部分 TCB、VSpace、Trap 和预算实现 | 固定并实现真实 `#eval` 约定 |
| `#eval` | C seed 仍在当前解释器中内联执行 | 改为共享 VSpace 的临时 TCB |
| Meta 编译期求值 | 旧求值路径已删除，暂不可用 | 在 LAINVM 路径完成后重新接入 |
| 自举 | 冻结产物暂时可用 | 新路径完成后重新生成并恢复固定点 |
| C backend 与发布 | 非当前主线 | 纠偏完成后继续收口和 CI 验证 |

当前实施顺序：

```text
C1 冻结 #eval / TCB / Trap 语义
  -> C2 C seed 通过临时 TCB 执行 #eval
  -> C3 Lain 解释器实现相同语义
  -> C4 Meta 重新通过 #eval 执行编译期计算
  -> C5 恢复自举并替换冻结产物
```

## C1：冻结 `#eval`、TCB 和 Trap 语义

目标：让 C seed、Lain 解释器和 compiler 共享一份最小且明确的执行约定。

需要写入规范的行为：

- `#eval` 块的参数可见性和静态返回类型；
- 临时 TCB 与调用者共享 VSpace；
- 临时 TCB 建立自己的根调用过程，过程内 `#alloca` 仍受 activation 生命周期约束；
- 正常结果是普通 LAINIR 值；
- 失败结果是 Trap，至少保存错误分类、procedure 和 instruction position；
- nested `#eval` 同步创建另一个临时 TCB，并共享同一 VSpace；
- step、call-depth 和 allocation 预算的继承或划分规则；
- 编译器在 backend 前消除全部 `#eval`。

完成条件：`docs/01-lain-ir.md`、`docs/03-meta-system.md` 和 `docs/04-lain-vm.md` 分别只
描述自己负责的层次，并由同一组可执行行为检查约束。

阶段提交：

```text
docs: define eval execution through temporary TCBs
```

## C2：C seed 使用临时 TCB 执行 `#eval`

目标：替换当前 `interp_eval_block` 的内联控制状态保存方式。

执行路径：

```text
遇到 #eval
  -> 在当前 VSpace 创建临时 TCB
  -> 建立 #eval 根调用过程
  -> 调度执行块
  -> 返回普通值或产生 Trap
  -> 结束临时 TCB
```

验收至少覆盖：

- `#bits<N>`、`#addr` 和 `#unit` 正常返回；
- 共享 VSpace 中的有效地址可以返回；
- 已结束 activation 的 `#alloca` 地址不可继续使用；
- step、call-depth 和 allocation 超限产生稳定 Trap；
- nested `#eval` 创建独立临时 TCB；
- fold 后产物不含 `#eval`；
- 不存在“非标量结果需要 adapter”一类错误路径。

阶段提交：

```text
seed: execute eval blocks through temporary TCBs
```

## C3：Lain 解释器实现相同语义

目标：让 Lain 写的 LAINIR 解释器与 C seed 使用相同的 `#eval` 行为。

工作：

- 增加执行 LAINIR block 的入口；
- 复用调用者 VSpace，并创建临时 TCB；
- 用普通 `Value` 表示正常结果，用 Trap 表示失败；
- 删除 provider API 中的 Meta result kind 和资源转移接口；
- 使用同一组 fixture 比较 C seed 与 Lain 实现的值和 Trap。

阶段提交：

```text
lainvm: add temporary-TCB eval execution
```

## C4：重新建立 Meta 编译期求值

目标：让 Meta 生成 `#eval`，并通过 LAINVM 执行；不修补旧 `EvalResult` 求值器。

恢复顺序：

1. 整数和基础物理运算；
2. 普通过程调用；
3. 类型值和模块值；
4. AST 值与宏展开；
5. nested `#eval`、预算和 Trap 诊断。

类型、模块和 AST 在 LAINVM 看来只是已经确定物理类型的普通值。对它们的解释和检查
始终留在 Meta。

每恢复一个切片就提交并加入真实编译 fixture，不等待整个 Meta 一次性恢复。

阶段提交按切片命名，最终收口提交为：

```text
lainc: route compile-time evaluation through LAINVM
```

## C5：恢复自举并替换冻结产物

目标：新 Meta 和 LAINVM 路径能够重新生成编译器，旧冻结产物退出临时引导角色。

工作：

- 重新生成 `src/lainir/lainc.l1` 和 snapshot；
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
