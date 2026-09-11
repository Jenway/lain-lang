# 效果系统、异步与错误处理

本文描述由标准库 Meta 层定义的语言能力。effect 的对象模型、检查、传播和 lowering 规则不属于 compiler core；编译器只提供执行 lowering 结果所需的 LAINIR、阶段调度和 capability 底座。

effect 是一套统一语义，描述两件事：运行时程序的不纯行为，以及 compile-time lain 的副作用行为。两者共享"显式声明、显式传播、显式处理"的原则，但 capability 来源不同——运行时 effect 依赖运行时库和目标平台，编译期 effect 依赖编译阶段注入的 capability。

## 运行时 effect

覆盖传统的不纯操作：`IO`、`RawPtrRead` / `RawPtrWrite`、`Throws<E>`、`Suspend`。

这些最终都会被 lowering 到物理层：

| effect | 物理结果 |
| :-- | :-- |
| `IO` | 系统调用或运行时封装 |
| `RawPtr*` | `#load` / `#store` |
| `Throws<E>` | 显式返回值编码 |
| `Suspend` | 上下文切换或调度协议 |

## 编译期 effect

编译期 effect 让 compile-time Lain 用同一套语言机制表达副作用。

典型的编译期 effect：`CtFsRead`、`CtFsWrite`、`CtProcess`、`CtNet`、`CtEmitSource`、`CtEmitInterface`。名字只是示意，真正的 effect 集合由库定义，不由编译器硬编码。

标准库 Meta 负责生成、检查和 lowering 这些形式。需要实际执行 IO、网络或进程操作时，编译期 Lain 通过显式 capability 完成工作。所有编译期执行仍经过 LAINIR `#eval` 边界。

## 编译期 capability 模型

编译器不直接"提供网络栈"或"内建包管理器"。

正确做法：编译器定义 capability injection substrate，driver 或外部库实现具体 capability，compile-time lain 通过显式 effect 使用 capability，meta 层只知道 capability 的存在与边界，不知道业务实现细节。

编译器负责注入 capability handle、检查 phase boundary、记录依赖和产物。不负责实现完整网络库、包管理逻辑、或把 bridge generator 写死在编译器里。

## 错误处理

分两类。

预期失败——文件不存在、schema 解析失败、外部工具返回业务错误——通过 `Throws<E>` 或同类显式 effect 表达，保留普通控制流语义。

致命错误——内部 lowering 不变式被破坏、不可恢复的运行时内存错误——不应包装成普通业务 effect，直接终止。

## 异步与 Suspend

`Suspend` 说明了几件事：effect 不等于某个固定 runtime，handler 决定调度策略，语言表面不需要被 `async` 关键字污染。

这条反过来也能指导 compile-time phase：编译期 capability 不该通过特殊语法魔法提供，应通过普通 effect + handler/capability 协议接入。

## 一个例子

```lain
let generated = comptime {
  let spec = fs::read("api.json")?;
  let bridge = cppbind::generate(spec)?;
  emit::source("gen/api.lain", bridge.source)?;
  emit::interface("gen/api.lci", bridge.interface)?;
};
```

编译期代码就是 lain，可以有 effect，需要 capability，产出 artifact。不需要把 `cppbind` 或 `fs` 做成编译器魔法。

## 与 meta 系统的关系

分工：标准库 Meta 定义 effect 语法、effect 对象、传播与 lowering 规则；compile-time Lain 消费这些规则并通过 `#eval` 执行副作用逻辑；编译器只提供 phase、artifact、diagnostic 和 capability substrate。`effect` 本身是标准库语言对象，不是编译器的语义硬编码。

## 实现策略：TCB 与 CPS

一个 effect 被 handle 时，handler 体里唯一与「恢复」有关的动作是 `resume`：

```lain
std::handler(E) ! { Remaining } {
    op(args) {
        resume EXPR
    }
}
```

**TCB 与 CPS 的全部差别只在 `resume` 的实现上**：被 handle 的 effect 是什么、handler 体
怎么写，都不影响。两者是同一接口的两份实现：

| 策略 | 剩余计算在哪 | 由谁提供 | 代价 |
| --- | --- | --- | --- |
| TCB | VM 状态（procedure、instruction position、activation、预算） | LAINVM 原语 | 上下文切换 |
| CPS | 一个普通函数值 | 编译器变换 | 零开销，但需全程序变换 |

因此两者是**同一个 effect 的两种 lowering**，而不是两个不同的 effect。

### 策略由 handler 的 handler 决定

`std::handler` 不需要额外的策略参数。`Handler(Handled, Remaining)` 的 `Remaining` 槽已经
承担了这件事：它记录「本 handler 自己还需要什么」，也就是它的外层 handler 必须负责什么。
`std/core/arena.lain` 是现成的例子——handler 的类型参数里写
`{effects.Throws(arena.OutOfMemory)}`，函数体的 effect 行是同一集合。

于是策略的可见性不是全局属性，而是**相对于外层 handler** 的：控制原语在 `Remaining` 里
露着就可见，被外层消解掉就不可见。

- **CPS**：外层变换掉控制原语，`resume` 落成一次普通调用；`Remaining` 里从此没有它。
- **TCB**：控制原语保持为运行期机制，由 LAINVM 提供。

策略选择发生在编译期，不是运行期 effect。

### 与 LAINVM 的关系

LAINVM 只实现 TCB 一条路径，且只提供原语（执行入口、VSpace、预算、Trap）。CPS 路径不
涉及任何 VM 操作——变换之后没有 VM 参与，因此 LAINVM 对它是零参与。

`#eval` 不属于这一层：它是 LAINIR 的概念，由 LAINIR 契约定义，LAINVM 只提供实现它所需
的原语。见 [`../04-lain-vm.md`](../04-lain-vm.md) §8.4。

### 当前实现状态

策略轴目前是**退化的**。现有全部 handler 的 `resume` 都在尾位置，或根本不 `resume`
（`std/bounds.lain` 全部是 throw/trap/落空）：

- `std/core/arena.lain`：3 处 `resume`，全部尾位置；
- `std/platform/memory.lain`：5 处 `resume`，全部尾位置；
- `src/lainvm/interpreter.lain`：`resume execute_child(...)`，尾位置。

尾位置 `resume` 正是 CPS 无需变换、TCB 退化为同步调用的情形。**没有任何 handler 多次
`resume`、保存 continuation 或延迟 `resume`。** 这解释了为什么 Lain 实现里的
`suspend_tcb`、`resume_tcb`、`run_slice` 与 Endpoint 都没有调用者：它们的消费者是
非平凡 `resume` 的 handler，而这类 handler 尚未出现。
