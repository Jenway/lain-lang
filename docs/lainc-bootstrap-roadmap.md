# Lainc bootstrap roadmap

> 计划快照：2026-08-16

> **历史快照。** 当前执行路线以
> [`docs/lainc-meta-roadmap.md`](lainc-meta-roadmap.md) 为准。新路线将高级
> 语言规则放入可替换的标准库：第一代标准库用 LAIN-IR 编写，正式标准库用
> Lain 编写；Meta 只操作 AST，编译期执行统一通过 LAIN-IR `#eval`。

> 这是当前长期计划的本地序列化文件。后续进度以这里的勾选项和“下一项实现任务”为准。

这份文档是当前路线的本地序列化版本。阶段勾选项记录已经完成的工作，
“下一项实现任务”记录下一轮开发入口。

## 目标

最终用 C 写的 `lainir-seed` 执行 LAIN-IR 编译器，得到可编译完整
`src/compiler-archive` 的 Lain compiler，并完成自举：

```text
C lainir-seed
    -> src/lainir/lainc.l1
    -> stage1 lainc
    -> stage1 编译 src/compiler-archive
    -> stage2 lainc
    -> stage2 再编译自身
```

`lainir-seed` 只提供文件、内存、诊断和 artifact I/O。Lain 的语义、Meta、
类型和编译期执行都由 LAIN-IR/Lain compiler 实现。

## 当前基线

已经具备：

- LAIN-IR parser、verifier、interpreter 和 C emitter。
- LAIN-IR 编译器的 stage1 -> stage2 -> stage3 自举收敛（字节一致）。
- Lain RawAst、多文件 workspace、import 图和基础诊断。
- record、module、部分类型环境和有限的 `std::consteval`。
- 函数、调用、整数/布尔表达式、局部变量、`if` 和 `while` 的部分 lowering。
- LAIN-IR formatter、语法高亮、dumb parser 和 LSP 基础协议处理。
- `src/lainir/lainc.l1`（已从当前源码重新 freeze），可编译最小
  Lain 子集，并把完整 `src/compiler-archive` 闭包降低成 verifier-valid LAIN-IR。
- 完整 compiler-API Meta 实例化：`compiler_api_schema.lain` + 全部
  `src/compiler-archive` + std 源 → `compiler_compile` 成功 → `lainir-print` 通过 →
  `lainir-seed run` 返回 `1`（`tests/lainir_lain/run_compiler_api_bootstrap.py`）。
- `Meta.expand` 调用链的 verifier 错误已修复：完整 API artifact 的
  `lainir-print` 已通过（含此前卡住的 `Modules.declare` nominal 参数物理
  类型检查）。
- 嵌套 type namespace 回归通过；record literal 表达式内联、模块缓存
  "构造中/已完成"状态均已实现。

还没有：

- 空输入 fixture 的 `lainir-seed run` 验收（当前卡点）：`api.compile` 对空输入
  生成的 LAIN-IR 在解释器中解引用空地址崩溃（最新代码下为
  access violation `0xC0000005`）；fixture `compiler_api_compile_empty.lain`
  已就位但验收脚本未写。
- 用真实 Lain 源码字节串作为输入的编译验收。
- 完整 Meta 值、闭包环境和模块成员解析（M9 后续项）。
- 完整 `#eval` 编译期执行桥。
- 最终可用的 `lainc`、`stage2 == stage3` 字节级自举比较和完整自举闭环。

已完成的第一步增量：

- 已生成并冻结 `src/lainir/lainc.l1`。
- C `lainir-seed` 可以用它编译 `return_42`、模块工厂和闭包捕获 fixture。
- 它可以把当前完整 `src/compiler-archive` 多文件闭包降低成 verifier-valid 的
  LAIN-IR artifact。
- 这个 artifact 仍然只包含当前前端实际能物化的编译器函数，尚未成为完整
  的 `lainc`。

历史增量（2026-08-14）：

- 模块自引用已经接通。导入模块先登记模块壳，模块体内可以通过自身名字
  访问导出成员；`std/bounds.lain` 和编译器 `types` 的自导入不再递归展开。
- 导入模块缓存改用 Meta value 的 kind 判定，避免把普通堆对象当成 RawAst
  空节点。
- `memory_model.Model(...)`、`source.SourceBuffer(Memory)`、
  `slice.Slice(SourceBuffer, Bounds)` 和 `SourceWorkspace(Memory)` 已有
  独立 bootstrap 回归，能生成 verifier-valid artifact。
- 当 `std/core/vec.lain` 在工作区中时，`vector.Vec(...)` 走真实 Meta
  工厂；工作区没有 Vec 源文件时才使用外部占位值。这样保留了源码闭包
  的轻量路径，同时让完整标准库路径能生成 `ByteVector` 的具体方法。
- 完整源文件集合下，`compiler_probe_core_shapes.lain` 能生成 artifact；
  完整 API 探针现在已经能走过 `workspace.sources.get(...).syntax_unit` 和
  `workspace.syntax.units.get(...).root` 两条真实路径，但在后续
  `Meta.expand` 的其他调用上仍会被 verifier 拦住，暂时不能称为完整
  `src/compiler-archive` 的 verifier-valid 结果。
- 局部工厂函数的物理描述符现在按需挂载；同一个描述符重复进入物理链时
  会被链接标记拦住。小型多文件用户闭包保留顶层未限定 helper 的兼容路径，
  大型 compiler 闭包继续按模块限定名延迟物化。
- 模块缓存已增加“构造中/已完成”状态，并修正了 Meta value 与 RawAst
  空值判断混用的问题。这样可以避免已完成的导入模块被源码收集阶段重复
  展开。
- `compiler_api_schema.lain` 现在已经通过 bootstrap 编译、`lainir-print` 和
  `lainir-seed run` 验收。此前的 `5124` 停止点来自普通 RawAst 遍历结束时先触发
  步数保护；空节点检查顺序已经修正。
- 完整 API 的主要耗时来自工厂函数体里的局部 `std::func`。当前非根工厂的
  局部函数采用延迟物化：验证阶段发现真实运行时调用时再加入物理函数链。
  这让 `Elaborator(Memory)` 探针从超时降到约 22 秒，完整 API 回归约 49 秒。
- Meta 调用入口已经加入每个编译期调用的步数计数器，当前上限为 50000 次。
  触发上限时返回诊断状态 `5124`，用于阻止无限递归或专用化爆炸继续占用
  CPU。这个上限只是保护措施，后续要把它改成可配置的编译上下文选项。

当前增量（2026-08-16）：

- 完整 API 探针已通过 `compiler_compile`、`lainir-print` 与 `lainir-seed run` 验收：
  `compiler_api_schema.lain` 在全部 `src/compiler-archive` + std 源闭包下实例化
  `lainc.API(Memory)` 并执行返回 `1`。此前的 `5124` 步数保护停止点与
  `Meta.expand` 调用链的 verifier 错误均已消除。
- `compiler_api_compile_empty.lain`（空输入编译请求）已能生成 artifact 且
  `lainir-print` 通过；`lainir-seed run` 执行仍在空地址处崩溃，这是当前唯一的已知卡点。
- `tests/lainir_lain/run_compiler_api_bootstrap.py` 与
  `run_type_namespace_nested.py` 已通过但尚未接入 `run_all.py`，等空输入
  验收后一并接线。
- `src/lainir/lainc.l1` 已从最新源码重新冻结；冻结快照与
  `scripts/freeze_lainc_bootstrap.py` 保持可复现。

## 阶段一：冻结最小 bootstrap `lainc`

目标：生成并提交一份可被 C bootstrap 解释器执行的
`src/lainir/lainc.l1`。

状态：第一版已完成。后续扩展仍然沿用同一个冻结入口。

第一版只支持以下明确子集：

- `let`
- `std::func`
- 函数参数和返回值
- 基础表达式
- 函数调用
- `return`
- 基本 `std::module`
- 多文件输入
- 生成合法 LAIN-IR

第一条验收程序：

```lain
let main = std::func() -> i32 {
    return 42;
};
```

验收条件：

```text
lainir-print src/lainir/lainc.l1
lainir-seed src/lainir/lainc.l1 ...
lainir-seed run generated.l1 main
```

冻结 bootstrap 前，必须把 Lain 源码类型名和生成的 LAIN-IR 物理类型分开，
避免把迁移期的 `i32` 直接当作最终 Lain 语法。

## 阶段二：完成 Meta 环境

Meta 环境需要保存：

- 模块值
- 函数值
- 类型值
- 参数绑定
- 闭包捕获
- 模块成员
- 成员函数所属的 namespace

优先验证模块工厂返回成员函数的场景：

```lain
let Factory = std::func(T: type) -> Module {
    let Concrete = ...;

    return std::module {
        let push = std::func(
            values: &mut Concrete,
            value: T,
        ) -> unit {
            ...
        };
    };
};
```

必须验证：

- `T` 能进入闭包环境。
- `Concrete` 成为模块成员。
- `Factory(...)` 返回模块值。
- `Namespace.push` 能被找到。
- 成员函数生成时包含明确的 receiver 参数。
- 未定义成员产生诊断，不转成隐式外部调用。

## 泛型系统的新理解与修正方向

Lain 的类型参数采用 Zig `comptime` 参数的模型。Lain 当前没有
`Vec<T>` 语法；`Vec(SourceUnit)` 是一次编译期函数调用。

```lain
let Vec = std::func(T: type) -> type {
    ...
};

let SourceUnitVector: type = Vec(SourceUnit);
```

这次调用必须在编译期完成，并建立一个明确的专用化环境：

```text
Vec(SourceUnit)
    T        = SourceUnit
    Concrete = Vec(SourceUnit) 的具体 record 类型
    get      返回 &SourceUnit
    push     接收 &mut Concrete 和 SourceUnit
```

进入物理 lowering 前，所有 `T` 都必须已经解析成具体类型。生成的
LAIN-IR 只包含具体 record、地址、字段偏移和物理宽度；运行时不携带
类型参数，也不做泛型分派。

### 与旧实现的差异

旧实现把 `T` 当作普通名字，在多个环境里搜索一个叫 `T` 的绑定。这会
丢失“哪个 `Vec(...)` 调用产生了这个函数”的实例信息，导致
`get` 的 `&T` 停留在占位状态。当前的
`workspace.sources.get(index).syntax_unit` 因此拿不到 `SourceUnit` 的
字段布局。

修正后，成员函数必须保存创建它的专用化环境。解析 `get` 的返回类型时，
从接收者的具体类型值读取 `T`，按 `&T -> &SourceUnit` 完成替换；解析
字段投影时直接使用 `SourceUnit` 的 record 描述。

### 实现任务

1. 把 `type` 参数记录为编译期绑定，并拒绝把运行时值传给它。
2. 为每次编译期函数调用保存：被调用函数、编译期参数值、结果类型或模块、
   专用化环境。
3. 用“被调用函数 + 规范化编译期参数”作为专用化缓存键；同一组参数复用
   专用化，不同类型分别生成实例。
4. 让模块成员函数、`Concrete` 和 receiver 一起携带专用化环境。
5. 让返回类型中的 `T`、字段访问中的 `T`、`&T` 和嵌套成员调用都沿这条
   环境链解析。
6. 清理依赖全局名称搜索的 `T`、`Concrete` 和 `Namespace` 猜测逻辑。
7. 明确 Meta 与编译期函数的边界：Meta 负责 AST 操作；类型工厂和普通
   编译期函数通过 `#eval` 执行，类型值仍然只存在于编译期环境。

### 验收用例

必须通过以下行为测试：

```lain
let A: type = Vec(i32);
let B: type = Vec(u8);

let a: A = A.new();
let b: B = B.new();

A.push(&mut a, 1);
B.push(&mut b, 2);
let x: i32 = A.get(&a, 0).*;
```

验收要求：

- `A` 和 `B` 使用各自的 `T`，不能互相污染。
- 同一个 `Vec(i32)` 调用只生成一个可复用的专用化。
- `get` 的返回值能继续访问具体 record 字段。
- 生成的 LAIN-IR 通过 `lainir-print`，解释执行结果正确。
- `src/compiler-archive` 中的 `SourceUnitVector` 能通过
  `sources.get(index).syntax_unit` 这条真实路径。

### 本轮修正记录（2026-08-14）

- `std::struct` 产生的类型值现在保存定义它的 Meta 环境；字段类型中的
  `Syntax.Store`、`SourceUnitVector` 等别名可以沿着这个环境继续解析。
- 嵌套接收者路径会逐段保留具体类型值。多行写法产生的缩进会在字段匹配
  前裁掉，`workspace.syntax.units` 因此可以得到 `Vec(Unit)` 的 namespace。
- `get` 的返回类型从接收者专用化环境读取 `T`，真实生成了
  `Vec(SourceUnit).get`、`Vec(Unit).get` 和后续字段 `#load`；此前会误选
  同宽度的 `Span.get`。
- 源对象现在在创建时缓存内容指纹，专用化环境筛选按指纹比较，避免对
  每个候选函数重复扫描整个源文件导致编译超时。

这条 Zig 风格的编译期专用化路径已经通过 nested namespace 回归；完整
compiler API 还剩 verifier 阶段的独立调用 lowering 问题。暂时不把重写
所有集合结构作为主线。

## 阶段三：完成 `#eval`

固定流程：

```text
#eval block
    -> 编译为 LAIN-IR
    -> 调用 LAIN-IR interpreter
    -> 返回标量、模块、类型或 AST 数据
    -> 写回 Meta 环境
```

先支持标量和普通函数调用，再支持模块、类型和 AST 数据。加入递归深度、
解释步数、分配量和诊断限制。

## 阶段四：编译 `src/compiler-archive`

按依赖顺序逐个通过 bootstrap `lainc` 编译：

```text
compiler_core
    -> compiler_driver
    -> compiler_api
    -> lainc
```

每完成一个模块，都运行 `lainir-print` 和 `lainir-seed run`。不要一次把整个
`src/compiler-archive` closure 当作一个未分阶段的大目标。

## 阶段五：形成自举闭环

最终链条：

```text
C lainir-seed
    -> src/lainir/lainc.l1
    -> stage1 lainc
    -> stage1 编译 src/compiler-archive
    -> stage2 lainc
    -> stage2 再编译自身
```

验收条件：

- `src/compiler-archive` 全部可编译。
- stage2 和 stage3 输出字节一致。
- `lainc` 能编译普通 Lain 程序。
- host 只负责文件、内存、诊断和 artifact I/O。

## 下一项实现任务

完整 compiler-API 实例化已验收（schema fixture 经 `lainir-print`/`lainir-seed run` 返回
`1`），`Meta.expand` 调用链的 verifier 错误已修复。当前唯一的已知卡点是
空输入 fixture 在 `lainir-seed run` 下的空地址崩溃，修复顺序如下。

阶段验收顺序：

1. 定位并修复空输入 fixture 的 `lainir-seed run` 崩溃：`compiler_api_compile_empty.lain`
   （空 `SourceSlice` 的 `api.compile(request)`）生成的 artifact 已通过
   `lainir-print`，但解释执行解引用空地址（access violation）。用解释器调试
   输出定位到具体 `#proc`/指令。
2. 补写验收脚本 `tests/lainir_lain/run_compiler_api_compile_empty.py`
   （`compiler_compile` → `lainir-print` → `lainir-seed run`，status 为 0）。
3. 把输入换成真实的 Lain 源码字节串，检查返回的 artifact、诊断和状态。
4. 把这条实例化路径接入 `run_compiler_source_closure.py`，并连同
   `run_compiler_api_bootstrap.py`、`run_type_namespace_nested.py` 一起
   接入 `run_all.py`。
5. 开始 stage1/stage2 的字节级自举比较，完成阶段五闭环。

已有的 Meta 基础仍需保持以下端到端约束：

```text
工厂调用 -> 闭包参数传播 -> 模块成员 -> receiver -> LAIN-IR lowering
```

`Memory`、`SourceBuffer`、`SourceSlice`、`SourceWorkspace` 以及
`Elaborator(Memory)` 已通过独立回归；`compiler_probe_*` fixture 覆盖
workspace、source factory、slice、parts 等路径。下一步先解决空输入的运行时
崩溃，再按六个工厂补齐回归并检查 `memory.Span` 的专用化缓存是否覆盖所有
调用环境。
