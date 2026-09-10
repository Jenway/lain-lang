# Lain 当前路线图

基线日期：2026-09-10。

本文只描述从当前代码状态开始的后续工作。已经完成的迁移过程、旧架构和旧阶段编号不在
本文中重复。

## 1. 当前基线

当前仓库已经具备以下基础：

- 项目源码按 `seed/`、`bootstrap/`、`src/` 分层，生成产物统一写入 `build/`；
- LAINIR 只表达确定的物理程序，值限于 `#bits<N>`、`#addr` 和 `#unit`；
- C seed 能够验证和执行 LAINIR，`#eval` 通过临时 TCB 运行，并在最终产物中消失；
- `src/lainvm/` 已经独立于 `src/lainir/`，定义 Artifact、Procedure、Value、TCB、VSpace、
  Trap、预算和子过程执行；
- bootstrap 已经能够通过 LAINVM 路径执行整数计算、标量 Meta 调用、effect 构造、
  effect operation 构造和 module factory；
- 正式标准库和正式编译器源码能够生成可验证的 LAINIR bundle；两次独立生成结果一致。

当前尚未解决的问题：

- 正式 lainc 发出的 `Vm.eval` 请求还没有在真实编译入口完成端到端执行；
- bootstrap 对不同 Meta 返回值仍有分散的识别和构造逻辑，尚未形成统一 callable 路径；
- 源码仍使用裸 `type`、旧 `comptime` 模型、generic policy 和 compiler-owned
  specialization；
- `std::func` 尚未实现 `?{}` 输入行；
- 生成的 `srclainc.l1` 尚未完成 gen1 -> gen2 -> gen3 固定点自举；
- C backend、完整诊断和发布检查尚未收口。

## 2. 当前设计边界

### 2.1 LAINIR

LAINIR 定义物理程序，只负责：

- procedure、block、branch 和 call；
- 定宽整数、地址和 unit；
- `#data` 只读静态物理数据；
- `#alloca` 当前 procedure activation 内的临时存储；
- `#lea`、`#load`、`#store` 和明确的物理运算；
- 可验证的 `#eval` 块。

LAINIR 不包含类型对象、模块对象、AST、泛型、字段语义、宽泛的 `#primitive` 或 VM 调度
对象。

### 2.2 LAINVM

LAINVM 执行已经验证的 LAINIR。它拥有 TCB、VSpace、Trap、执行预算和 procedure 执行。
每次 `#eval` 使用临时 TCB，并默认共享调用者的 VSpace。

LAINVM 的公开执行接口只接收 Artifact、Procedure 和物理参数，只返回普通物理 Value 或
产生 Trap。LAINVM 不判断一个地址在 Meta 中代表类型、模块还是 AST。

### 2.3 Meta 与标准库

Meta 解释 RawAst，定义 Lain 的语言含义，并生成 LAINIR。Parser 只建立 Atom 和 Group，
不内建函数、类型、泛型或 effect 语法节点。

以下规则由标准 Meta 环境定义：

- 类型是一等 Meta 值，类型值所属的类型写作 `std::type`；
- `std::func(T: std::type) -> std::type` 是普通 Meta 函数；
- `Vec(T)`、`Result(T, E)` 等类型构造是普通 Meta 调用；
- 函数签名形式是
  `std::func(显式参数) ?{环境输入} -> 返回类型 !{输出 effect} {函数体}`；
- `?{}` 中的值由调用环境提供；`!{}` 中的 operation 可以由函数执行时发出；
- `?{T}` 等价于待推导的 `?{T: _}`，`?{T: std::type}` 是显式写法；
- 函数依赖的环境输入必须在 `?{}` 中声明。

编译器不提供独立的泛型语言机制，也不按 Meta 函数的返回类别建立 type factory、module
factory 或 AST factory 执行协议。所有 Meta 函数共用同一条调用与 LAINVM 执行路径。

## 3. 编码阶段

执行顺序如下：

```text
编码 0：清除旧泛型语义并固定新接口
  -> 编码 1：统一 Meta callable 与 LAINVM 执行
  -> 编码 2：实现完整 std::func 签名
  -> 编码 3：实现输入 effect 的解析与推导
  -> 编码 4：迁移标准库和正式编译器
  -> 编码 5：完成固定点自举
  -> 编码 6：收口 backend 与发布检查
```

### 编码 0：清除旧泛型语义并固定新接口

目标：让后续实现只面对一套明确的语言模型，即使这一步会暂时破坏旧标准库和旧 Meta。

工作：

- 在标准 Meta 根环境中建立 `std::type`；
- 删除把裸 `type` 当作特殊参数类型的判断；
- 删除 `comptime T: type` 语法和隐式自由类型变量；
- 删除 generic parameter kind、generic policy 和 generic specialization token；
- 删除按 binding 返回类型选择 Meta 执行路径的代码；
- 定义统一 callable 数据结构所需的最小字段：显式参数、输入行、返回类型、输出 effect 行、
  函数体和声明环境；
- 添加新语义的最小正例和旧语义的拒绝测试。

验收：

- `std::type` 能通过正常的 `std` 名称解析获得；
- 裸 `type`、`comptime` 泛型和旧 generic policy 不再被当前编译器接受；
- 仓库不存在 `program_is_type_binding` 一类决定执行协议的返回类别判断；
- 失败的标准库构建能够明确指出尚待迁移的源文件，不通过兼容分支继续运行。

提交：

```text
meta: remove compiler-owned generic semantics
```

### 编码 1：统一 Meta callable 与 LAINVM 执行

目标：标量、类型、模块和 AST 返回值使用同一个 Meta 函数调用过程。

工作：

- 从 callable 的静态签名生成临时 LAINIR procedure；
- 将 Meta 参数 lowering 为对应的物理值或地址；
- 通过统一 LAINVM API 执行 procedure；
- 根据 callable 的静态返回类型，由 Meta 解释返回的物理值；
- 在正式 lainc 执行入口安装 `Vm.eval` handler，使请求在子 TCB 中执行；
- 删除 bootstrap 中分别构造 scalar、module、type 或 AST 返回对象的执行协议；
- 保留可选的结果缓存，但缓存只是一种实现策略。

验收：

- 同一调用入口能够执行返回整数、`std::type`、Module 和 AST handle 的 Meta 函数；
- LAINVM API 中没有 Meta kind、对象标签或 `EvalResult`；
- 正式 lainc 的真实 fixture 会触发 `Vm.eval` handler，而非 recording provider；
- 正常结果与 Trap 分别进入返回路径和诊断路径；
- 最终 LAINIR 中不残留 `#eval`。

提交：

```text
meta: execute all callables through lainvm
```

### 编码 2：实现完整 `std::func` 签名

目标：由 `std::func` 的 Meta elaborator 解释完整函数声明，Parser 保持无语义。

工作：

- 从 RawAst 读取显式参数组；
- 读取可选的 `?{}` 输入行；
- 读取 `->` 返回类型；
- 读取可选的 `!{}` 输出 effect 行；
- 读取函数体并建立声明环境；
- 将以上内容写入统一 callable 表示；
- 为缺失分隔符、重复段和错误顺序提供 source diagnostic。

验收：

```lain
let identity = std::func(value: T)
    ?{T: std::type}
    -> T
    !{} {
    return value;
};
```

该声明由标准 Meta elaborator 完成，不新增 Parser 语义节点；空的 `?{}` 与 `!{}` 可以省略。

提交：

```text
meta: elaborate complete function signatures
```

### 编码 3：实现输入 effect 的解析与推导

目标：让调用环境能够以有类型、可诊断的方式满足 `?{}` 请求。

工作：

- 为函数类型加入 input row；
- 从当前 Meta 环境解析具名输入；
- 使用参数、返回类型、其他输入和函数体形成的约束推导 `?{name}` 的类型；
- 检查缺失输入、重复输入、歧义和类型不匹配；
- 让输入请求和输出 operation 共享有类型的 request、handler、continuation 基础设施；
- lowering 时把已解析输入变成确定参数，并消除 `?{}`。

验收：

```lain
let max = std::func(a: T, b: T)
    ?{T, ord: Ord(T)}
    -> T {
    return ord.max(a, b);
};
```

- `T` 能推导为 `std::type`；
- `ord` 能从调用环境解析；
- 缺少 `ord`、无法推导 `T` 和存在多个候选时分别产生稳定诊断；
- 输入 effect 不会被记录进函数的 `!{}` 输出 effect 行。

提交：

```text
meta: implement typed input effects
```

### 编码 4：迁移标准库和正式编译器

目标：当前源码全部使用新类型函数和输入 effect 模型。

工作：

- 将类型标注从裸 `type` 迁移到 `std::type`；
- 将 `Vec`、`Result`、`Slice`、`String`、内存模型和 LAINVM 类型构造迁移为普通 Meta
  函数；
- 将依赖调用环境的参数迁移到 `?{}`；
- 删除 `ComptimeValue` 中只服务于旧泛型机制的分类；
- 删除旧 specialization 诊断和旧策略 fixture；
- 更新当前 Meta、AST、effect 和标准库文档。

验收：

- `std/**/*.lain` 和 `src/**/*.lain` 中不再出现裸 `type` 或 `comptime` 泛型；
- 正式标准库闭包和正式编译器源码闭包均能生成并通过 verifier；
- `Vec(i32, Allocation, Bounds)` 等调用通过普通 Meta callable 路径完成；
- 标准库测试覆盖显式 Meta 参数、推导输入和输出 effect 的组合。

提交：

```text
stdlib: migrate type functions and input effects
```

### 编码 5：完成固定点自举

目标：正式 Lain 编译器能够重新编译自身，并产生稳定结果。

工作：

- 用 bootstrap 生成 gen1 `srclainc.l1`；
- 用 gen1 编译相同源码得到 gen2；
- 用 gen2 编译相同源码得到 gen3；
- 对 gen2 和 gen3 做规范化比较；
- 验证三代产物中均不存在旧泛型设施、Meta 返回包装或残留 `#eval`；
- 将所有生成产物保存在 `build/`。

验收：gen2 与 gen3 规范化一致，并且二者都能编译最小程序、标准库核心和输入 effect
fixture。

提交：

```text
bootstrap: reach the new compiler fixed point
```

### 编码 6：收口 backend 与发布检查

目标：把已自举的编译器接回完整 native 构建与发布流程。

工作：

- 补全 C backend 剩余物理指令与 ABI；
- 运行 native compiler matrix、LAINIR API baseline 和 determinism check；
- 完成 source span、Trap source mapping 和发布诊断；
- 在真实 CI runner 上验证 bootstrap、标准库和 native smoke；
- 建立统一 release gate。

验收：从干净 checkout 可以生成固定点编译器、编译并运行 native fixture，所有 release
检查通过。

提交：

```text
release: close the self-hosted compiler pipeline
```

## 4. 开发规则

- 正确性优先于兼容性；删除错误抽象，不保留旧别名和适配层。
- 每个编码阶段完成后立即提交；阶段内部可独立验收的切片单独提交。
- 测试必须经过真实解析、Meta、LAINVM 和 verifier 路径；文本搜索只能作为附加检查。
- 编译产物、snapshot、bundle 和 executable 只能写入 `build/`。
- 若一个阶段暂时破坏自举，必须提供明确失败点和恢复它的下一阶段，不能用兼容实现掩盖。
- 遇到会改变本路线图设计边界的问题，先讨论再修改实现。
