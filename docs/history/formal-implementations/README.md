# 暂停的形式实现（LAINIR provider 与 LAINVM interpreter）

状态：**已暂停并归档，2026-09-12。** 这里保留的是第二版实现，用于追溯；它们不参与当前构建。

## 为什么暂停

这两批代码是用 Lain 写的：

- **LAINIR provider**：`l1_ir`、`l1_unit_builder`、`l1_verifier`、`l1_printer`、`default_provider`
  （2537 行中的 2195 行），负责构造、验证和打印物理 IR；
- **LAINVM interpreter**：`interpreter`（1424 行），负责执行物理 IR。

它们能编译、能通过 verifier，但**没有真正的执行路径**：

| | 实测状态 |
| --- | --- |
| LAINIR provider | 有一个 gate 编译并执行它，但只调用了 `schema_version()`（返回常量）。试着深入一层（`Builder.new_unit()`）立即失败：成员别名无法 lowering。五个文件里其余部分从未被调用。 |
| LAINVM interpreter | **零执行**。全仓库对 `execute_child` 的引用只有一处字符串匹配。 |

决定：**等 Lain 成熟后用 Lain 重新实现它们**，而不是现在维护一批跑不起来的代码。

## 保留了下来什么

`src/` 里保留两个**契约**（它们不是实现，是接口说明）：

```text
src/lainir/api_contract.lain     CapabilitiesShape、Contract 等
src/lainvm/api_contract.lain     ExecutionShape、Eval 等
```

`src/lainc` 的 12 处 import 里只用到这两个契约里的类型，把它当**参数**接收，不自己实例化。所以归档实现**不影响 lainc 编译**——`build_srclainc.py` 仍然退出 0。

## 三个实现的关系

同一个契约有三份实现，它们各自独立、互不依赖：

| 实现 | 位置 | 状态 |
| --- | --- | --- |
| C seed | `seed/src/interpreter/` | ✅ 工作实现，CI 里跑 |
| bootstrap | `bootstrap/compiler/` | ✅ 工作实现，15 道 gate 通过 |
| Lain | 本目录（原 `src/lainir`、`src/lainvm`） | ⏸️ 已归档 |

注意 `seed/` 里的 `lainir_parse_module_checked` 与 `bootstrap/` 里的
`lainvm_validate_consteval_group` 只是**命名巧合**——它们是各自实现内部的函数名，
与这里的代码无关。

## 重写时要注意什么

1. **契约是起点**。`api_contract.lain` 定义了一个 provider 必须满足什么，重写时对着它写。
2. **LAINIR provider 的缺口**：模块成员别名（`@export let x = Other.f;`）无法被 lowering
   成物理过程。这是归档前最后一次实测发现的，重写时必须先解决。
3. **LAINVM 的缺口**：归档版本缺浮点（0 处）、缺 Endpoint 与 scheduler（0 处），
   表达式覆盖 27/42、指令覆盖 6/9。C seed 是完整参考。
4. **`check_lainvm_boundary.py`** 已改写为只检查契约边界与「归档文件不得回到 src/」，
   不再检查实现形状。
