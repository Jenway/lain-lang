# 设计决策记录（Decisions）

每个决策一个 `##` 小节，固定四字段：

- **What** —— 决策是什么（结论 / 定下来的东西）
- **Why** —— 为什么这么定（动机、约束、排除掉的备选）
- **How** —— 怎么做 / 落点在哪（代码、文件、机制）
- **Status** —— 状态：`提议` / `采纳` / `实现中` / `已完成` / `搁置`

编号从 `D-001` 递增；背景参见 `docs/01-lain-ir.md`、`docs/02-lain-vm.md`、`docs/03-lain-meta.md`。

---

## D-001 — 冻结 bootstrap，新语言工作转入 lain0

**What**
冻结手写 LAINIR bootstrap，不再往里面追加 surface Lain 特性；新的语言特性（语法、类型、降级）一律进入 `seed/lain0/`。冻结期结束后，`bootstrap/` 由 Lain 源码重写为 Meta 库，手写 IR 档案已删除。

**Why**
lain0 已经能够很好地替代手写 bootstrap，且已把它该承担的角色用尽——继续在手写 IR 上叠加特性只会让两份实现长期分叉、维护成本翻倍。

**How**
- 手写 IR 已从仓库删除；`bootstrap/` 现在是 Lain 写的 Meta 库，模块清单是 `bootstrap/SOURCE_ORDER`。
- 语言特性在 `seed/lain0/`（源码 `seed/lain0/src/`、契约头 `seed/lain0/include/lain0/`）。
- 回归集是 `bootstrap/lain/examples/*.lain`。

**Status**
已完成

## D-002 — 统一 call 与 addr：过程即地址，地址即位模式

**What**
调用与取址统一为一套表示：

```lainir
%r = #call @add(%a, %b)                       // 符号形式：直接按名字调用
%r = #call[(#bits<32>) -> #bits<32>] %f(%a)   // 值形式：在地址值 %f 上调用，括号里标签名
%p = #bitcast[#addr](%n)                       // bits -> addr（替代 int2ptr）
%n = #bitcast[#bits<64>](%p)                   // addr -> bits（替代 ptr2int）
```

移除 `#proc_addr`、`#data_addr`、`#int2ptr`（以及对称的 `#ptr2int`）。

**Why**
让过程成为一等公民——过程本质就是一段地址，`#call` 直接吃地址即可，不必为取址单独设 `proc_addr`/`data_addr`；地址就是机器字长的位模式，整型↔地址本来就是 `bitcast`，不必另设 `int2ptr`/`ptr2int`。这样砍掉一批冗余 IR 原语，算子表与实现都更瘦。

**How**
- LAINIR 算子表（`docs/01-lain-ir.md`）：删除 `proc_addr` / `data_addr` / `int2ptr` / `ptr2int` 行；`call` 改为接受「符号（`@name`）」或「带括号签名标注的地址值」两种形式；`bitcast` 增加 `addr ↔ bits<N>` 用法。
- 落点（IR 层变更，波及 seed）：`seed/kernel/include/lain/ir/codes.h`（算子枚举/opcode）、`seed/kernel/include/lain/ir/build.h`（构造 API）、`seed/kernel/include/lain/text/print.h` + `seed/kernel/include/lain/text/parse.h`（文本往返），以及 lain0 的发射端（`seed/lain0/src/LowerExpr.cpp` / `LowerDecl.cpp` 的 call / 指针降级）与 `bootstrap/lain/examples` 里用到这些算子的用例。
- 注意：这是 **IR 层面的改动**；按 D-001 新语言工作进 lain0，但本决策要先动 seed 与 docs/01，需与 lain0 的 call / 指针发射端同步排期。

**Status**
已完成（seed IR 层 opcode 已移除并统一为 `#call` + `#bitcast[#addr]`；lain0 发射端已切换）

## D-003 — 保证尾调用：`#call[tail] @f(...)`

**What**
新增「保证尾调用」调用形式（与 D-002 的 call 统一形式衔接，只是多一个 `tail` 标签）：

```lainir
%r = #call[tail] @f(...)      // 符号形式；值形式同理：#call[tail][(sig)] %f(...)
```

要求实现端**必须**按尾调用处理（复用当前 stack 帧、不产生新帧），不能把它当成「尽力优化」。

**Why**
尾递归 / 相互尾调用在 Lain 这种降级到扁平 LAINIR 的语言里很常见，需要一种 IR 级的契约让后端**必须**做尾调用优化。普通 `#call` 是否尾化是后端自由裁量的事，无法在 IR 层表达「这里必须尾调」；没有这个保证，深递归会让 stack 溢出，且行为不可预测。

**How**
- LAINIR 算子表（`docs/01-lain-ir.md`）：`call` 增加 `tail` 修饰形式，并写明语义约束——调用点必须是过程的最后一个动作（其后再无计算，仅接 `#return` 或自身即终结点），否则 `lainir_verify` 确定性拒绝；后端必须复用 stack 帧、禁止新建帧，无法保证时确定性拒绝，不得悄悄退化为普通调用。
- 落点（IR 层，波及 seed）：`seed/kernel/include/lain/ir/codes.h`（`call` 的 tail 标志 / opcode 变体）、`seed/kernel/include/lain/ir/build.h`（`lainir_inst_call` 增加 tail 参数或独立构造）、`seed/kernel/include/lain/text/print.h` + `seed/kernel/include/lain/text/parse.h`（文本 `[tail]`）、`seed/kernel/src/ir/verify.c`（校验 tail 调用位置合法性）、解释器/编译后端 `engine`（实际复用帧）。
- lain0：发射端在尾位置（return 前的 call、或作为表达式语句的尾调用）选用 `#call[tail]`；与 D-002 的 call 形式一致。
- 注意：属 IR 层改动，与 D-002 强耦合，应一并排期。

**Status**
已完成（seed 加 `is_tail` + `lainir_inst_call_ex`；lain0 在尾位置发 `#call[tail]`；verify 校验尾位置，非尾位拒绝）

## D-004 — 模块命名空间与根名清单

**What**
模块命名空间与根名清单。每个模块的顶层声明发出 `<模块键>__<名字>`，模块键由源根相对路径去掉 `.lain` 后把 `/` 换成 `_` 得到；清单里标为根名的少数声明保持平坦，供宿主按名字查找；跨模块访问一律写 `M.名字`，`symbol_addr` 只管本模块的静态数据。

**Why**
内核按平坦字符串解析名字，模块前缀是让两个模块的同名声明共存的最小手段；清单承载顺序与根名，使整份模块的产物只由清单决定，满足「同一份 SOURCE_ORDER 必须给出逐字节相同的模块」。

**How**
`Program` 一次加载整个依赖图并两轮分析，`Lowerer` 按清单顺序把全部模块降级进同一个 `L1Builder`；`scripts/build_meta_unit.py` 只负责调用 CLI 并生成驱动要的 unit 列表。

**Status**
采纳（已实现并验证：`lain0 --unit bootstrap/SOURCE_ORDER` 零诊断、连跑两次产物 SHA-256 相同；回归 38/38；Meta 管线 `host_status=0` / `region grants=2` / `output bytes=74`，与 `build/arith.out` 逐字节相同。规范面见 `docs/04-bootstrap-dialect.md` 的《模块：成员位置与符号命名》）

<!-- 决策在此逐条追加 -->
