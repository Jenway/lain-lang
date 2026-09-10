# Lain 当前路线图

基线日期：2026-09-10。

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
11. 类型是一等 Meta 值，其类型通过标准 Meta 环境中的 `std::type` 表达。迁移完成后，
    源语言不再保留裸 `type` 作为编译器特殊语法。
12. Lain 不设置独立的泛型语言机制。`std::func(T: std::type) -> std::type` 是接收并返回
    Meta 值的普通 Meta 函数；`Vec(T)`、`Result(T, E)` 等类型构造也都是普通函数调用。
13. 函数签名采用
    `std::func(显式参数) ?{环境输入} -> 返回类型 !{输出 effect}`。`?{}` 中的条目由调用
    环境提供；`!{}` 中的条目是函数执行时可以发出的 operation。两者使用同一套有类型的
    request、handler 和 continuation 基础设施，但方向和检查规则不同。
14. `?{T}` 是待推导的环境输入，等价于 `?{T: _}`。Meta 根据整个函数签名推导它的类型；
    `?{T: std::type}` 是对应的显式写法。函数使用的所有环境输入必须出现在 `?{}` 中。
15. Parser 继续只产生 Atom 和 Group。`std::func` 的 Meta elaborator 负责解释
    `(参数) ?{输入} -> 返回类型 !{effect} {函数体}`，compiler core 不建立泛型、类型工厂
    或输入 effect 专用语法节点。

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
| LAINVM 基础 | C seed 与 Lain 源码边界均已采用 TCB、VSpace、Trap 和预算模型；bootstrap 与正式实现都已公开 Artifact、Procedure、物理参数和 eval 接口 | 扩展通过该接口执行的 Meta callable 范围 |
| `#eval` | C seed 已通过临时 TCB 执行并在 fold 前消除；Lain VM 已提供子 TCB 原语；lainc Meta 已改为发出 `Vm.eval` effect | 在正式编译器执行入口安装 handler，并运行真实编译 fixture |
| Meta 编译期求值 | `Ir.Eval`、`Eval.Result` 和伪造的 status/value 返回已经从正式 Meta 源码删除；bootstrap 已能通过 LAINVM 执行算术、标量调用、effect 构造和 module factory | 统一普通 Meta callable；禁止按返回类别建立执行分支 |
| 函数与类型机制 | 当前源码仍有裸 `type`、`comptime`、generic policy 和 compiler-owned specialization 模型 | 引入 `std::type` 与 `?{}`，把类型构造统一为普通 Meta 函数调用，然后删除旧机制 |
| 自举 | `build/bootstrap/lainc.l1`、正式 `std/**/*.lain` 和 `src/lainc` 闭包均已生成并验证；两次独立构建的 `srclainc.l1` 在 357 个 procedure 上规范化一致 | 让 `srclainc.l1` 自身编译下一代产物，完成 gen1 -> gen2 -> gen3 固定点 |
| 项目结构 | `seed`、`bootstrap` 与 `src` 已分离；`src/lainvm/` 已拥有执行实现 | 保持职责边界并在 C5 恢复新自举 |
| C backend 与发布 | 非当前主线 | 纠偏完成后继续收口和 CI 验证 |

当前实施顺序：

```text
C4 Meta 重新通过 #eval 执行编译期计算
  -> C4.5 统一 Meta 函数、std::type 与输入 effect
  -> C5 恢复自举并替换冻结产物
```

## C4：让 lainc 通过 LAINVM 重新建立 Meta 编译期求值

目标：让 Meta 生成 `#eval`，由 lainc 通过 LAINVM API contract 发起执行，且由 LAINVM 的
effect handler（接收 `Eval` 请求并启动子 TCB 的代码）在子 TCB 中执行；不修补旧 `EvalResult`
求值器。

先完成运行时接线，再恢复语言能力：

1. **C4.1：固定执行接缝。** 已完成 LAINVM `Eval` effect operation 和 API contract；
   该 contract 只表达 Artifact、Procedure、物理参数与物理返回值。C seed 已实现同义的
   bootstrap 句柄接口，并用真实双参数过程验证返回 42。
2. **C4.2：接通编译入口。** `Vm` capability 已传入 lainc 的 compiler、API、driver 和
   Meta factory；Meta 已通过 `perform Vm.eval(...)` 发出执行请求。剩余工作是在执行正式
   编译器的 LAINVM 边界安装 `eval_handler`，让请求使用活动 TCB 运行子过程。
3. **C4.3：恢复 bootstrap 物理 lowering。** 已从误删文件中恢复程序状态、类型布局、
   检查与 LAINIR 输出，同时移除旧求值协议。普通物理 fixture 已重新通过。
4. **C4.4：逐项恢复 Meta 语义。** 算术、纯标量过程调用和
   `allocation.Alloc(Policy)` 已经完成：bootstrap 把过程和调用点编译成含 `#eval` 的
   临时 LAINIR，seed 验证后由临时 TCB 执行，最终 artifact 中不保留 `#eval`。
   Effect factory 的模块参数以物理地址传入，返回的 Effect 地址保存了本次调用的形参与
   实参绑定。`std::effect_operation(...)` 也已通过同一路径建立保留 effect、名称和函数
   签名语法的 callable 描述。完整正式标准库闭包现已生成并通过
   `scripts/build_formal_stdlib.py`、标准库一致性、Meta AST、模块诊断和策略检查。下一步
   `scripts/build_srclainc.py` 也已生成并验证正式编译器源码闭包；
   `scripts/check_srclainc_artifact.py` 证明两次独立生成的 357 个 procedure 规范化一致。
   module factory 已通过同一路径执行。不得继续增加按返回值类别划分的 type、module、AST
   执行分支；后续 callable 统一工作进入 C4.5。

恢复顺序：

1. 整数和基础物理运算（已完成 bootstrap `consteval` 切片）；
2. 普通过程调用（已完成纯标量参数与返回值切片）；
3. 模块值（已完成 module factory 的第一段）；
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

## C4.5：统一 Meta 函数、`std::type` 与输入 effect

目标：删除 compiler-owned 泛型规则，让函数、类型构造和环境输入都由标准 Meta 语义定义；
所有 Meta 函数共用同一条 LAINVM 执行路径。

### C4.5.1：固定当前语言形式

- 在 `std` 的根 Meta 环境中提供 `std::type`，它表示类型值所属的 Meta 类型；
- `std::func` 解释完整签名：
  `(显式参数) ?{环境输入} -> 返回类型 !{输出 effect} {函数体}`；
- `?{name}` 允许从完整签名推导输入类型，`?{name: TypeExpr}` 保留显式类型；
- `?{}` 和 `!{}` 都允许为空，并分别参与调用检查与 effect 检查；
- 删除 `comptime T: type`、隐式自由类型变量和裸 `type` 的当前语法约定，不提供兼容别名。

验收 fixture 至少覆盖：

```lain
let Box = std::func(T: std::type) -> std::type {
    return std::struct { value: T };
};

let max = std::func(a: T, b: T)
    ?{T, ord: Ord(T)}
    -> T {
    return ord.max(a, b);
};
```

### C4.5.2：建立统一 callable 表示

- `std::func` 的 Meta elaborator 从 RawAst 提取显式参数、输入行、返回类型、输出 effect 行和
  函数体；Parser 不增加相应的语义节点；
- callable 参数统一承载 Meta 值和运行时值，不设置 `program_is_type_binding`、type factory
  或 generic factory 分支；
- 调用 Meta 函数时，将显式参数和解析后的环境输入组成调用环境，再 lowering 为物理参数并
  交给 LAINVM；
- LAINVM 只接收物理值。返回的物理地址由 Meta 按该 callable 的静态返回类型解释，VM 不
  识别它是类型、模块还是 AST；
- 同一 callable 和相同 Meta 参数可以缓存生成结果。缓存是实现策略，不定义语言中的泛型
  或参数合法性。

### C4.5.3：实现输入 effect

- 为函数类型增加有类型的 input row；
- 调用检查从当前 Meta 环境解析每个输入，缺失、重复和歧义都产生 source diagnostic；
- bare input 的类型推导使用参数、返回类型、其他输入和函数体形成的同一组约束；
- handler 可以向 continuation 提供 input，也可以处理 `!{}` operation；两种方向共享底层
  request 表示，但保持各自的静态检查；
- lowering 后不残留 `?{}`，最终 LAINIR 只包含确定的物理参数、过程和 operation lowering。

### C4.5.4：删除旧泛型设施并迁移标准库

- 删除 generic policy、generic parameter kind 检查和 generic specialization token；
- 删除 `ComptimeValue` 中仅为旧泛型参数模式存在的分类和诊断；
- 将 `Vec`、`Result`、`Slice`、`String`、内存模型和 LAINVM 类型构造迁移为普通 Meta
  函数；
- 将源码中的类型标注从裸 `type` 迁移到 `std::type`；
- 更新 Meta、AST、effect 和标准库文档，清除旧 `comptime` 泛型示例。

C4.5 完成条件：

- 上述 `Box` 和 `max` fixture 通过真实 bootstrap 编译与 LAINVM 执行；
- 缺少 `ord`、无法推导 `T`、输入歧义和输出 effect 不匹配分别产生稳定诊断；
- 仓库当前源码与当前规范中不再出现旧 `comptime` 泛型语法或 compiler-owned generic
  policy；
- 类型函数、模块函数和普通标量 Meta 函数通过同一 callable 调用入口；
- 生成的最终 LAINIR 不含 `?{}`、Meta 类型对象或未执行的 `#eval`。

阶段提交按实现切片提交，最终收口提交为：

```text
meta: unify type functions and input effects
```

## C5：恢复自举并替换冻结产物

目标：统一 Meta callable、`std::type`、输入 effect 和 LAINVM 路径能够从 bootstrap 源码
重新生成编译器。

工作：

- 生成 `build/bootstrap/lainc.l1` 和 snapshot（bootstrap bundle 已完成）；
- 验证新产物不含 `EvalResult` 及衍生接口；
- 生成并验证 `build/lainir/srclainc.l1`（已完成，357 个 procedure，独立重建一致）；
- 完成 gen1 -> gen2 -> gen3 固定点比较；
- 运行 native compiler matrix、LAINIR API baseline 和 release gate；
- 把纠偏阶段的完成证据归档，主 roadmap 只留下后续工作。

阶段提交：

```text
bootstrap: replace the legacy eval artifact
```

## 后续工作

纠偏完成后恢复以下工作：

- compiler source span 和剩余 backend 指令语义；
- 真实 CI runner 上的 bootstrap 与发布验证；
- 多 TCB 调度、Endpoint 和 CSpace；
- 参考解释器、native、Linux 和裸机的 LAINVM lowering；
- `#data` relocation、跨 backend alignment、浮点 ABI 和 Trap source mapping。

LAINIR 长期保持物理边界：不恢复 legacy 类型和模糊整数操作；字段访问展开为
`#lea + #load/#store`；不加入宽泛的 `#primitive`；AST、module、类型函数、effect 和
源语言类型不进入 LAINIR。

## 开发与提交规则

- 正确性优先于兼容性；不得为旧调用者保留错误接口、别名或适配层。
- 每个阶段完成后立即提交；可独立验收的切片也单独提交。
- 只提交当前阶段的文件，不混入工作区已有的其他修改。
- 测试必须验证真实执行路径，不能用 recording provider 或结构检查代替实现完成度。
- 暂时不可构建必须在提交说明和 roadmap 状态中明确记录，不能用兼容代码掩盖。
- 遇到会改变上述设计决定的问题时先讨论，再实施。
