# C1：`#eval` 执行契约审计

日期：2026-09-09。本文记录 C1 开始时的代码事实，不定义新的语言语义。

## 现有 C seed

`seed/src/interpreter/interpreter.c` 中的 `interp_eval_block` 直接保存并恢复当前解释器的
return、break、continue 状态，然后用调用者的 frame 执行块。这是内联执行，不会创建
TCB，也没有独立根 activation。

公开入口 `lainir_eval_block` 则创建一个新的 `LainirInterpreter` 和名字为 `<eval>` 的
临时 subroutine。它也没有输入 VSpace、调用者 frame 或 Trap record 参数。因此这两个
入口不能共同表达 C1 所要求的“临时 TCB 共享调用者 VSpace”。

`lainir_fold_module` 还有三项与 C1/C2 不一致的行为：

1. 它递归处理 nested `#eval`，随后调用上述独立入口；
2. 它只把 `#bits` 结果替换成常量；
3. 它保留 `#unit` 的 `EXPR_EVAL` 节点，并拒绝 `#addr` 和过程值。

因此 C2 需要替换执行路径和 fold 规则，不能在这些限制上增加兼容分支。

## 现有 Lain 解释器

`src/lainir/api/l1_interpreter.lain` 已有 `VSpace`、`TCB`、activation、Trap 和
scheduler 状态，但它们与 LAINIR 数据表示混在同一模块。C3 应将执行部分移至
`src/lainvm/`；C1 不移动代码。

该模块的内部 `Result` 结构包含 `status`、`value` 和 Trap 字段。它目前也被导出，并由
`src/lainir/api_contract.lain` 的 `EvalShape` 要求。这与路线图禁止把 `#eval` 正常值和
Trap 放进同一返回包装的决定冲突。C1 已从 LAINIR API contract 删除这项公开 contract；C3
可以在私有解释器控制流中采用任意等价表示，但不能把它作为 VM 或 `#eval` 返回值导出。

## C1 必须冻结、C2/C3 才实现的边界

| 项目 | C1 | C2 | C3 |
| --- | --- | --- | --- |
| `#eval` 的静态类型、捕获、预算和 Trap 规则 | 定义 | 按定义实现 | 按定义实现 |
| 临时 TCB 与共享 VSpace | 定义接口和生命周期 | C seed 实现 | Lain VM 实现 |
| `EvalShape` 的公开 Result 包装 | 删除 | 不得恢复 | 不得恢复 |
| `l1_interpreter.lain` 的执行代码 | 不移动 | 不移动 | 迁至 `src/lainvm/` |
| `#eval` 从 backend 产物中消失 | 定义验收 | 实现并检查 | 与 seed 对照 |

## 已冻结的 `#eval` contract

`#eval` 隐式按值捕获其自由 `%local`。lowering 按词法绑定把捕获值传入临时根过程；TCB
不会引用调用者 frame。捕获 `#addr` 不复制存储，也不延长区域或 activation 的生命周期。

最外层 `#eval` 建立共享预算账户；嵌套 TCB 共同消耗 step 和 allocation 配额，call-depth
连续计数。临时 TCB 共享调用者 VSpace，只能使用编译请求和调用者共同授权的 capability。

## C2/C3 的共同测试矩阵

两种实现必须对同一组 fixture 给出相同的普通值或 Trap 分类：

| 情形 | 期望 |
| --- | --- |
| 捕获 `#bits<N>` 局部值 | 临时根过程得到同一位模式 |
| 捕获仍存活的 `#addr` | 地址可按原 VSpace 权限访问 |
| 返回子 TCB 的 `#alloca` 地址 | memory/lifetime Trap |
| 嵌套 `#eval` | 独立 TCB、共享 VSpace 与预算账户 |
| 子 TCB 超过 step、call-depth 或 allocation 限制 | quota Trap，调用点没有普通值 |
| capability 不允许的 `#extern` | capability Trap |
| 正常 `#unit`、`#bits<N>`、`#addr` 返回 | backend 前消除对应 `#eval` 节点 |
