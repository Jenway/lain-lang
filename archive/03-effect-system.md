# 效果系统、异步与错误处理

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

编译期 effect 的目标不是把 Scheme 变成大型宿主脚本语言，而是让 compile-time lain 用同一套语言机制表达副作用。

典型的编译期 effect：`CtFsRead`、`CtFsWrite`、`CtProcess`、`CtNet`、`CtEmitSource`、`CtEmitInterface`。名字只是示意，真正的 effect 集合由库定义，不由编译器硬编码。

编译期 IO、网络、进程如果全塞进 Scheme VM，Lisp VM 会膨胀成大型宿主环境，语言层变换和副作用构建搅在一起，effect system 也没法统一约束这些行为。所以编译期副作用应该：Scheme 只生成、检查、变换语言，compile-time lain 执行 effectful work。

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

分工：Scheme meta 定义 effect 语法、effect 对象、传播与 lowering 规则；compile-time lain 消费这些规则去执行副作用逻辑；编译器只提供 phase、artifact、diagnostic、capability substrate。`effect` 本身是语言库对象，不是编译器的语义硬编码。
