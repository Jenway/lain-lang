# Lain 语言核心设计与哲学

## 设计原则

Lain 的核心原则只有一句：

**编译器只提供无法在库中实现的 substrate，不提供语言语义本身。**

这意味着：

- `type / module / effect / signature / interface` 不是编译器内建对象
- 这些概念由 `meta` 库定义、组合、检查、lower
- 编译器只负责承接它们留下来的语法、工件、IR 和诊断

另外两条原则保持不变：

- **显式性**：副作用、阶段、接口边界都必须显式可见
- **零成本**：高层概念只有在 lowering 后留下必需的物理成本

## 三相架构

Lain 不是“Scheme + runtime”两层，而是三个明确阶段：

### 1. `Scheme meta phase`

这一层负责语言层面的改写与静态建模：

- 解析和规范化顶层形式
- 定义 `module`、`signature`、`interface`、`effect` 等对象模型
- 做名字解析、静态检查、ascription、lowering 准备
- 产出 lowered declaration / IR builder 调用 / 接口工件

这一层默认应保持纯粹。它的职责是“改语言”，不是“替编译器跑一个操作系统”。

### 2. `compile-time lain phase`

这一层负责真正有副作用的编译期计算：

- 读取 schema / IDL / 头文件
- 生成绑定
- 生成额外源码或接口工件
- 调用外部工具链
- 进行受控的文件、进程、网络等操作

这部分必须写成 `lain`，而不是继续往 Scheme VM 里塞越来越多的宿主能力。

### 3. `runtime phase`

这一层是最终程序本身。它消费前两层留下来的 lowered 结果与工件，不再保留高层 module/meta 语义。

## 为什么必须是三相

如果把所有编译期能力都放进 Scheme，会出现两个问题：

1. Lisp VM 会被迫承载 IO、网络、进程、包管理、桥接生成，最终变成一个巨大而混乱的编译期运行时
2. 语言用户会把“语言改写”和“副作用构建”混在一起，边界越来越不清楚

三相设计的目的就是把这两类职责拆开：

- Scheme 只负责语义变换
- `lain` 负责 effectful compile-time work

## 编译器真正提供什么

编译器只需要提供以下底座。

### 1. syntax substrate

- 语法节点构造与拆解
- span / hygiene / source mapping
- symbol / gensym

### 2. phase substrate

- 调度 `Scheme meta`
- 调度 `compile-time lain`
- 在阶段之间传递依赖、工件、诊断

### 3. IR substrate

- 构造 L1 / LainIR
- 定义 extern / global / linkage / `link_name`
- 输出后端可消费的物理表示

### 4. artifact substrate

- 读写 `.lci`
- 记录依赖和 hash
- 注册生成文件

### 5. capability injection substrate

- 给 `compile-time lain` 注入 capability handle
- 不把“网络 / 包管理 / C++ 绑定”硬编码成编译器语义

## 什么不应内建

以下东西不应作为编译器 semantic API 存在：

- `host.make-module`
- `host.make-type`
- `host.make-effect`
- `host.make-signature`
- `host.make-interface`

如果这些能力存在，也应只是通用语法或工件底座上的库实现。

## 模块系统在这套架构里的位置

模块系统属于 `Scheme meta phase` 的语言对象模型：

- `module` 是 meta 层对象，不是默认运行时值
- `signature` 是 module 的接口类型
- `interface` 继续表示动态分发协议
- `import(path)` 返回 module 对象
- `export` 形成接口，不是 runtime effect

因此，模块机制的实现重点不是把 module 做进 L1，而是：

- 在 meta 层定义 module object
- 在 `.lci` 中序列化 module boundary
- 在 lowering 时只保留物理链接所需的信息
