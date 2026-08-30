# Lain Meta、archive 自举与后端路线图

> 基线日期：2026-08-30  
> 本文是当前执行计划。早期阶段记录见 `docs/lainc-archive-plan.md` 和
> `docs/lainc-bootstrap-roadmap.md`。

## 1. 总目标

让 Lain 的高级语义主要由 Meta/标准库提供，compiler core 只保留少量
不可下放的机制，并完成从 C bootstrap 到 Lain-written `lainc` 的自举闭环：

```text
C lainir-seed
    ↓ 执行冻结的 LAIN-IR compiler
src/lainc/lainc.lain
    ↓ 编译 archive + std
archive lainc
    ↓ 编译自身和用户程序
可运行的 lainc / backend
```

目标边界：

```text
Meta 层       类型、泛型、模块、effect、bounds、ownership、宏、consteval
LAIN-IR core  表示层类型、CFG、SSA、调用签名、地址操作、最终 verifier
宿主能力      文件、内存、诊断、Clang/LLVM、目标文件和进程接口
```

高级语义不得因为实现方便而重新硬编码进 `src/lainc` 或 LAIN-IR verifier。

## 2. 当前基线

### 已完成

- `std/meta.lain` 提供 Meta value、类型和模块 interning、callable registry、
  phase 标记以及基础 consteval 支持。
- `src/compiler-archive/meta.lain` 已接入标准 Meta，并有 `evaluate`、
  `invoke_program` 和诊断边界。
- `src/lainc/lainc.lain` 已支持多源 archive 编译、模块工厂、类型表、记录
  布局、部分泛型/consteval 以及固定点生成。
- gen1/gen2 可以生成完整 std + archive 的 API artifact。
- 生成 artifact 已通过 `lainir-print` verifier。
- `compiler_api_compile_empty.lain` 端到端执行成功并返回 `0`。
- standalone Meta、record、consteval、source-closure 和 archive-usable 回归已
  通过；此前 `f0_sb_new` 的 `%y1048576` 未定义值来自旧 gen2 产物，更新 fresh
  fixed-point cache 后已消失；
  module fixture 的 namespace、qualified call、constant member 和 consteval
  member 四项均已通过。
- M1 的结构化调用 fixture 已接入：Lain 能生成包含真实 L1 record 的 Meta
  invocation artifact，artifact verifier 和运行时 gate 均通过；archive registry
  已改为可持久化的标量槽表，记录 callable 的 id、名称哈希、procedure、phase、
  program，并支持按注册序号做 phase/program 校验。syntax invocation 当前对已
  校验 callable 使用 hygienic clone fallback；完整 interpreter/factory dispatch
  和可靠的名称字符串解析仍待接通。
- `lainir-seed run` 已提供生成 self-hosting artifact 所需的最小 allocator
  capability（`allocate-pages`/`release-pages`）；其余宿主 capability 仍由
  bootstrap 编译命令提供。
- `seed` 的 run/bootstrap allocator 现在登记外部页，支持显式释放，并在运行
  结束时统一回收；bootstrap 可通过 `LAINIR_BOOTSTRAP_MAX_ALLOC_BYTES` 对宿主
  页和解释器 `#alloca` 设置同一预算，run 可通过 `--max-alloc-bytes` 设置预算。
- 已加入 artifact canonicalizer（extern/procedure 排序、换行和尾随空白规范）
  及 allocator capability 正负例。
- module acceptance runner 已给每个编译步骤加超时；`module_consteval_member`
  现已在 120 秒边界内完成并返回 36。
- `meta_stack_pop` 已修复小型 `StringBuffer` 长度字段的写入偏移：长度写入
  header offset 8，不再覆盖 sentinel/data-pointer 字段；因此 module consteval
  的 `math.square(6)` 不再错误折叠为 0。

### 尚未完成

- `Meta.invoke` 对任意 syntax-producing procedure 的 AST 执行仍缺少可运行的
  archive registry/interpreter factory；当前 program/environment 数据结构已有
  最小 scalar ABI，当前 evaluator 只覆盖 bounded scalar expression，factory
  实例化、完整 registry dispatch 和返回 record lowering 仍不完整。
- archive 中部分 Model/Builder 数据结构仍是验证路径所需的最小实现，尚未具备
  完整运行语义。
- `std::type`、`std::generic`、`std::bounds`、`std::effect`、`std::ownership`
  尚未形成统一的可替换策略接口。
- Meta 参数、闭包 specialization、物理 lowering 尚未使用统一的
  specialization context 和 cache key。
- 尚未得到可以编译普通用户程序并输出 native binary 的最终 `lainc`。
- 编译性能仍受 archive 工厂展开、重复 Meta 求值和文本 emitter 影响。
- Windows 宿主分配已按大小分流：大块走 demand-zero `VirtualAlloc`，小块由 bootstrap
  arena 分块提供；解释器调用参数和 primitive 操作数的常见小数组也改为栈内联缓存。
  这避免了约 250 万次 8/16 字节 CRT 分配造成的堆 churn。最近一次 gen1 编译约
  206 秒，驻留内存采样保持在几十 MiB；预算保护、
  所有权登记和未知地址拒绝仍保留，临时 Meta/字符串缓冲的生命周期压缩属于
  后续 M6 工作。
- `run_lainc_m2.py` 在最新 emitter 变更后仍通过 fixed-point gate；本轮测得
  gen2/gen3 为 436883 bytes，`lainir-print` 均通过。自举过程的直接采样峰值约
  49 MiB，没有观察到持续增长到 GiB 的情况。
- archive Meta invocation 夹具的 scalar 子路径已通过：显式传入
  `DirectUnitFacts` 后，带环境捕获的 parameter-0 procedure 返回 `5`，缺失
  program 和 phase mismatch 也走稳定诊断码；archive 中 `phase as usize` 已按
  标量零扩展处理，避免把 i32 误送入 `ptr2int`。原始 `&L1.Unit` direct bridge
  仍不能跨 factory 传递完整类型；这条路径暂不作为 M1 验收依据。
- syntax 子路径已补上最小 `TokenStream` ABI、TokenKind 常量、空输入分支、
  `Unit.root` 显式布局访问和 trivia 跳过；`Syntax.parse` 能为非空输入建立
  token/node 记录。`Meta.expand` 运行时 gate 已通过，并验证了 procedure-derived
  节点链、列表合并和 source span 保留；算术 syntax fixture 已验证 parameter-0 +
  constant + binary expression 的真实 procedure body 求值；完整用户自定义 AST
  procedure 和 registry evaluator 仍未完成。
- 针对用户报告的 seed 内存增长，allocator 回归已通过；bootstrap arena 的小对象现在
  带有释放头，`release-pages` 可回收单个对象，并在 arena 变空时立即释放分块页；大块
  宿主页仍走 demand-zero `VirtualAlloc`，预算和未知地址检查继续生效。seed verifier 的 `infer_expr_type`
  也已改为复用共享的物理类型值，避免查询型推导产生无主临时分配。

## 3. 里程碑

### M0：稳定当前固定点

目标：把当前可工作的 archive 路径冻结成可靠基线。

任务：

- 修复并接通 `run_all.py` 中的 M1、M2、archive-usable 回归。
- 固定生成 artifact 的格式和入口命名。
- 为 `lainir-seed run` 的 allocator capability 增加独立测试。
- 记录 gen1、gen2、archive product 的大小、耗时和 verifier 结果。

验收：

```text
run_lainc_m1.py                         → product 执行到 42
run_lainc_m2.py                         → gen2 == gen3
run_lainc_archive_usable.py             → archive api.compile(empty) 返回 0
lainir-print <archive-product> main     → verifier 通过
```

### M1：完成 Meta.invoke 执行环境

目标：让 Meta 可以执行真实的 syntax-producing procedure，不依赖 identity
特例。

核心对象：

```text
MetaProgram
MetaEnvironment
MetaProcedure
MetaValue
MetaDiagnostic
```

任务：

- 为 procedure id 保存所属 program、参数描述、body 和 phase。
- 为调用建立参数 frame、局部 binding、闭包捕获和返回值。
- 区分 `runtime`、`comptime`、`syntax`、`backend` callable phase。
- 将外部 capability 检查放在 environment 中，而不是散落在 evaluator 中。
- 把失败统一转换为带 source span 的 Meta diagnostic。

验收：

- 用户定义的 Meta procedure 可以构造并返回 AST/Meta value。
- procedure 可以调用另一个受允许 phase 的 Meta procedure。
- 缺失环境、phase 不匹配和外部 capability 调用都有稳定诊断。
- archive `Meta.expand` 能走真实 procedure，而非返回预置对象。

### M2：标准 Meta 语义库

目标：把高级语言规则迁移到标准库，形成共享的 elaboration 接口。

建议接口：

```text
resolve(name, environment)
check(node, environment)
elaborate(node, environment)
lower(node, context)
diagnose(error, source)
```

库划分：

```text
std/type       名称、类型值、转换、重载和类型诊断
std/generic    参数、约束、实例化和 specialization
std/bounds     checked / verified / unchecked 访问策略
std/effect     effect 集合、传播、处理和 capability 检查
std/ownership  borrow、move、alias 和生命周期策略
std/diagnostic 统一错误对象、span、notes 和 fix-it
```

设计要求：同一个源码构造可以挂接不同策略库；策略差异最终体现为不同
Meta elaboration，不能要求 LAIN-IR 增加一套高层类型系统。

验收：

- 至少有两种可替换 type/bounds policy。
- Meta 错误和 IR verifier 错误可以清楚区分。
- checked、verified、unchecked 数组访问都能生成 verifier-valid IR。
- archive 编译器只依赖这些库的接口，不直接读取策略内部表格。

### M3：统一 specialization 与 physical lowering

目标：解决 Meta 参数经过 closure specialization 后丢失的问题。

建立统一上下文：

```text
SpecializationContext
TypeArgumentVector
ModuleEnvironment
LayoutEnvironment
PhysicalTypeMap
SpecializationKey
```

`SpecializationKey` 至少包含：

```text
代码身份
Meta 参数
外层环境 fingerprint
目标平台
backend 版本
```

任务：

- 参数绑定、模块绑定、类型 alias 和 record layout 全部从同一 context 查询。
- closure specialization 完成后显式生成 physical type map。
- 禁止 emitter 通过字段名猜测泛型参数的物理宽度。
- 为 `Vec(T)`、record、Meta callable 和 backend fragment 共用实例化路径。

验收：

- 泛型参数能从 Meta 一直传到函数签名、字段访问和 call operand。
- 同一 specialization key 只生成一次物理实例。
- 不同模块的同名成员不会产生错误复用。
- nested namespace、record literal、方法调用和闭包捕获回归保持绿色。

### M4：Lain-written LAIN-IR backend

目标：用 Lain 实现从稳定中间表示到目标代码的后端主干。

阶段一先输出 verifier-valid LAIN-IR 或结构化 C/LLVM bridge；阶段二再输出
native object/executable。后端应通过 Meta registry 注册，不写入 compiler core
的 LLVM 特判。

任务：

- 定义 backend input contract：函数、数据、外部符号、target facts。
- 实现 data layout、calling convention、基本块和指令选择。
- 输出 object、link manifest 和诊断信息。
- 保留一个 capability-only host，使 compiler core 不依赖操作系统细节。

验收：

- archive `lainc` 可以编译一个包含函数、记录、调用和条件分支的用户程序。
- 生成的二进制能运行并返回预期结果。
- verifier 能在 backend 输出进入链接前发现表示层错误。

### M5：typed foreign C++/LLVM fragment

目标：利用 LLVM C++ template API，同时避免 C ABI 造成的类型信息擦除。

Meta 库提供类似：

```lain
cpp::type("llvm::Instruction")
cpp::instantiate(llvm.cast, Instruction)
cpp::fragment { ... }
```

任务：

- 保存 C++ type、template argument、overload set 和 foreign expression AST。
- 以 fragment 为单位调用 Clang，而非每个 API 调用启动一次编译。
- 返回 opaque foreign artifact 和 source-mapped diagnostic。
- 将 foreign fragment 纳入 M3 的 specialization cache。

验收：

- `llvm::cast<T>`、`IRBuilder<>`、`SmallVector<T, N>` 至少各有一个端到端样例。
- 模板参数在 Clang specialization 前保持可见。
- C ABI 只作为最终装载入口，不承担模板语义。

### M6：增量编译与性能

目标：让 archive 编译从“能完成”变成“可持续开发”。

任务顺序：

1. 保留现有 phase trace：读取、解析、Meta binding、specialization、lowering、
   emission、verify、run。
2. 为每个 phase 记录 wall time、调用次数、输入规模和 cache 命中率。
3. 增加 parse cache、Meta evaluation cache、layout cache 和 specialization cache。
4. 建立 source dependency graph，只重编译受影响的模块。
5. 最后再考虑并行化独立 source unit；并行化不能改变 Meta binding 顺序。

验收：

- 每次编译都生成本地 profile 文件。
- 无源码变化的第二次 archive 编译显著减少 Meta 求值和 specialization 时间。
- 缓存失效由源码、Meta 环境、target 和 backend 版本共同决定。
- 性能优化不改变 gen2/gen3 固定点和 artifact 内容。

### M7：发布级自举闭环

目标：从 C bootstrap 过渡到可独立使用的 Lain-based `lainc`。

验收链：

```text
C lainir-seed
    → frozen compiler
    → gen1 lainc
    → gen2 archive lainc
    → gen3 archive lainc
```

必须满足：

- gen2 和 gen3 的 canonical artifact 一致，或差异可以由明确的 formatter
  规范消除。
- archive `lainc` 可以编译自身、标准 Meta 库和一个真实用户程序。
- `build/laincexe` 不再依赖 seed 解释执行高级语义。
- seed 只保留启动、文件、内存、诊断和明确声明的外部 capability。
- LSP、CLI、测试 runner 共用同一套 Meta 标准库，不维护第二套语义实现。

## 4. 优先级与依赖

```text
P0  M0 基线、M1 Meta.invoke、M3 specialization context
P1  M2 type/bounds/effect 标准库、M4 Lain backend
P2  M5 C++/LLVM foreign fragment、M6 cache/并行化
P3  M7 发布打包、IDE/LSP 深度集成和生态工具
```

依赖关系：

```text
M0 → M1 → M2
          ↘
           M3 → M4 → M7
                ↘
                 M5
M3 → M6
```

不要在 M1 完成前扩大高级语法范围，也不要在 M3 完成前用大量 emitter 特判
解决新的泛型或类型问题。这样会把临时修复固化成第二套 compiler core。

## 5. 每阶段通用验收规则

- 新能力必须有最小 fixture 和失败 fixture。
- Meta 语义错误必须带 source span；LAIN-IR 错误必须能由 verifier 重现。
- 每次改变 emitter、specialization 或 host capability 后，先跑 M1/M2，再跑
  archive-usable。
- 生成 artifact 必须可保存、可重复验证、可脱离源码运行。
- 性能改动必须同时报告耗时、内存、cache 命中率和 artifact 是否变化。

## 6. 当前下一项

下一轮只做 M0/M1：

1. 重新跑通 `run_all.py`，把 M1/M2 和 archive-usable 固定为 CI gate。
2. 为 `MetaProgram`/`MetaEnvironment` 建立最小结构化表示。
3. 把 archive `Meta.invoke` 的 procedure id 接到真实 program/environment。
4. 增加一个返回 AST 节点的 Meta procedure fixture。
5. 在不改变 LAIN-IR verifier 边界的前提下，移除 identity-only 特例。

完成这五项后，再开始 M2 的标准类型和策略库迁移。

## 7. 执行方式

### 7.1 状态标记

每个工作包使用以下状态之一：

```text
[ ] 未开始
[~] 实施中
[x] 已完成并通过验收
[!] 被阻塞，需要设计决策或外部能力
```

“代码已经存在”不等于工作包完成。只有代码、最小 fixture、失败 fixture、
回归命令和文档都具备，才可以标记为 `[x]`。

### 7.2 提交粒度

一个提交只解决一个可验证问题。推荐提交顺序：

```text
接口/数据结构
    → 最小正例
    → 最小负例
    → archive 集成
    → 固定点回归
    → 性能记录
```

任何改变 `src/lainc/lainc.lain`、`src/lainir/lainc.l1` 或 physical lowering
的提交，都必须同时记录：

- gen1、gen2、gen3 是否成功；
- artifact 是否 verifier-valid；
- `run_lainc_m1.py`、`run_lainc_m2.py` 和 archive-usable 是否通过；
- 该提交前后的编译耗时和产物大小。

## 8. 核心接口契约

### 8.1 Meta value 契约

Meta value 必须保留语义身份和来源，不应只用一个整数表示：

```text
MetaValue {
    kind: Type | Module | Callable | Constant | Syntax | Foreign | Diagnostic
    identity: stable interned id
    owner: module/environment id
    source_span: source id + start + length
    phase: runtime | comptime | syntax | backend
}
```

物理地址、LAIN-IR 临时编号和 Meta identity 不得混用。需要转换时必须经过
显式的 lowering 函数。

### 8.2 Callable 契约

所有可调用对象统一保存：

```text
Callable {
    name
    phase
    parameter descriptors
    result descriptor
    body or foreign capability
    closure environment
    specialization key
}
```

调用流程固定为：

```text
resolve callee
    → check phase
    → specialize arguments
    → create environment/frame
    → evaluate or lower body
    → return MetaValue / physical value / diagnostic
```

禁止在 emitter 中直接根据名字猜测 callable 的 phase 或物理类型。

### 8.3 Lowering 契约

Meta lowering 必须明确区分三种结果：

```text
Meta-only       只产生 Meta value，不进入产品 artifact
Physical        产生 LAIN-IR 函数、数据或类型布局
Foreign         产生需要宿主编译/链接的 opaque artifact
```

每个 lowering 结果都应带有：

```text
result kind
physical type/ABI shape
dependencies
diagnostics
cache key
```

## 9. 工作包清单

下面的编号是可直接转换成 issue 或提交任务的最小单元。

### M0 工作包：基线和回归

#### M0.1 修复统一回归入口

- [x] 检查 `tests/lainir_lain/run_all.py` 的执行顺序和失败传播；module
  consteval fixture 已修复，整个入口返回 0。
- [x] 修复 M1 测试对 allocator extern 声明的期望。
- [x] 修复失败路径中缺失的 Python `sys` 导入。
- [x] 将 archive-usable 作为固定点之后的强制 gate。

验收：`python tests/lainir_lain/run_all.py` 返回 0，输出中包含所有阶段名。
当前状态：M1、M2、archive-usable、archive factory chain 和 module consteval
gate 均通过；`run_all.py` 返回 0，本工作包完成。

#### M0.2 固定 artifact 规范

- [x] 定义 extern 声明排序规则。
- [x] 定义 procedure、label 和 module prefix 的 canonical 规则。
- [x] 定义换行符、末尾换行和 formatter 规则。
- [x] 编写 artifact canonicalizer，比较前先 canonicalize。

验收：同一输入在两次独立执行中得到相同 canonical artifact。

#### M0.3 宿主 capability 分层

- [x] `lainir-seed run` 只内建 allocator capability。
- [x] source、artifact、diagnostic I/O 只由 bootstrap 命令注册。
- [x] 为 capability 缺失、参数错误和分配失败分别写负例。
- [x] 明确外部内存的所有权和释放策略。

验收：archive product 可以在 `run` 下返回 0；普通无 extern 程序不受影响。

### M1 工作包：Meta.invoke

#### M1.1 结构化 program handle

- [x] 在 `src/compiler-archive/meta.lain` 定义 program descriptor。
- [x] descriptor 已包含 program/unit/source/procedure-count 及有界 procedure
  table；每项指向独立 `MetaProcedure` descriptor。module 表句柄仍待接通。
- [x] procedure id 在 descriptor 内独立保存，不依赖物理函数序号。
- [~] 已添加 program 创建、登记和 procedure 查询接口（含 id/source span/参数数）；
  销毁接口待环境所有权完成后补齐。

验收：可以通过 `(program, procedure_id)` 找到 body、参数和 phase。

#### M1.2 环境和 frame

- [x] 实现参数 frame 与局部 binding 表。
- [x] 支持 caller environment 的只读捕获。
- [~] 定义 shadowing 规则和重复绑定诊断（当前只完成 frame 结构，名称解析待接入）。
- [~] 定义 address、Meta value、unit 和 scalar 的 frame 表示（当前已覆盖 scalar，
  address/Meta value/unit 的统一 ABI 待补齐）。

验收：一个 Meta procedure 能读取参数、建立局部 `let`、返回值并访问捕获常量。

#### M1.3 phase boundary

- [x] runtime callable 不得被 comptime evaluator 隐式调用。
- [x] syntax callable 只能在拥有 syntax environment 时执行。
- [x] backend callable 只能在 backend context 中执行；`Context.phase` 使用显式
  backend 标记，`invoke_backend_program` / `invoke_backend_facts` 在 ABI 层拒绝
  缺失上下文（2911），并通过 facts bridge 执行最小 backend procedure。
- [x] 缺失环境、phase 不匹配、外部 capability 缺失分别返回稳定诊断码。

验收：每种非法调用都有负例，且不会生成部分 artifact。

#### M1.4 AST-producing procedure

- [x] 增加返回一个最小 AST 节点的 fixture。
- [x] 增加返回节点列表和带 source span 节点的 fixture。
- [~] `Meta.expand` 已调用 registry-backed syntax invocation bridge 并返回可验证
  节点；当前 bridge 会校验 callable 的 phase、procedure 和 program，再使用
  hygienic clone fallback 生成 Expansion。标量 facts dispatch 已接通并有正负例；
  真实 syntax procedure evaluator、稳定名称解析和任意 AST 构造仍待补齐。
- [ ] 删除 identity-only 路径，或把它降级为普通测试 fixture。

验收：archive 的 `Meta.expand` 能返回真实 syntax value，并可继续进入 lowering。

### M2 工作包：标准 Meta 语义库

#### M2.1 `std/diagnostic`

- [x] 定义 error、warning、note、help 四类诊断及稳定的严重级别 ABI。
- [x] 统一 source span、主标签、次标签和关联 note 的 record shape。
- [x] 以无宿主依赖的纯 Meta module 供 evaluator、domain parser 和 backend 共用。
- [x] 提供 `DiagnosticSet` 错误聚合计数，避免第一个错误后丢失后续独立错误。

验收：标准值模型已落地并通过 `std_diagnostic` 编译/运行 fixture；类型错误、
phase 错误和 lowering 错误的 formatter/source-stage 映射仍待接入。

#### M2.2 `std/type`

- [x] 定义 nominal、structural、alias、function、opaque、foreign type value。
- [x] 定义 representation shape 与语义类型的映射。
- [x] 实现 strict/permissive 显式转换和隐式转换策略接口。
- [x] 实现最小 overload candidate rank 集合和冲突判定。

验收：`std/type.lain` 已提供 strict/permissive 两套策略，并由同一 fixture
验证不同转换结果、representation mismatch 和 overload 冲突；nominal identity
及完整诊断 formatter 仍待接入。

#### M2.3 `std/generic`

- [x] 定义 type/value/module/effect 四类参数。
- [x] 定义参数约束、默认参数和参数 shadowing 策略值。
- [x] 将基础实例化请求编码为确定性的 specialization token，供 M3
  `SpecializationKey` 扩展。
- [x] 对递归实例化提供请求成本与剩余预算判定。

验收：`std/generic.lain` 已验证 type/value/policy 改变会产生不同 token，重复
请求得到相同 token，递归预算和 shadowing 有正负例；真实 `Vec` 实例化缓存仍待
接入 M3 context。

#### M2.4 `std/bounds`

- [x] 定义 checked、verified、unchecked 三种策略值。
- [x] 定义 range fact 的基本产生形状（lower/upper/valid）。
- [x] checked 策略生成 runtime branch 结果码。
- [x] verified 策略在证明缺失或越界时返回失败判定。
- [x] unchecked 策略明确返回 trust assumption。

验收：`run_std_bounds.py` 验证三种策略都生成 verifier-valid IR，checked/verified
失败行为和 unchecked trust 均有运行时负例；range fact 的跨 procedure 传播、失效
诊断仍待接入真实 Vec/lowering 路径。

#### M2.5 `std/effect` 与 `std/ownership`

- [x] effect 集合支持最小声明、传播、处理和 capability 映射值。
- [x] ownership policy 支持 borrow、move、alias 的最小状态机。
- [x] 先以诊断/Meta 约束形式实现策略，再留给优化器消费事实。
- [x] effect/ownership 名称未加入 LAIN-IR core opcode。

验收：`run_std_effect_ownership.py` 验证 effect allow/propagate、capability id
以及 borrow/move/alias 转移的正负路径；上下文级诊断接线和优化器消费仍待补齐。

### M3 工作包：specialization 与物理 lowering

#### M3.1 canonical specialization key

- [x] 代码身份使用稳定 source/module identity，不使用进程地址。
- [x] 参数按类型、值、模块、effect policy 分类编码。
- [x] 环境 fingerprint、target/backend 版本进入 key seed。
- [x] key seed 使用纯整数编码，可序列化并跨进程复算。
- [~] `tests/core/self_hosting/artifact_cache.py` 已提供带 stamp/hash 信任边界的磁盘
  specialization cache，并通过跨进程 key、target 失效和篡改负例；编译器内部
  specialization context 尚未直接调用该 cache。

验收：`std/generic.lain` 的 `specialization_key_token` 已验证同一输入得到相同
key，改变 target 会失效；磁盘 cache 和编译器 context 接线仍待完成。

#### M3.2 context threading

- [~] 新增扁平的 `specialization_context`（layout/types/meta/consts +
  source/target/backend identity + deterministic key），并接入
  `compile_source` 和 `eval_module_factory`；当前由
  `emit_function2_context` 适配旧 emitter，`emit_function2` 的全量签名迁移仍待完成。
- [ ] `emit_operand`、`emit_type`、record layout 和 call lowering 从 context 查询。
- [ ] 删除基于字段名、函数名和固定偏移的泛型猜测。
- [ ] 为每次物理决策保留 Meta source span，供诊断使用。

当前验收：冻结 compiler 可生成并通过 `lainir-print` 检查，产物中可见
`specialization_context_new`、`emit_function2_context` 及其真实调用；完整验收
仍要求泛型参数影响函数签名、字段宽度、call operand 和返回值宽度。

#### M3.3 module/closure identity

- [~] specialization context 已保存稳定 source identity/key；模块成员的完整 identity
  与物理 label 分离仍待把 meta row owner 改为显式 module id。
- [ ] closure environment 显式记录捕获值和捕获类型。
- [ ] 相同成员名在不同模块中只能通过正确 owner 解析。
- [~] context 已加入 materialization state（1=constructing、2=complete、3=failed），
  工厂成功路径和 SourceWorkspace 快速路径会写入 complete；失败态和递归构造保护仍待
  接入所有返回分支。

验收：nested namespace、重复成员名、闭包捕获和递归 import 均有正负例。

#### M3.4 structured unit builder

- [~] 为关键 LAIN-IR 构造提供结构化 builder API；`l1_unit_builder.lain` 已覆盖
  literal、parameter、binary、call、argument、return、if、procedure 和 block，
  并统一拒绝越界 id、非法类型和不匹配的调用参数。
- [ ] 文本 emitter 只作为兼容输出层。
- [~] builder 已统一检查表达式类型、procedure 返回类型、调用参数类型、block id
  和 condition 的 `#bits<1>` 类型；archive native 路径已补齐 `Model_*` 物理操作和
  `append_return_typed`，return 的 owner-return-type 关联仍待补进 L1 model。
- [ ] 文本输出与 structured builder 输出通过 canonicalizer 比较。

`l1_verifier.lain` 已同步检查 binary/call 类型、call arity、return 类型和 bool
branch condition；这保证手写 Unit 与 Builder 生成的 Unit 共享同一结构校验边界。

验收：至少一个 archive 模块可完全走 structured builder，并通过同一 verifier。

### M4 工作包：Lain-written backend

#### M4.1 backend input contract

- [~] 新增 `std/backend.lain`，定义 `Module`、`Function`、`Extern`、`Target`、
  `DataLayout` 及 target/calling-convention/endian 枚举；data facts 和完整 target
  fingerprint 仍待扩展。
- [~] `validate_target`、`validate_extern` 和 `diagnostic_severity` 提供稳定错误码，
  可映射到 `std/diagnostic`；当前尚未直接返回 Diagnostic record。
- [x] calling convention、pointer width/alignment、endian 的 target interface 已有
  明确字段和正负校验。
- [~] capability 需求由 `capability_required` 给出；backend 的文件/linker/process
  capability 隔离仍待 M4.2/M4.3 接线。

验收：`run_std_backend.py` 验证未知 target、非法 ABI shape、未解析 extern 的拒绝路径，
以及合法 LainIR target 的通过路径；完整 Diagnostic record 映射仍待完成。

#### M4.2 第一阶段输出

- [~] `scripts/run_lain_backend.py` 继续输出 C bridge，并在 backend 前强制运行
  `lainir-print`；结构化 Unit 输入仍待接线。
- [~] 已覆盖函数、按声明宽度发射的局部绑定、条件分支和外部调用的可编译子集；
  `tests/core/backend_manifest/backend_full.l1` 由 `run_lain_backend.py` 生成 C，
  输出用户 extern 原型和调用，并通过 `zig cc -std=c11 -O2 -c`；fixture 还验证
  `#bits<32>` 局部绑定发射为 `uint32_t`。记录/常量以及结构化 Unit 输入仍待接线。
- [x] `scripts/backend_manifest.py` 生成 deterministic link manifest，记录外部符号、
  参数/返回签名、capability 和输入源 hash。
- [x] verifier 已成为 `run_lain_backend.py` 的强制前置；非法 L1 在 C 生成前失败。

验收：`run_backend_manifest.py` 验证 manifest 的 capability 分类与确定性，并覆盖
函数/局部/分支/extern 的真实 archive→C→C11 object 路径；完整用户 fixture 的
archive→C→executable 端到端仍待补齐。

#### M4.3 native binary

- [~] 当前实现 C object source emission；`build_lainc_native.py` 先校验 compiler
  artifact，再用 Lain-written backend 生成 C。
- [~] 已有 `zig cc` link step 和 `native_lainc.c` driver；target-specific flags、
  object cache 仍待完善。native driver 现在支持 `--run`，在同一进程内用内置
  parser/verifier/interpreter 执行刚生成的 L1。
- [~] 文件读取、artifact 输出和分配由 native driver 的宿主边界提供；linker/临时
  目录尚未抽象成独立 capability。
- [x] `-std=c11`、`-O2`、`LAIN_NATIVE_LIBRARY_ENTRY` 及 Zig cache 目录已固定记录在
  `scripts/build_lainc_native.py`。

验收：`build/laincexe --run` 可以脱离外部 `lainir-seed run` 编译、校验并运行用户
程序；`tests/core/native_binary/run_native_inprocess_smoke.py` 覆盖该路径。

### M5 工作包：C++/LLVM foreign fragment

#### M5.1 foreign type model

- [ ] 保存 C++ qualified name、template arguments、value arguments 和 overload set。
- [ ] foreign type 只在 Meta 中解释，运行时使用 opaque handle。
- [ ] 记录 Clang diagnostic 的源映射。
- [ ] 禁止把 foreign type 擦除为无结构的整数 tag 后再恢复。

#### M5.2 fragment compiler

- [ ] 以 Meta module 或 fragment 为编译单位。
- [ ] 支持 `cpp::type`、`cpp::instantiate`、`cpp::construct`、`cpp::call`。
- [ ] 支持一次性生成 C++ translation unit。
- [ ] 支持 Clang/LLVM artifact 的缓存和版本校验。

验收：`llvm::cast<T>`、`IRBuilder<>`、`SmallVector<T, N>` 各有一个端到端测试。

### M6 工作包：性能与增量编译

#### M6.1 trace schema

每次编译输出一份 JSON 或等价结构化 profile，至少包含：

```text
run id
source list and hashes
phase name
start/end/duration
input/output size
Meta call count
specialization count
cache hit/miss
peak memory if available
diagnostic count
```

- [x] `scripts/profile_lainc_bootstrap.py` 为每个阶段落盘 stdout/stderr、耗时、输出
  大小、峰值 RSS/VMS，并写出 `profile.json` 与 `analysis.md`。
- [x] `LAINIR_TRACE_PROFILE` 的 procedure TSV 会被聚合为总调用数、总 steps、Meta
  调用数和 specialization/factory 调用数，同时记录 top procedures。
- [x] seed trace 直接输出 `cache` rows，记录 Meta lookup hit/miss；profile JSON
  保留计数和 `instrumented` 标志。未启用优化 cache 的运行会明确记录为 0，
  不把“未走 cache”伪装成 cache 命中。diagnostic count 继续从 bootstrap summary
  解析，缺失时保持 `null`。

#### M6.2 优化顺序

按以下顺序实施，避免过早并行化掩盖根因：

1. 消除重复 source scan 和重复 brace matching；
2. 缓存 Meta lookup、consteval 和 layout；
3. 缓存 specialization 和 structured unit；
4. 建立 source dependency graph；
5. 只对无共享可变 Meta 状态的 source unit 并行化。

- [~] scratch buffer 已改为按需增长，避免小缓冲越界和固定 4 KiB 浪费；bootstrap
  小分配已改为带对象释放头的 arena，解释器常见 call/primitive 参数改用栈缓存；source scan/Meta
  lookup/layout 的跨编译缓存仍未完成。
- [~] `build_lain_compiler.py` 与 `build_lain_frontend.py` 已加入输入源码、bundler
  版本和输出 hash 的持久 stamp；命中后仍运行 `lainir-print`，损坏或输入变化会回退
  重建。编译器内部 source scan/Meta lookup/layout cache 仍未接线。
- [~] consteval 的临时参数表现在通过 `bootstrap.release-pages` 在返回路径回收，
  native 编译期页也有独立 owner；完整 archive 的峰值已从约 449 MiB 降至约
  127 MiB。`emit_function2` 的 per-function type table 改为惰性分配，module
  type table 在模块发射完成后回收；模块工厂的 eager/non-eager 早退路径也会释放
  临时表，避免每个 archive 模块累积一页；同一 API archive fixture 的 seed logical
  allocation 从约 451 MiB 降至约 320 MiB（尚未重新测量最新早退修复后的数值）。
  native 完整 archive 输入的内存 owner 已能在 artifact flush 后统一释放；此前 O2
  入口的 access violation 已定位为完整 C 原型生成中的 `append_span` 结束位置下溢，
  修复后 O2 可完成整套 std+archive 输入，empty/nonempty API product 均通过 verifier
  并分别返回 0/1。剩余 gate 转为 archive 自编译、缓存命中和干净目录重建。

验收：第二次无修改编译显示 cache hit；修改单个模块不会重新执行整个 archive。

#### M6.3 性能预算

每个阶段都记录基线和目标，不以单次总耗时作为唯一指标：

```text
parse/source scan
Meta binding
factory evaluation
specialization
physical lowering
emission
verification
host I/O
```

当某阶段超过总耗时 50% 时，下一性能任务必须优先处理该阶段。

- [x] `scripts/profile_lainc_bootstrap.py` 将 50% 阈值、主导阶段和告警写入
  `profile.json`，并在 `analysis.md` 给出下一任务建议；`run_trace_contract.py`
  覆盖告警落盘。

### M7 工作包：发布级自举

#### M7.1 固定点证明

- [x] `tests/lainir_lain/run_lainc_m2.py` 生成并检查 gen1、gen2、gen3。
- [x] `scripts/prove_lainc_fixed_point.py` 对 artifact 做 canonicalize 后比较。
- [x] proof report 比较 procedure label、extern、签名和函数体 SHA-256 摘要。
- [x] 差异按 canonical text、extern、label、header、body 分类输出；非 formatter
  差异直接使命令返回失败。

#### M7.2 bootstrap 缩减

- [x] `seed/src/host/bootstrap.c` 的 capability 注册已完整列出：
  `source-count/source-length/source-data/source-path-data/source-path-length`（host
  source 输入，compile phase）、`copy-bytes`（backend/source bridge，compile phase）、
  `allocate-pages/release-pages`（compiler scratch，compile/runtime bridge）、
  `write-artifact`（legacy artifact sink，emit phase）、`write-diagnostic`（失败报告，
  diagnostic phase）、`artifact-begin`、`artifact-write-byte`、`artifact-write-span`、
  `artifact-write-literal`、`artifact-finish`（artifact sink，emit phase）。
- [x] 每个 capability 已在上表标注用途、当前调用方类别和 phase；注册入口集中在
  `bootstrap_run_cli`，便于后续做最小集合审计。
- [ ] 删除高级语义相关的 seed 特判。
- [~] 当前注册集合仅包含 source 输入、内存、artifact/diagnostic I/O 和 byte-copy；
  高级 Meta 语义仍在 Lain 层，完整最小集合裁剪和特判删除待后续验证。

#### M7.3 发布 gate

- [ ] archive lainc 编译自身。
- [ ] archive lainc 编译标准 Meta 库。
- [ ] archive lainc 编译真实用户程序。
- [ ] LSP、CLI、测试 runner 使用同一 Meta package 版本。
- [ ] 从干净目录可重建 bootstrap artifact。

## 10. 测试矩阵

### 单元和负例

| 能力 | 正例 | 负例 | 必须验证 |
|---|---|---|---|
| Meta value | interning identity | 错误 owner | identity 稳定 |
| callable phase | comptime 调用 | runtime 调 comptime | phase 诊断 |
| environment | 参数/捕获 | 未绑定名 | frame 隔离 |
| type policy | strict/permissive | overload 冲突 | Meta diagnostic |
| bounds | checked/verified | proof 失败 | policy 不进 core |
| generic | `Vec(i32)` | 缺失参数 | key 稳定 |
| module | 同名成员隔离 | 错 owner | namespace 解析 |
| lowering | 合法 IR | 宽度不匹配 | verifier 拒绝 |
| foreign | typed fragment | Clang 错误 | source mapping |

### 集成 gate

```text
python tests/lainir_lain/run_meta_module.py
python tests/lainir_lain/run_meta_record.py
python tests/lainir_lain/run_meta_eval.py
python tests/lainir_lain/run_compiler_api_bootstrap.py
python tests/lainir_lain/run_type_namespace_nested.py
python tests/lainir_lain/run_lainc_m1.py
python tests/lainir_lain/run_lainc_m2.py
python tests/lainir_lain/run_lainc_archive_usable.py
python tests/lainir_lain/run_all.py
```

每个新标准 Meta package 还必须有：

```text
独立 source fixture
archive 集成 fixture
至少一个失败 fixture
verifier 检查
运行时检查（如果产生 runtime code）
```

## 11. 风险登记

| 风险 | 触发信号 | 处理方式 |
|---|---|---|
| fixed point 分叉 | gen2 != gen3 | 回退到最小 emitter 改动，补外围 lowering |
| Meta 无限展开 | step budget 触发 | 输出 specialization stack，检查递归 key |
| 语义偷渡进 core | verifier 出现高层类型/策略判断 | 移回 Meta policy，并保留 representation 检查 |
| 文本 emitter 漂移 | structured/text artifact 不一致 | 先 canonicalize，再修 builder 或 emitter |
| cache 误命中 | 不同环境得到同一 key | 扩充 environment fingerprint，删除旧缓存 |
| capability 泄漏 | Meta 随意访问文件/进程 | phase + capability table 双重检查 |
| 性能回退 | 总耗时或 Meta call 数上升 | 用 profile 找热点，禁止凭感觉优化 |
| archive 假可用 | verifier 通过但真实程序失败 | 增加非空、非平凡用户源验收 |

## 12. Definition of Done

一个阶段只有同时满足以下条件才算完成：

1. 设计边界写入文档；
2. 实现位于正确层（Meta、IR core 或 host capability）；
3. 至少一个正例和一个负例；
4. archive 集成成功；
5. verifier 和 runtime gate 通过；
6. 固定点没有回归；
7. 生成 profile 或说明该阶段为何不需要 profile；
8. 已知限制写入文档；
9. 可从干净工作区重现结果；
10. 后续阶段的接口依赖已明确。

## 13. 接下来十个可执行任务

按当前优先级，下一轮按以下顺序开工：

```text
T01 重新跑通 run_all.py，并固定当前 M0 基线
T02 为 seed run allocator capability 增加独立正负例
T03 定义 MetaProgram/MetaEnvironment/MetaProcedure 最小记录
T04 添加 Meta procedure id → program/body 的查询接口
T05 添加参数 frame、捕获环境和 shadowing 负例
T06 将 Meta.expand 接到真实 program/environment
T07 增加返回 AST 节点的 syntax procedure fixture
T08 统一 Meta.invoke 的 phase 和 capability 诊断
T09 为 Meta 调用生成 specialization trace
T10 用真实 AST fixture 替换 identity-only 验收
```

T01–T02 属于 M0，T03–T10 属于 M1。完成 T10 前，不扩展新的高级语法；完成
M1 后，才开始 `std/type` 和 `std/bounds` 的策略接口设计。

## 14. 当前验证快照（2026-08-30）

- seed allocator 的显式 `allocate-pages/release-pages` 配对已通过独立正负例；
  最新 profiler 的 bootstrap 峰值约 50.3 MiB RSS / 47.8 MiB VMS，未再出现
  持续增长到 GiB 的现象。Windows 上约 4.1 GiB 的虚拟地址空间是进程基线预留，
  不能当作已提交的物理内存。最新 `run_lainc_archive_usable.py` 带
  `LAINIR_TRACE_ALLOC=1` 的实跑峰值约 112 MiB RSS；该 seed 进程结束后下一阶段
  进程回落到约 10 MiB，未观察到跨阶段累积。
- `run_lainc_m1.py` 通过；M3.2/M3.3 context 适配后的 gen1/gen2/gen3 均通过
  `lainir-print`，canonicalizer 已改为忽略字符串字面量中的结构性大括号。
  最新 gen2 在加入 `Sources.new()` 构造器显式 lowering 后仍通过 verifier；
  `run_lainc_m2.py` 已重新确认 gen2 == gen3，固定点 artifact 为 436883 字节。
  `emit_function2_context` 已覆盖顶层 compile_source
  和工厂 helper 发射，context 还包含工厂 materialization state；完整 lowering 子路径
  迁移仍待继续。
- `sb_new_small` 已改为带 sentinel/容量元数据的可增长 scratch buffer：初始数据区
  512 字节，写满后倍增复制；gen1/gen2/gen3 均通过 verifier，避免了固定 4 KiB
  分配和长表达式越界。旧的 512 字节裸缓冲方案会产生非确定性 verifier 错误，已移除。
- `run_lainc_archive_usable.py` 重新通过，`src/lainc` 生成的真实 archive
  `api.compile(empty)` 与 `api.compile(nonempty)` product 均通过 verifier，运行结果
  分别为 0 和 1。
- `run_meta_invoke_environment.py` 已通过 L1 verifier 和运行时 gate；默认 fixture
  与 arithmetic syntax fixture 均通过。fixture 已覆盖返回节点链、列表合并以及
  source span 保留；`Syntax.parse` 的 root/layout、trivia 跳过、Expansion/SyntaxList
  物理布局已固定。当前 archive 产物可以执行受限的真实 scalar syntax procedure，
  但仍不能称为支持任意用户 syntax procedure 的完整 parser。
- backend phase 负例与正例已加入 `meta_invoke_environment.lain`：无 backend
  context 稳定返回 2911，有 backend context 可执行常量 procedure 并返回 5；
  `MetaProgram` 的 64 项 procedure table、按 id 查询和第二项 descriptor 的
  facts dispatch 已通过正负例，完整 generic registry evaluator 仍待接通。
- `MetaProgram` 当前为 64 项有界 procedure 指针表；fixture 已登记两个 procedure，
  并通过显式 ABI getter 验证第二项的 id、source_start 和 parameter_count。
- `std/diagnostic.lain` 已提供四级 severity、Span/Label、Diagnostic、
  DiagnosticSet 和聚合 API；`run_std_diagnostic.py` 验证 severity、span width、
  code 分类和聚合路径，archive product verifier 与运行结果均为 42。
- `std/type.lain` 已提供六类语义 type kind、五类 representation shape、
  strict/permissive conversion 和 overload rank；`run_std_type.py` 验证正负路径，
  archive product verifier 与运行结果均为 42。
- `std/generic.lain` 已提供四类参数、约束/默认/shadowing 策略、确定性
  specialization token 和递归预算；`run_std_generic.py` 验证不同 type/policy
  请求分离、重复请求命中和预算负例，archive product verifier 与运行结果均为 42。
- `std/bounds.lain` 已提供 checked/verified/unchecked 策略值、RangeFact 形状、
  checked/verified 结果和 unchecked trust；`run_std_bounds.py` 验证三种策略及越界
  负例，archive product verifier 与运行结果均为 42。
- `std/effect.lain` 已提供最小 effect propagation/capability 策略，新增的
  `std/ownership.lain` 提供 borrow/move/alias 状态机；`run_std_effect_ownership.py`
  验证允许/禁止 effect 和 ownership 转移负例，archive product verifier 与运行结果
  均为 42。
- archive `types.ModuleValue` 已与 `std/meta` 的模块值形状对齐，补齐
  `module_payload` 字段及模块解析构造路径；`run_pure_lain_contract.py` 已重新通过。
- `std/generic.lain` 的 specialization key 已扩展为包含 environment/target/backend
  的确定性 token；`run_std_generic.py` 增加重复 key 与 target 变化负例，仍待接入
  M3 磁盘缓存。
- profiler 报告位于 `build/profiles/lainc-20260830-081219/`：该快照记录 gen1
  206.405 秒，峰值 RSS 50.3 MiB、VMS 47.8 MiB；gen2/gen3 分别为 10.1/10.4 MiB
  RSS，固定点比较通过。随后 phase 标量 lowering 修复使最新 fixed-point artifact
  更新为 436883 字节；热点 procedure trace 仍待重新采集。profile 现在会根据
  50% 阈值直接标出主导阶段，后续 M6.2 应优先减少重复扫描和 lookup。
- `tests/core/profiling/run_trace_contract.py` 已验证 seed trace 的 `cache` rows
  能稳定解析为 profile JSON 的 hit/miss/instrumented 字段；`zig build`、Python
  语法检查和此前完整 `run_all.py` 固定点回归均通过。未启用 gen2 cache 的完整
  回归仍可能在模块夹具的 300 秒单步预算内超时，这属于当前性能边界，不能当作
  功能或内存安全通过。`run_lainc_module.py` 支持显式
  `LAIN_META_USE_CACHED_GEN2=1`，复用前置 fixed-point 并单独验证四项模块行为。
- specialization cache 已补入 `tests/core/self_hosting/artifact_cache.py`：canonical
  JSON key 包含 source、type/value/policy、environment、target 和 backend，artifact
  stamp 校验输出 hash；`run_artifact_cache.py` 已覆盖跨进程稳定、target 变化失效和
  artifact 篡改拒绝。
- build artifact cache 契约由 `tests/core/build_cache/run_build_cache_contract.py`
  覆盖，验证有效 stamp 命中和 artifact 篡改拒绝；两套 build script 的实际第二次
  构建已观察到 `cache hit`。
- M7.1 proof 工具 `scripts/prove_lainc_fixed_point.py` 已生成当前固定点报告：
  canonical artifact 408970 字节、10 个 extern、257 个 procedure，label/signature/
  body 摘要均一致；`tests/core/fixed_point_proof/run_fixed_point_proof.py` 覆盖正负例。
- M7.2 已完成 seed capability 清单和用途/phase 标注；当前 seed 没有高级语义解释器，
  但 capability 裁剪仍需从干净 bootstrap 和各调用方覆盖率继续验证。
- M4.1 已加入 `std/backend.lain` 的 target/ABI/extern contract，
  `run_std_backend.py` 已验证合法 LainIR target 和未知 target/未解析 extern 负例。
- M4.2 已加入 `scripts/backend_manifest.py`；`run_lain_backend.py` 现在先执行
  `lainir-print`，再生成 C，并落盘 link/capability manifest。
- M4.2 真实 smoke 已用 `build/backend_fixture.l1` 跑通，产出 C、通过 `zig cc -c`
  编译并落盘 `*.manifest.json`；manifest 的 source 路径统一为 `/` 分隔，便于跨平台复现。
- M3.4 已把 archive 的 L1 structured builder 改为带校验的构造层：表达式类型、
  call 签名、参数数量、return 类型、region/procedure id 均在写入 Unit 前检查；静态 contract
  回归为 `tests/core/structured_builder/run_structured_builder_contract.py`；condition
  现在也强制要求 bits<1>。文本/结构化双路径的 canonical parity 和 return 的
  owner-return-type 关联仍未完成。
- M4.3 native driver 已具备可重复的 C 编译入口：`build_lainc_native.py` 会先用
  `lainir-print` 验证 compiler artifact，再以固定 C11/O2 参数链接；当前 stage1
  可生成并运行 CRLF 用户源（`tests/core/native_binary/run_native_smoke.py`），
  `--run` 通过内置 parser/verifier/interpreter 在同一进程执行用户 L1，回归覆盖于
  `tests/core/native_binary/run_native_inprocess_smoke.py`。
- seed bootstrap 的小分配已使用分块 arena，解释器常见 call/primitive 参数使用栈内联
  缓存；allocator 回归和 native in-process smoke 均通过。
- native host 的编译期 `bootstrap.allocate-pages` 已加入独立 allocation owner，并在
  artifact flush 后释放；历史完整 archive 采样从约 449 MiB 降至约 127 MiB。最新
  自举链直接采样约 49 MiB；完整 std+archive 输入在 O2 下已成功生成 136 KiB 左右
  product，经过 `lainir-print` verifier 并在 seed interpreter 中返回 0/1。原先的
  access violation 根因是 C 原型生成的 `append_span` 下溢，已修复；native archive
  发布 gate 仍等待 archive 自编译和干净目录重建。
- native `lainc --run` 已尝试整套 std + archive 输入；`sb_append_literal` 现在按
  NUL 终止字面量计算真实长度，已消除 `next#let`、`##proc` 这类越界拼接产生的
  语法错误；Builder alias 的 `Model_*` 目标也已物化，完整 archive 产物可通过
  verifier。嵌套 factory specialization 产生的重复 `Model_*` helper 已通过输出
  sentinel 和成员过滤去重；`compiler_api_compile_empty.lain` 与
  `compiler_api_compile_nonempty.lain` 均已由 gen2 编译、通过 verifier，并在 seed
  解释器中分别返回 0/1。非空路径此前因 `Sources.new()` alias 丢失而生成空地址，现已
  在 `emit_expr` 中显式降到 `f0_SourceWorkspace_new()`；M7.3 的 archive lainc
  自编译、标准 Meta 库、真实用户程序、统一 package 版本和干净目录重建仍未全部
  验收，发布 gate 继续保持未勾选。
- 最新 O2 native binary 已直接编译整套 std+archive 与 empty/nonempty API client：
  产物约 136 KiB，`lainir-print` 验证通过，seed interpreter 运行结果为 0/1；
  这证明 native backend 的 archive 输入路径已稳定，但还不等同于 archive lainc
  自举后的独立发布 binary。
- backend 已修复跨行深层 L1 表达式的逻辑指令合并；这消除了 archive native C
  生成中的断括号错误，原生 C 编译 smoke 已覆盖该路径。
