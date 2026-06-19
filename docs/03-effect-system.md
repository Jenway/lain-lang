# 效果系统、异步与错误处理设计

## 效果系统的统一定位

Lain 的 effect system 不只服务运行时，也服务编译期。

准确地说，effect 是一套统一语义，用来描述：

- 运行时程序的不纯行为
- compile-time `lain` 的副作用行为

两者共享“显式声明、显式传播、显式处理”的原则，但它们依赖的 capability 来源不同：

- 运行时 effect 依赖运行时库和目标平台
- 编译期 effect 依赖编译阶段注入的 capability

## 运行时 effect

运行时 effect 仍然覆盖传统的不纯操作：

- `IO`
- `RawPtrRead` / `RawPtrWrite`
- `Throws<E>`
- `Suspend`

这些 effect 最终都会被 lowering 到物理层行为，例如：

| effect | 物理结果 |
| :-- | :-- |
| `IO` | 系统调用或运行时封装 |
| `RawPtr*` | `#load` / `#store` |
| `Throws<E>` | 显式返回值编码 |
| `Suspend` | 上下文切换或调度协议 |

## 编译期 effect

编译期 effect 的目标不是把 Scheme 变成大型宿主脚本语言，而是让 compile-time `lain` 用同一套语言机制表达副作用。

典型的编译期 effect 包括：

- `CtFsRead`
- `CtFsWrite`
- `CtProcess`
- `CtNet`
- `CtEmitSource`
- `CtEmitInterface`

这些名字只是示意，真正的 effect 集合应由库定义，而不是由编译器硬编码。

## 为什么编译期副作用必须放在 lain

如果把编译期 IO、网络、进程都塞进 Scheme VM，会立即出现三个问题：

1. Lisp VM 会膨胀成大型宿主环境
2. 语言层变换和副作用构建混在一起
3. effect system 无法统一地约束这些行为

因此编译期副作用应遵循这条规则：

- Scheme 只生成、检查、变换语言
- compile-time `lain` 执行 effectful work

## 编译期 capability 模型

编译器不应直接“提供网络栈”或“内建包管理器”。

更合理的做法是：

1. 编译器定义 capability injection substrate
2. driver 或外部库实现具体 capability
3. compile-time `lain` 通过显式 effect 使用 capability
4. meta 层只知道 capability 的存在与边界，不知道业务实现细节

所以编译器只负责：

- 注入 capability handle
- 检查 phase boundary
- 记录依赖和产物

而不负责：

- 实现完整网络库
- 实现包管理逻辑
- 把 bridge generator 写死在编译器里

## 错误处理

Lain 仍然应区分两类错误：

### 1. 预期失败

例如：

- 文件不存在
- schema 解析失败
- 外部工具返回业务错误

这类错误应通过 `Throws<E>` 或同类显式 effect 表达，并保留普通控制流语义。

### 2. 致命错误

例如：

- 内部 lowering 不变式被破坏
- 不可恢复的运行时内存错误

这类错误不应被包装成普通业务 effect，而应直接终止。

## 异步与 `Suspend`

`Suspend` 依然是运行时效果系统的关键例子，因为它说明：

- effect 不等于某个固定 runtime
- handler 决定调度策略
- 语言表面不需要被 `async` 关键字污染

这条原则也能反过来指导 compile-time phase：

- 编译期 capability 不该通过特殊语法魔法提供
- 它也应通过普通 effect + handler/capability 协议接入

## 一个编译期例子

```lain
let generated = comptime {
  let spec = fs::read("api.json")?;
  let bridge = cppbind::generate(spec)?;
  emit::source("gen/api.lain", bridge.source)?;
  emit::interface("gen/api.lci", bridge.interface)?;
};
```

这段代码想表达的是：

- 编译期代码就是 `lain`
- 它可以有 effect
- 它需要 capability
- 它产出 artifact

这里不需要把 `cppbind` 或 `fs` 做成编译器魔法。

## 与 meta 系统的关系

effect system 和 meta system 的分工应明确：

- `Scheme meta` 定义 effect 语法、effect 对象、传播与 lowering 规则
- compile-time `lain` 消费这些规则去执行副作用逻辑
- 编译器只提供 phase、artifact、diagnostic、capability substrate

这保证了 `effect` 本身仍然是语言库对象，而不是编译器的语义硬编码。
