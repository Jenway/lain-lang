# 编码 1 设计：库 Meta 语义与 LAINIR 编译期求值

状态：修订计划（2026-09-13），尚未实施新迁移。对应
[`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md) §7；实施顺序与验收以路线图为准。
本稿替代旧的“统一 Meta callable 执行”设计。

## 1. 职责与撤回的结论

Meta 在编译期操作 AST，实现 expand、elaborate、lower。绑定、函数、类型、模块、effect、
operation 和 handler 的语言语义由库拥有。编译期计算由库生成 LAINIR `#eval` 表达；
LAINVM 执行已验证的物理 IR，Backend 只消费求值后的运行时 IR。

旧编号的完成证据与被撤回方案见
[`完成归档`](../history/roadmap-lain-completed-2026-09-13.md) 及当前路线图的边界约束。
本设计只跟踪剩余 1e–1h；库必须完整实现语义，识别谓词不能替代语义迁移。

core 提供 SourceApi、AstApi、IrApi、通用阶段结果与诊断，以及物理执行能力的显式组合。
AstApi 只操作拓扑、文本、span、origin/hygiene；IrApi 只构造和验证物理 IR。
库内部可有类型/effect/module 对象，不把它们的 kind、offset 或语义暴露为 VM 执行协议。

## 2. 当前库实现债务

实际 artifact 归属与五条生成路径见
[`1d 审计快照`](../history/roadmap-lain-1d-2026-09-13.md)。下列旧 compiler 目录中的
语言实现均打入 stdlib，未进入 core；生成的编译期计算已有显式 `#eval`。

以下路径由当前源码定位，行号随实现变化，后续迁移仍须记录完整调用证据：

| 位置 | 当前行为 | 迁移要求 |
| --- | --- | --- |
| `bootstrap/compiler/meta_call.l1::program_collect_meta_call_candidate` | 依次尝试 operation/module/effect/scalar，分别绑定结果 | 由库语义处理取代语言专用分派 |
| `meta_call_vm.l1::lainvm_meta_write_scalar_expression` | 简化源表达式 writer | 用库 lowering 与普通物理表达式生成覆盖 |
| `meta_call_vm.l1::lainvm_meta_build_module_artifact` 等 | 构造引用语法/环境的描述值，未完整 lower 原函数体 | 描述值及其消费者由库拥有，移除专用 artifact 捷径 |
| `lower_program.l1::program_function_is_meta` / `program_write_function` | 跳过尚未消解的高层函数 | 库完成阶段处理后生成物理 IR，避免高层对象泄漏 |
| macro 辅助函数与探针 | 已移入 `bootstrap/std/ast_macros.l1`，有边界和库替换执行检查 | bootstrap 与正式库均通过 12 个共用用例及正式调用者 context/origin 检查；完整绑定与展开仍待验收 |
| 正式库与阶段失败/资源/metadata | 尚缺完整阶段语义及更广执行证明 | 按剩余验收建立实际结果消费与失败生命周期证据 |

可复用基础及 bootstrap 阶段结果交接见完成归档与
[`1e 部分实现`](../history/roadmap-lain-1e-stages-2026-09-13.md)。迁移不得只移动文件或让库回调 core 的语言 builder；
须审计当前能力及生命周期。测试产物仅落 `build/`，不得修改生成物通过验收。

## 3. 实施片与交接

| 片 | 主要产出 | 验收重点 |
| --- | --- | --- |
| 1e（剩余） | 完整宏/attribute、正式库阶段语义与更广的失败/资源/metadata 验收 | core 只含通用 AstApi，库规则与生命周期有实际运行证明 |
| 1f | effect 构造、operation、检查及一个 handler 的完整库实现 | 正例运行，反例诊断，库不依赖 compiler 专用 builder |
| 1g | 显式 LAINIR `#eval` 生成、验证、求值及结果消费 | 求值前后 IR 证据，Trap 传播，backend 无 `#eval` |
| 1h | module/type 等沿相同边界迁移；删除全部语言专用捷径 | 全量行为回归、闭包重建、没有类别驱动执行协议 |

1f 复用现有显式 `#eval` 生成/执行链，并验证实际 effect/handler 消费；1g 扩大计算
覆盖与物理生命周期验证。不得新增绕过 `#eval` 的源语言求值协议。每片独立验收。

物理求值交接为：库生成 LAINIR（含所需 `#eval`）→ 验证 → 通过显式组合的执行能力求值
→ 库消费物理结果或诊断 → 输出已消除 `#eval` 的运行时 IR。不得直接执行尚未 lowering 的
源语言函数。阶段编排如何在计算依赖处继续、结果如何进入库环境，按 1d 审计快照中的当前契约给出
调用证据；不凭本稿虚构现成接口。

## 4. 旧堵塞的归属与风险

`meta_value_kind = 4` 的类型/effect 重用属于库内部表示审计。必要的修复包含所有消费者，
不得增加 compiler effect kind 来驱动执行路径。

`formal_meta_effect_factory.lain` 的返回签名与返回值矛盾由库返回类型规则检查：建立正确
正例并保留不匹配反例，明确诊断预期。专用路径不能继续容忍错误签名。

旧 scalar writer 的覆盖不足属于库 lowering 与物理代码生成缺口，纳入 1h。
旧描述值布局迁移须审计引用、环境和生命周期，保持公开 ABI，禁止引入 Meta sidecar 协议。

新增 C seed 能力只允许填补真实缺失的通用物理机制或宿主能力，不实现 Lain effect/type/module
规则。遇到路线图 §15 条件，停止相关实现并给出最小复现与两个方案。

## 5. 删除与验收

删除清单以路线图 §7.3 为准，包含 scalar 专用 writer、module/effect/operation 的 builder、
eval wrapper 和源码形状分派。可复用纯物理部分须拆出；改名不能代替删除。

计划新增（尚未实现）：

- `check_library_effect_lowering.py`：运行 effect/operation/handler 正例及诊断反例。
- `check_lainir_comptime_pipeline.py`：整数、AST 操作、库语义数据、let/if/普通调用与 Trap 的
  `#eval` 路径证据；验证结果消费与求值后 IR。

新检查须有负对照并登记 baseline。旧 `check_meta_form_swap.py` 继续作为识别回归。
构建与现有 gate 命令见路线图 §7.4；文字搜索只确认删除，不能证明语义正确。

最终证据必须说明：替换库 Meta 可改变语言规则，core/VM 无语言专用协议，所有编译期
计算经过 `#eval`，剩余物理 IR 可独立验证和执行。

## 6. 与后续阶段的关系

编码 0、2、3、4 的已有成果可复用；完整函数签名与输入行语义须由库真正消费。
本阶段在 bootstrap 链执行，并维持 bootstrap/std 与正式 std 的共同契约，不把两份实现混用。
编码 5 负责正式 lainc 的 provider/VM 运行组合，选择 C seed 或重写 Lain 实现仍待决定。
编码 6 的编译器固定点和编码 7 的最终发布验收不因本阶段文档修订而完成。
