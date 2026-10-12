# 实施计划：D-002 / D-003 — 统一 call/addr + 保证尾调用

关联决策：`docs/decisions.md` 的 D-002、D-003；约束见 D-001（冻结 bootstrap）。

## 目标（来自决策）
1. 单一 `#call`：符号形式 `#call @name(...)` 与值形式 `#call[(sig)] %f(...)` 统一。
2. 地址↔整数走 `#bitcast[#addr]` / `#bitcast[#bits<N>]`；移除 `#proc_addr`/`#data_addr`/`#int2ptr`/`#ptr2int`。
3. `#call[tail]` 保证尾调用（复用栈帧，否则 verify 拒绝）。

## 与 D-001 的硬冲突（必须裁决）
`bootstrap/` 的手写 LAINIR（`meta.l1` + `bootstrap/std/*.l1` + `bootstrap/lain/examples/lainir_f*.l1`）在 **1163 行**里用了待移除的 opcode。物理删除这些 opcode 会直接让 bootstrap 管线无法解析/验证，与"冻结 bootstrap（保持其可用）"冲突。

两个方向：
- **A（彻底）**：现在从 seed 物理删除 `INST_CALL_INDIRECT`/`INST_INT2PTR`/`INST_PTR2INT`/`INST_DATA_ADDR`/`INST_PROC_ADDR`，并把 bootstrap 全部 1163 行机械迁移到新 IR。干净，但工作量大、暂时违反 D-001 的"bootstrap 保持可运行"。
- **B（推荐，本计划默认）**：seed 保留 legacy opcode（标记 deprecated），lain0 发射端切到新统一形式、docs 描述 canonical IR；待 lain0 完全取代 bootstrap（D-001 终局）后再物理删除。尊重 D-001、零风险、可立即推进。

> 本计划先实现**两方案共用的安全部分**（不破坏 bootstrap）；A/B 的破坏性删除留作裁决后的下一步。

---

## 第一步：seed 基础设施（A、B 都需要，纯增量，不删 opcode）
1. `seed/include/lain/ir/core.h` 的 `struct L1Inst`：新增 `bool is_tail;`（紧挨 `is_volatile`）。
2. `seed/include/lain/ir/build.h`：新增
   `const L1Inst *lainir_inst_call_ex(L1Builder*, const char *result, const char *symbol, const L1Operand*, uint32_t n, bool tail);`
   原 `lainir_inst_call` 保留（tail=false），避免改崩现有调用方。
   值形式（原 INST_CALL_INDIRECT）新增
   `const L1Inst *lainir_inst_call_value(L1Builder*, const char *result, const L1Operand *target, const L1Operand*, uint32_t n, bool tail);`
3. `seed/include/lain/text/print.h` 实现：INST_CALL 打印 `#call[tail] @symbol(...)`；INST_CALL_INDIRECT 打印 `#call[(sig)] %target(...)`。（仅 tail 调用新增 `[tail]` 文本，现有无 tail 用例格式不变 → 不破坏 bootstrap 夹具。）
4. `seed/src/ir/verify.c`：INST_CALL 当 `is_tail` 为真时，校验其后仅接 `#return` 或自身即终结点，否则 `L1V_BAD_TAIL` 拒绝。

## 第二步：lain0 发射端（仅 B 也够，A 后续再删 opcode）
- `lain0/src/LowerExpr.cpp`：
  - `ptr_of`：`INST_PTR2INT` → `INST_BITCAST`，目标类型 `#addr`。
  - `addr_of`：`INST_INT2PTR` → `INST_BITCAST`，目标类型 `#bits<N>`（N=机器字长）。
  - `lowerCall`：新增 `bool tail` 形参；tail 时走 `lainir_inst_call_ex(..., tail=true)`。
- `lain0/src/LowerStmt.cpp`：
  - 识别尾位置：`return <call-expr>` 或 作为表达式语句的尾调用 → 标 tail，调用 `lowerCall(..., tail=true)`。
- 值形式调用：当前样例不需要，先留 `INST_CALL_INDIRECT` 路径，等需要再接。

## 第三步：文档
- `docs/01-lain-ir.md`：算子表更新——`call` 统一符号/值两种形式并标注 `[tail]`；`bitcast` 增加 `addr ↔ bits<N>`；注明 `#proc_addr`/`#data_addr`/`#int2ptr`/`#ptr2int` 已并入门（legacy 暂留 seed 供 bootstrap 兼容）。

## 验收
- `clang++ -fsyntax-only` 过全部 TU。
- 全量链接 `build/lain0/lain0.exe`。
- 回归：`bootstrap/lain/examples/*.lain` 仍 11/31 通过，且 `memory.lain`（走 ptr_of/addr_of→bitcast）必须仍 verify 通过。
- 新增一个最小尾调用用例验证 `#call[tail]` 产物 + verify 接受；并验证"非尾位置的 tail"被 verify 拒绝。

## 裁决后（仅 A）
- 物理删除 5 个 opcode；printer 统一 `@` / `(sig)` 语法；机械迁移 bootstrap 1163 行；bootstrap 回归集改为 lain0 产物。
