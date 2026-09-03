# Lain 标准库自举与 archive lainc 路线图

> 基线日期：2026-09-02
> 本文是当前自举主线。它取代旧版“Meta.invoke 执行环境”路线。
>
> 核心修正：Meta 只负责 AST 操作和语言规则变换；编译期执行由
> LAIN-IR `#eval` 完成。第一代标准库使用 LAIN-IR 编写，正式标准库使用
> Lain 编写，两者遵守同一接口。

> 当前进度：AstApi/结果布局、identity bootstrap stdlib、core/std 分包和自动基线
> 已落地；module/struct、record/type、import、collection、call 和 evaluator 代码
> 已进入第一代标准库 artifact。evaluator 统一入口已经通过显式 `#eval` block
> 执行，且新鲜自举链的基础类型错误已修复；正式 `std::meta` 已能由 bootstrap
> artifact 编译并运行；archive 的 Meta 结构调用、AST 克隆、字符串 ABI、token
> 名称解析、alias 查询和未知名称诊断探针已可重复通过。第二套生成的 archive
> API 现在也能通过 empty/nonempty 两个客户端验收；完整正式 std/archive 源码
> 已能由第 2 套编译并通过 verifier 和运行 smoke；`scripts/build_formal_stdlib.py`
> 现在可以从全部 `std/**/*.lain` 生成并验证 `build/lainir/formal_stdlib.l1`。
> 该 artifact 还会检查并导出五个 `lain_std_*` ABI 入口；当前正式库的
> `expand` 已对首个源根节点执行 AstApi 复制，复制动作由正式 `std::meta`
> 导出的库函数调用 seed ABI 完成；正式 bootstrap 入口的 AST 只读访问
> （first/next/kind/delimiter/start/length/nil）也已统一经由 `std::meta`
> 包装；`elaborate` 仍传递该句柄，
> `lower` 已能为十进制常量 `return`、简单 `+ - * /` 算术、同一源文件内的
> 无参数/一至两个标量参数的函数调用以及布尔/数值比较的 `if ... else` 双分支返回生成并运行最小
> LAIN-IR；算术 lowering 生成的表达式现在包在 `#eval` 块中，由 seed evaluator
> 执行后再返回标量，
> `unit` 空返回也能生成并通过 verifier，且不完整表达式会被拒绝；其他形式
> 明确返回迁移中的 `5203`。
> `run_lain_compiler.py --stdlib-artifact` 已能用同一个 compiler core 实际加载
> formal artifact，并在该 lowering 边界得到 `5203`。
> archive API 的空输入和非空
> 输入客户端已经可用；简单标量 `return` 的真实用户程序已能生成并回灌执行；
> 完整 L1 Unit 生成、正式标准库全面替换、复杂真实用户程序、native `lainc`
> 以及 archive 自举固定点仍未完成。

> 最新验证：从冻结 seed 重新生成第 3 套后，再连续生成 gen2、gen3；两者均通过
> verifier，canonical 内容一致（490131 字节）。第 2 套从这条链直接编译完整
> 正式 std/archive 的验收脚本也已通过；`return 42` 用户程序的 artifact 回灌
> 验收也已通过。复杂真实用户程序、native `lainc` 和 archive 自举仍是后续 gate。

## 1. 最终目标

完成下面这条可重复、可验证的自举链：

```text
C seed
  执行 LAIN-IR
    -> 第 2 套：LAIN-IR 写的 lainc 底座
       调用 LAIN-IR 写的第一代标准库
         -> 编译 Lain 写的正式标准库
         -> 编译 archive lainc
           -> archive lainc 编译正式标准库和自身
             -> 生成独立可用的 lainc
```

当前四套代码的定位固定为：

```text
seed/                         C 写的 LAIN-IR 解释器和最小宿主能力
src/lainir/lainc.l1           第 2 套，冻结的 LAIN-IR compiler artifact
src/lainc/lainc.lain          第 3 套，临时过渡编译器
src/compiler-archive/*.lain   第 4 套，最终 Lain-written lainc 源码
```

第 3 套不进入最终自举链。archive lainc 接管后，将它从默认构建和测试中移除；
确认没有独立用途后再删除源码。

## 2. 不可混淆的职责

### 2.0 鸡生蛋问题的解决办法

第一代标准库和正式标准库不是两套接口：它们实现同一个
`Bootstrap Standard Library ABI v1`。因此启动顺序是：

```text
seed
  -> 第 2 套 compiler core
  -> 第一代 LAIN-IR stdlib（实现 ABI）
  -> 编译正式 std/*.lain
  -> 用正式 stdlib 替换第一代 stdlib
  -> 编译 archive lainc
```

这里的“第一代”只负责启动，不负责长期维护。它可以先用 LAIN-IR 写死必要的
AST、类型、模块和 lowering 接口；第二套编译器通过 ABI 调用它，不需要在自身
代码里重新判断这些高级语法。正式标准库接管后，第一代实现只作为带版本和
hash 的 bootstrap snapshot 保留。

替换是否真实发生，用三条检查确认：

1. 不改 compiler core，只替换 stdlib artifact，编译结果会随 stdlib 规则改变；
2. 第一代和正式标准库对同一输入的 AST、诊断、依赖和 LAIN-IR 能做一致性比较；
3. 默认构建改用正式 stdlib 后，第 2 套仍能直接编译 archive。

### 2.0.1 当前范围：暂不实现语言级所有权检查

这里的 `CompileContext.owner` 是运行时资源归属字段，用来防止跨上下文误用
AST、诊断或 L1 Unit；它不等于 Lain 语言的所有权系统。

当前里程碑暂不要求：

```text
移动语义检查
借用规则和 borrow checker
生命周期推导
所有权错误诊断
```

`std/ownership.lain` 可以继续作为独立策略模块保留和测试，但不进入 archive
可用性的必需路径。等基础自举链稳定后，再单独增加这组语言规则和一致性测试。

### 2.1 Meta

Meta 提供 AST 操作和由库定义的语言规则：

```text
读取节点       创建节点       复制节点       替换节点
遍历子树       保存 span      hygiene        产生诊断
识别库定义形式  将形式改写为更低层 AST 或 LAIN-IR AST
```

Meta 不执行 Lain procedure，也不拥有另一套表达式解释器。

### 2.2 `#eval`

`#eval` 是唯一的编译期执行边界：

```text
源码 AST
  -> 标准 Meta 库改写 AST
  -> 产生含 #eval 的 LAIN-IR
  -> LAIN-IR evaluator 执行 #eval
  -> 结果回到后续 AST / lowering 流程
```

### 2.3 编译器底座

第 2 套编译器只保留无法由普通库实现的能力：

```text
源码和 package 读取
RawAst 的无语义拓扑
AST 节点的存储、span、hygiene 和通用编辑原语
标准库 artifact 的加载和链接
LAIN-IR Unit builder、verifier、printer
#eval 的调度和执行
诊断、内存、文件等 capability 边界
```

它不得直接判断 `func`、`struct`、`module`、`import`、`effect`、`generic`
等名字的语言含义；ownership 也不进入当前核心链。

### 2.4 标准库

标准库拥有高级特性的全部规则：

```text
std::func / std::struct / std::module / import / attributes
类型和值的名字解析
泛型工厂和 specialization
effect / bounds
（ownership 暂为可选策略模块）
诊断格式和 source mapping
从语义 AST 到 LAIN-IR 的 lowering
需要计算时生成 #eval
```

验证一项能力是否真正属于标准库的方法是：替换标准库实现，不改编译器，
语言行为随标准库变化。

## 3. 当前代码事实

已经存在、可以复用：

- `src/lainir/lain/raw_ast.l1` 已有节点读取、复制、替换、删除、从文本建树、
  写回文本和基础 hygiene 操作。
- `run_ast_ops.py` 已覆盖 AST 变换及在 `#eval` 中返回 AST 地址的最小用例。
- `src/lainir/lain/meta_*.l1` 已包含 record、module、type、factory、binding 等
  第一代语义代码的大量原料。
- `scripts/build_lain_compiler.py` 能把多份 LAIN-IR 文件拼成冻结 artifact。
- seed 已有 LAIN-IR parser、verifier、interpreter 和受限 `#eval` 能力。
- Lain 版本的 `std/meta.lain`、`std/type.lain`、`std/generic.lain`、
  `std/effect.lain`、`std/bounds.lain` 已有初步代码和
  单独 fixture。
- 第 3 套可以生成并执行 archive API 的 empty/nonempty 探针，可作为迁移期间
  的行为参照。

当前缺口：

- 第 2 套把 `meta_*.l1`、`lower_program.l1` 与编译器底座静态拼在一起，
  没有独立标准库边界。
- 第 2 套仍直接识别多个高级形式的名字。
- `#eval` 已有局部探针，尚未成为标准库 lowering 的统一执行通道。
- Lain 标准库多数只通过单独 fixture，尚未掌握编译主线的语言行为。
- 当前 archive 可用性强验收依赖第 3 套；第 2 套只有 source-closure 和 schema
  级验收。
- “输出通过 verifier”不能证明生成的 archive compiler 可用。

## 4. Bootstrap Standard Library ABI v1

第一代 LAIN-IR 标准库和正式 Lain 标准库必须实现同一逻辑接口。B1 阶段会把
它冻结成物理 LAIN-IR 签名和 record layout。

### 4.1 编译器提供给标准库的接口

```text
AstApi
  node_kind / delimiter / span / syntax_context
  first_child / next_sibling / parent
  new_atom / new_group / copy / replace / remove
  fresh_symbol / attach_origin

SourceApi
  source_count / source_path / source_bytes
  import_edge / package_identity

IrApi
  new_unit / add_type / add_procedure / add_region
  add_expression / add_instruction
  verify / print

EvalApi
  execute_eval_block
  step_limit / allocation_limit / recursion_limit

DiagnosticApi
  add(code, severity, span, message)
```

这些接口只提供通用数据和操作，不出现任何 Lain 高级特性的名字。

### 4.2 标准库提供给编译器的接口

逻辑入口固定为：

```text
lain_std_abi_version() -> usize
lain_std_initialize(context) -> status
lain_std_expand(context, source_unit, root) -> MetaPassResult
lain_std_elaborate(context, expanded_root) -> MetaPassResult
lain_std_lower(context, elaborated_root, l1_unit) -> MetaPassResult
```

`MetaPassResult` 至少包含：

```text
status
result AST / semantic handle / L1 unit handle
diagnostic list
dependencies
changed flag
```

B1 实现时固定字段顺序、宽度、资源 owner、空值和错误规则。接口版本不匹配必须在
处理源码前失败，不能静默兼容。

### 4.3 运行时资源归属

这里记录的是 ABI 资源的生命周期，不是 Lain 语言的所有权系统。当前只要求：

- AST、semantic value、diagnostic 和 L1 Unit 带明确的运行时 owner 标记。
- 编译器拥有 source workspace 和最终 artifact。
- 标准库产生的临时对象在 pass 结束或转交 compile context 时统一释放。
- `addr` 只作为 ABI handle，不代表无类型的永久对象。

移动语义、借用规则、生命周期推导和 borrow checker 不属于当前自举阶段。

## 5. 目录目标

迁移后的目录职责：

```text
src/lainir/lain/
  compiler_driver.l1        调度，不包含高级语言规则
  compiler_context.l1       source、diagnostic、owner、limits
  ast_runtime.l1            RawAst 存储和稳定 AstApi v1
  ir_runtime.l1             L1 builder/verifier/printer 入口
  eval_runtime.l1           #eval 调度入口
  std_loader.l1             加载/连接标准库 artifact

src/lainir/bootstrap_std/
  meta_ast.l1               通用 AST 辅助
  core_forms.l1             func/struct/module/import/attribute
  type.l1
  generic.l1
  effect.l1
  bounds.l1
  ownership.l1             可选策略，不进入当前自举必需链
  lower.l1
  entry.l1                  Bootstrap Standard Library ABI v1

std/*.lain                  正式标准库源码，长期语义权威
src/compiler-archive/*.lain 最终 compiler，调用正式标准库
```

第一轮迁移允许文件仍在原位置，通过 build manifest 分类。B2 完成时再移动文件，
避免“只改目录，没有改依赖方向”。

## 6. 分阶段计划

状态标记：

```text
[ ] 未开始
[~] 实施中
[x] 已完成且通过对应验收
[!] 等待必须的外部决定或能力
```

### B0：冻结当前行为

目标：保存迁移前的输入、输出和失败边界。

- [x] 已保存 smoke 基线 `build/baselines/current-smoke-3/baseline.json`，其中
  `run_raw_ast.py`、`run_ast_ops.py`、`run_meta_record.py`、
  `run_meta_module.py`、`run_meta_eval.py` 的结果。
- [~] 迁移前完整基线保留在 `build/baselines/current-full-4/baseline.json`；第 2 套的
  source-closure（12.1 秒）和 compiler-API（71.5 秒）通过；archive usable 在
  150 秒上限内未完成，已记录 `timed_out=true`，峰值 RSS 约 110 MB。
- [x] baseline JSON 同时记录命令、返回值、耗时、峰值 RSS/VMS 和源码 hash；产物
  路径不写入源码 hash。

验收：每个结果都能在干净工作区重跑，失败也记录为失败，不把 verifier 通过写成
“compiler 可用”。

### B1：冻结底座 ABI

目标：标准库只通过 ABI 使用 AST、IR、`#eval` 和诊断。

- [x] 为现有 `raw_*` / `ast_*` 建立带版本的 `lain_ast_v1_*` 包装层，见
  `src/lainir/lain/ast_runtime.l1`。
- [x] 定义 `MetaPassResultV1`、`DiagnosticV1` 和 `CompileContextV1` 的物理布局，见
  `src/lainir/lain/compiler_context.l1`；owner、step/allocation/recursion limit
  和 capability mask 均有固定字段及消费接口；零预算表示不设限，递归预算支持
  enter/leave，单 capability bit 可查询。
- [x] `MetaPassResultV1` 已提供 owner 匹配和从旧 owner 转交到 CompileContext
  的 ABI 操作；`scripts/check_compile_context.py` 已覆盖成功转交、转交后旧
  owner 拒绝和 owner 匹配负例。
- [x] compiler API 在 expand、elaborate、lower 三个边界校验结果 owner，错误
  owner 返回 `5202`，不会把外部结果继续交给下一阶段。
- [~] 五个 `lain_std_*` ABI 入口均已定义并接入编译器调度；当前
  expand/elaborate 仍是 identity pass，lower 通过内部 hook 调用现有 lowering，
  语义迁移尚未完成。
- [~] 已覆盖错误版本、空 pass-result handle 和跨 owner handle 负例；空 AST/IR
  对象和重复释放语义仍待补齐。空 pass-result 会在边界返回 `5202`，不会被解引用。
- [x] 添加 `tests/lainir_lain/run_bootstrap_std_bundle.py`，覆盖 ABI、AST wrapper、
  pass-result 布局和合并包执行。

验收：一个只有“把输入 AST 原样返回”能力的假标准库可以被第 2 套加载并运行；
替换假标准库不需要改编译器源码。

### B2：拆出第一代 LAIN-IR 标准库

目标：把当前混在第 2 套中的高级语义代码变成独立 artifact。

- [~] 已先拆出独立 `bootstrap_std.l1` 身份包并保留 core/std 两份 artifact；并将
  `module/struct` 的状态识别策略迁到 `bootstrap_std/core_forms.l1`；
  `module_meta.l1`、`meta_module.l1`、`meta_record.l1`、`meta_type.l1`、
  `meta_import.l1`、`meta_eval*.l1`、`meta_call.l1`、`meta_collect.l1` 已通过
  build manifest 纳入标准库包（首轮仍保留原路径）。这些文件现在通过
  `core_eval_contracts.l1` 声明所需的底座接口，核心包不再携带这组实现。
- [~] 源码扫描器已从旧 monolith 抽出到 `src/lainir/tools/source.l1`；求值、Meta、
  lowering、workspace/import 和 syntax-index 实现已从 compiler core 清单移入
  bootstrap stdlib 清单。`scripts/build_lain_compiler.py` 现在会同时构建并校验
  三个 artifact，缺失的职责模块会在构建阶段直接暴露。
- [x] `raw_ast` 存储、compiler context、IR builder 和 eval 调度留在底座；标准库
  只通过这些底座接口取得句柄并提交 pass 结果。
- [x] 分别生成 `build/lainir/lain_compiler_core.l1` 和
  `build/lainir/bootstrap_std.l1`，再由 bundler 连接。
- [x] build stamp 分别记录 compiler core 和 bootstrap stdlib 的输入/输出 hash。
- [x] 添加 `tests/lainir_lain/run_bootstrap_std_bundle.py`；它会检查模块、记录、类型、
  导入和求值实现确实只出现在标准库 artifact，并验证替换标准库时核心包无需重编译。

验收：拆分前后现有 AST/Meta fixture 输出一致；当前已验证 compiler core artifact
不再包含 module、record、type 的实现。

### B3：让核心形式由第一代标准库决定

目标：删除第 2 套对高级形式名字的直接判断。

迁移顺序：

```text
attributes/import
  -> module
  -> struct/type alias
  -> func/call
  -> const/comptime surface
```

- [~] `module/struct/record/type` 的状态识别已通过 `lain_std_meta_status` 由
  bootstrap stdlib 提供；完整 AST handler 和 lowering 仍在迁移中。每迁移一种形式，先在 bootstrap
  stdlib 注册 AST handler，再删除 compiler
  中对应的字符串判断。
- [~] `lain_std_expand` 已遍历 syntax index，并对每个源单元调用
  `lain_std_meta_status`；因此 module/struct/record 的状态诊断已进入真实的
  stdlib pass 调度，而不再只是单独的探针；现在也会在 expand 阶段传播
  syntax-index 的未解析 import/循环依赖诊断。完整 AST 变换和 lowering 仍待迁移。
- [~] compiler API 已只调用 `lain_std_expand/elaborate/lower`；完整 lowering
  实现已经进入 bootstrap std artifact，并通过 `lain_std_lower_program` 暴露，
  但 expand/elaborate 的完整 AST 变换和正式 ABI 仍待完成。普通运行函数中
  出现局部 `std::module`/`std::struct` 时不会再被误判为 Meta 函数。
- [x] 加 boundary lint：`scripts/check_lainir_boundaries.py` 检查 core artifact
  不含高级形式字符串和语义实现过程，并确认对应实现存在于 bootstrap stdlib；
  构建脚本每次都会运行该检查。
- [x] `scripts/check_stdlib_swap.py` 已验证：只替换 bootstrap stdlib 中的
  `lain_std_meta_status`，不重建 core，编译行为会变为替换库提供的诊断码。
- [~] evaluator 的统一入口已放进 bootstrap stdlib，并在入口内部通过显式
  `#eval` block 调用原有 evaluator；现在已具备正确的执行边界，待下一步把
  AST -> 完整 L1 Unit 的生成也移到这个入口。
- [~] archive Meta 探针已覆盖未知名称诊断；未知构造器、重复注册、无限
  expansion 和无效返回 AST 负例仍待补齐。

验收：修改 bootstrap stdlib 对一种测试形式的 lowering，编译器不重建也能得到
变化后的结果；恢复标准库后现有 fixture 全部通过。

### B4：接通通用 `#eval`

目标：编译期计算只通过 LAIN-IR 执行。

- [~] 标准库 evaluator 已通过 `#eval` block 执行并返回 `EvalResult`；结果现在带有
  scalar/meta/type/module/syntax 标签和独立 owner 字段，CompileContextV1 已提供
  有界/无界预算、capability 查询和 owner 字段，最小 L1 Unit 与 artifact 回灌 smoke
  已通过，完整 Unit 生成和对象结果矩阵尚未完成。
- [~] Meta step limit 已从 `program_unit_meta_step_inc` 的硬编码移到 unit ABI
  字段，并由 compiler request 创建的 CompileContext 注入；request 现在可配置
  step/allocation/recursion/capability 四类预算。带 CompileContext 的 unit 已将
  step 消费和递归 enter/leave 接入 `program_eval_function`，递归超限返回
  `5125`；seed 的 `bootstrap.allocate-pages` 已接入 allocation limit 下发接口。
  allocation 的逐对象计费、capability 的实际使用和 native host 诊断仍待补齐。
- [~] evaluator 的 `EvalResult` 已能携带 scalar、type/module/AST handle 及类型标签，
  并拒绝 tagged Meta nil 对象（返回 `5108`）；真实 `#eval` 输入分别返回这三类
  对象的完整矩阵仍待补齐。
- [~] pass 结果已在 compiler API 中校验并带 CompileContext owner 后交给下一阶段；
  不同 owner 的真实转交和后续 AST/Unit 生命周期仍待接通。
- [~] 递归和 step 已有独立消费与诊断，allocation 已接入 seed host 上限；逐对象
  allocation 计费、capability 的实际调用约束和统一诊断仍待补齐。
- [x] `scripts/check_compile_context.py` 已验证 step/recursion 的成功、超限和
  leave 后重入，以及 capability 查询和 owner 转交的 ABI 行为。
- [~] `#eval` 标量表达式现在递归处理括号组，并继续使用统一的
  `program_eval_consteval_group` 边界；嵌套算术已用实际 Lain 源码验证。
- [~] Meta 中原先两套二元表达式求值已合并为一条带词法环境的实现，避免
  无环境入口与环境入口产生不同的运算行为。
- [~] 第 2 套已删除独立整数 consteval 解释器：bootstrap std artifact 统一走
  `program_eval_consteval_arithmetic`；旧 `eval.l1` 仅由尚未迁移的 inspection
  frontend 兼容入口引用，待该入口切换到 compiler API 后删除文件。
- [x] `run_std_eval_bridge.py` 已检查标准库 artifact 含 `#eval` 入口，并通过
  consteval 编译、verifier 和运行回归；独立的 type/module/AST 返回值矩阵仍待补齐。

验收：同一个 fixture 覆盖“Meta 生成 `#eval` -> 执行 -> 返回 AST -> 继续
lowering”，并证明禁用 stdlib 后 compiler 不会自己识别或执行该高级形式。

### B5：用第一代标准库编译正式 Lain 标准库

目标：让 `std/*.lain` 从测试样品变成编译主线的真实依赖。

- [~] 已通过 bootstrap artifact 编译并运行正式 `std::meta` 核心（见
  `run_stdlib_bootstrap.py`）；type、module、diagnostic、
  generic、effect、bounds 和 backend contract。
- [x] 正式 `std/*.lain` 的完整 source closure 已由第 2 套生成
  `build/formal-stdlib-fixed.l1`，并通过 `lainir-print` 的普通解析/验证；
  本轮修复了“单语句字段赋值被误判为返回表达式”的路径，`&mut Diagnostic`
  等字段更新现在走正常赋值降级。
- [x] `scripts/build_formal_stdlib.py` 已固定正式标准库闭包的源码清单、排序、
  hash manifest 和验证步骤；当前产物为 `build/lainir/formal_stdlib.l1`，全部
  `std/**/*.lain` 均可生成 verifier-valid LAIN-IR。
- [~] Meta 工厂内部的局部 `std::func`、`Module`/`ModuleShape` 返回值和
  `effects.Handler` 返回值已按编译期对象处理，避免生成未定义的 `%module`；
  正式标准库现在已导出五个 `lain_std_*` ABI 名称，expand 已完成首个根节点
  的 AstApi 复制且复制动作由 `std::meta` 库函数委托给 seed ABI，elaborate 仍为
  句柄传递，lower 在语义迁移完成前明确返回
  `5203`；十进制常量 `return`、简单二元算术（`+`、`-`、`*`、`/`）、同源无参数
  调用（无参数或一至两个十进制参数）、单参数及括号包裹的算术表达式、局部标量绑定及其调用参数和布尔/数值比较的
  `if ... else` 双分支已完成正式库 lowering 及 artifact 回灌，
  不完整表达式会被拒绝，一般控制流和完整 L1 Unit 仍待迁移。
- [~] 正式 `std::meta` 已导出 AstApi 的只读访问包装和 `meta_ast_count`；
  `std/bootstrap/abi_entry.lain` 不再直接声明
  `raw_node_first/next/kind/delimiter/start/length` 和 `raw_is_nil`，
  这些调用经过正式 Meta 库再落到 seed ABI；源码数据和字节读取也经过
  `meta_source_data/meta_source_byte_at`，source handle、词法和解析入口
  经过 `meta_source_new/meta_source_lex/meta_source_parse`。下一步继续
  将这些 SourceApi 操作从 ABI 薄包装扩展为正式的源模型。
- [~] 每个正式库导出与 Bootstrap Standard Library ABI v1 对应的入口；
  `std/bootstrap/abi_entry.lain` 使用专用 `@abi_export` 导出五个物理符号，
  `scripts/build_formal_stdlib.py` 会检查并运行版本入口。正式 Meta/lowering
  实现仍待接入这些入口；当前正式库会解析全部输入源，并为每个源生成真实的
  syntax-index 条目（源句柄、根节点、首节点、节点数、import 数和根 span）；
  expand 已通过 `std::meta` 库函数调用 AstApi 复制首个根节点；正式库已在
  `syntax_index_import_status` 中按 source path 解析本地 import 和
  `std::...` 逻辑路径，缺失普通模块返回 `4101`；同一入口使用三色 DFS
  检测本地依赖环并返回 `4103`。elaborate 仍只传递句柄，lower 对常量
  `return`、简单二元算术、同源无参数/一至两参数调用和布尔/数值比较双分支已生成可执行
  artifact；`unit` 空返回、单参数算术表达式、括号表达式、局部标量绑定及局部绑定调用已加入正式库验证，
  其余形式明确返回 `5203`。
  `run_lain_compiler.py --stdlib-artifact build/lainir/formal_stdlib.l1`
  已验证实际替换链。
- [x] 生成版本化标准库 artifact 和 manifest，记录 ABI、source hash、依赖和
  target-independent 标记；产物为 `build/lainir/bootstrap_std.manifest.json`。
- [x] 添加 `tests/lainir_lain/run_stdlib_bootstrap.py`。
- [ ] 将现有 `run_std_*.py` 从单独函数测试升级为通过标准库 artifact 调用。

验收：第 2 套只依赖第一代标准库，就能编译完整正式标准库；所有正式标准库
fixture 使用编译后的 artifact 运行。

### B6：第一代与正式标准库一致性

目标：证明正式标准库可以替换第一代标准库。

- [~] `scripts/check_stdlib_conformance.py` 已把共同支持的常量、算术、无参数调用、
  单参数调用和双参数调用分别交给第一代与正式标准库；过程名哈希和透明 `#eval`
  包装会在 canonical 比较时消除，六个样例的生成 IR 与运行值一致；同一脚本还
  比较了可解析本地 import、未解析 import（`4101`）和循环依赖（`4103`）的结果。
- [~] 已将一致性比较扩展到本地 import 的成功、未解析诊断和循环依赖诊断；
  完整 AST、所有诊断种类和依赖对象内容仍待补齐。
- [ ] 对 func、struct、module、import、type factory、generic、effect、bounds、
  attribute 和 `#eval` 各设正负例；ownership 暂不纳入必需矩阵。
- [ ] 差异报告精确到第一个 pass、节点 span 和 IR procedure。
- [ ] 添加 `tests/lainir_lain/run_stdlib_conformance.py`。

验收：一致性矩阵全部通过；之后默认加载正式标准库，第一代标准库只用于从 seed
开始的冷启动。

### B7：第 2 套直接编译 archive lainc

目标：不经过第 3 套，得到可执行的 archive compiler。

- [x] `run_meta_invoke_environment.py` 已用第 2 套直接编译并执行 archive Meta
  结构调用、AST 克隆、字符串 ABI、token 名称解析、alias 查询和未知名称诊断；
  它覆盖的是 Meta 子系统，不等于完整 compiler API。
  `run_lainc_archive_usable.py` 已覆盖完整 archive API 的 empty/nonempty 客户端。
  `src/lainir/lainc.l1 + 正式标准库 artifact`。
- [x] empty/nonempty API client 已由第 2 套生成并执行到 0/0，且产物通过
  verifier。
- [x] 编译全部正式 std 和 `src/compiler-archive` 源码；
  `tests/lainir_lain/run_lainc_full_std_archive.py` 已保存可复现验收路径。
- [x] API 返回的非空 artifact 已能通过可选的 `LAINIR_RUN_ARTIFACT` 输出通道
  回灌成新的 `.l1` 文件；第二个 seed 进程重新解析、校验并运行该文件的
  `main`。对应验收使用
  `compiler_api_compile_emit_artifact.lain` 和
  `LAIN_FULL_REFEED_ARTIFACT=...`；可直接运行
  `tests/lainir_lain/run_lainc_archive_refeed.py`。
- [x] 增加简单真实用户程序，验证 archive 输出可再次交给 seed/verifier/backend；
  `return 42` 已经从用户源代码进入 artifact，并由第二个 seed 进程执行得到 42。
- [x] 修正 archive lowering 中的几个物理 ABI 缺口：Surface.Function.name 使用
  16 字节偏移；`ExprIdVector.push` 将整数编号先写入临时槽再按地址传递；
  这些修正已重新通过 gen2/gen3 固定点和 archive artifact 回灌。
- [~] 已把真实用户程序 gate 扩展到无参数的标量 `return` 和简单整数算术链
  （`+`、`-`、`*`、`/`）；完整表达式语义、函数调用和控制流仍待实现。对应
  验收使用 `tests/lainir_lain/run_lainc_archive_arithmetic.py`。
- [~] 已禁止未解析的普通成员路径（例如 `missing.member`）继续进入物理
  lowering；缺少 receiver binding 时现在返回诊断 `5108`，不再生成
  `%missing` 的零偏移加载；writer 的未知字段路径也不再猜测类型或输出
  offset-zero load，未知成员赋值也不会生成零偏移写入。合法 record 成员
  的读写已通过小例验证。局部 `Module` 别名现在会进入同一 Meta 环境，合法
  的 `M.value` 已生成正确标量返回，未知模块成员仍返回 `5108`；工厂返回的
  record type 也能在局部绑定后实例化并完成字段读写。post-call projection
  仍由已验证的调用解析路径处理，factory 和完整模块成员覆盖待补齐。

验收：第 2 套生成的 archive artifact 通过四项强 gate：

```text
格式合法
API empty 可运行
API nonempty 可运行
真实用户程序可编译和运行
```

### B8：archive 自举和正式切换

目标：第 4 套接管编译器主线。

- [ ] archive lainc 编译正式标准库和 archive 自身，得到 gen2。
- [ ] gen2 用相同输入得到 gen3。
- [ ] canonical gen2 == gen3；verifier、ABI manifest 和 procedure body hash 一致。
- [ ] 从干净目录只用 seed 和已提交的第一代标准库重建整条链。
- [ ] 生成 native `lainc`，编译并运行一组真实用户程序。
- [ ] LSP、CLI 和测试 runner 使用同一正式标准库 artifact/version。

验收：干净目录自举、固定点、native 使用和完整回归同时通过。

### B9：移除过渡代码

目标：项目结构与设计原则一致。

- [ ] 第 3 套从默认构建、发布和回归入口移除。
- [ ] 确认没有独立调试用途后删除 `src/lainc/lainc.lain` 的 compiler 实现；
  可保留与最终 backend 无重复的工具。
- [ ] 删除第 2 套剩余的高级形式字符串判断和 archive 专用物理偏移。
- [ ] 删除 archive Meta 中承担编译期 procedure 解释的代码；Meta 只保留 AST
  操作和 expansion 调度。
- [ ] 第一代标准库保留为带 ABI/version/hash 的 bootstrap snapshot。
- [ ] 更新 README 和所有旧路线图指向本文。

验收：boundary lint 无例外；删除过渡实现后仍可从 seed 完成 B8 全链。

## 7. 测试矩阵

| Gate | 证明什么 | 允许使用第 3 套 |
|---|---|---|
| AST ABI | AST 底座稳定、资源 owner 正确 | 否 |
| Bootstrap std bundle | 第 2 套通过接口调用第一代标准库 | 否 |
| Core-form delegation | 高级形式不在 compiler core | 否 |
| `#eval` bridge | 编译期计算由 LAIN-IR 完成 | 否 |
| Stdlib bootstrap | 第一代标准库能编译正式标准库 | 否 |
| Stdlib conformance | 两代标准库可替换 | 否 |
| Archive usable | 第 2 套产出可工作的第 4 套 | 否 |
| Archive fixed point | 第 4 套能编译自身且收敛 | 否 |
| Migration reference | 对照迁移前行为 | 是，仅作参照 |

任何 gate 都不能只检查输出中存在某个名字。必须检查生成物、诊断和运行结果。

## 8. 提交顺序

每次提交只跨一个接口边界：

```text
1. ABI / record layout / wrapper
2. 对应正负测试
3. 一个高级形式迁到 bootstrap stdlib
4. 删除 compiler core 中同一形式的实现
5. 跑该形式、ABI、source-closure 三组回归
```

禁止同时迁移多个形式后再补测试。禁止用 verifier-safe stub、默认零值、隐式 extern
或固定 archive 偏移让 gate 表面通过。

## 9. 性能和缓存

功能边界稳定前，只保留已有 trace 和预算保护，不以缓存掩盖重复求值。

从 B5 开始，cache key 必须包含：

```text
compiler substrate ABI
stdlib ABI 和 artifact hash
source/package identity
target/backend identity
capability set
```

每次性能报告至少记录：总耗时、各 pass 耗时、峰值 RSS、`#eval` 次数、cache
hit/miss 和 artifact hash。

## 10. 完成定义

只有下面条件全部满足，路线图才完成：

- 第 2 套通过稳定 ABI 调用第一代标准库。
- 编译器底座不含 Lain 高级形式语义。
- Meta 只负责 AST 操作和变换。
- 所有编译期执行都经 `#eval`。
- 第一代 LAIN-IR 标准库能编译正式 Lain 标准库。
- 正式标准库通过两代实现一致性测试并成为默认语义来源。
- 第 2 套直接产出可用的 archive lainc。
- archive lainc 编译自身，gen2/gen3 固定点成立。
- native `lainc` 能编译和运行真实用户程序。
- 干净目录只依赖 seed、提交的 bootstrap std snapshot 和源码即可重建。
- 第 3 套不再位于自举、发布或默认测试主线。

## 11. 立即执行的十个任务

```text
T01 [x] 保存 B0 当前 baseline JSON 和 canonical artifact hash
T02 [x] 定义 AstApiV1 的 wrapper 名称、参数、资源 owner 和错误规则
T03 [x] 定义 MetaPassResultV1 / DiagnosticV1 / CompileContextV1 布局，固定
    owner、预算、capability 和错误返回规则
T04 [x] 实现 identity bootstrap stdlib 和 ABI 正负例
T05 [x] 将 compiler core 与 bootstrap stdlib 分成两个 build artifact
T06 [x] 先把 module/struct 识别迁到 bootstrap stdlib
T07 [x] 删除 compiler core 对 module/struct 的直接字符串判断
T08 [~] 接通由 bootstrap stdlib 生成的真实 #eval 路径；入口、预算和运行回归已通过，
    完整 L1 Unit 生成及结果 owner 转交仍在补齐
T09 [x] 用该路径编译正式 std/meta AST 核心
T10 [ ] 建立第一代/正式标准库的第一个 canonical AST 对照测试

这十项完成后，按下面的主线继续推进，不再增加第三套编译器的功能：

```text
阶段 A：接口和边界
  固定 AstApi、SourceApi、IrApi、EvalApi、DiagnosticApi 的 ABI
  固定 owner、预算、capability、错误码和 artifact manifest
  用 identity stdlib 验证“换库不改 compiler core”

阶段 B：第一代标准库接管语义
  依次迁移 attributes/import、module、struct/record/type、func/call、
  generic/effect/bounds 和 lowering；ownership 作为后续可选模块
  每迁移一项，就删除 compiler core 对该项的名字判断，并加入正负例

阶段 C：把 #eval 变成唯一的编译期计算通道
  Meta 只创建或改写 AST，以及生成带 #eval 的 LAIN-IR
  evaluator 只执行 LAIN-IR 的 #eval
  补齐 scalar、type、module、AST 四种返回值和资源 owner 转交测试

阶段 D：正式标准库替换第一代实现
  由第一代 LAIN-IR 标准库编译 std/*.lain
  比较两代库的 AST、诊断、依赖、LAIN-IR 和运行结果
  一致性通过后默认使用正式标准库，第一代只保留为 bootstrap snapshot

阶段 E：第二套直接产出 archive
  第 2 套直接编译全部 std 和 src/compiler-archive
  验收格式、空 API、非空 API、真实用户程序四个 gate
  禁止 unresolved member 被静默改成 extern、空地址、固定偏移或零值

阶段 F：archive 自举并切换主线
  archive 编译正式标准库和自身，得到 gen2；再次编译得到 gen3
  检查 canonical 内容、verifier、ABI manifest 和 procedure body hash 一致
  生成 native lainc，跑真实程序、LSP、CLI 和测试 runner
  从干净目录重建后，移除第三套编译器和遗留的高级语义判断
```

每个阶段的完成条件都是可执行检查，不以“代码已经拆到某个目录”作为完成依据：

```text
接口完成       -> ABI 负例、owner/预算检查通过
语义迁移完成   -> compiler core 无该特性判断，替换 stdlib 后行为会变化
#eval 完成      -> 同一 fixture 能生成、执行并消费返回值
正式库替换完成 -> 两代标准库一致性矩阵通过
archive 可用   -> 四个强 gate 全部通过
自举完成       -> gen2 == gen3 且 native/clean rebuild 通过
```
```

T01-T04 完成前，不继续扩展第 3 套。T05-T08 完成前，不宣称高级特性已经由
标准库实现。B7 完成前，不宣称第 2 套已经编译出可用的 archive lainc。
