# Lain-Lang 架构总览

> [!QUOTE]
> 我们能否让编译器本体尽量的精简，让所有的高级语言特性都以库的方式实现？
>
> 我们希望它的 meta 功能尽可能强大，这里的 meta 指的是在编译期间对于 AST 的操作变换。
> 
> 我们希望它的底层中间表示（也就是 Lain-IR）尽可能简洁而忠实反映现代的 CPU。
>
> 我们希望它的编译期运行在一种虚拟机环境（Lain-VM）里，安全的给予最多的编译期能力。

## 分层

- **Lain-IR**（`docs/01-lain-ir.md`）：扁平的、带验证器的中间表示，是 Meta 与宿主之间的
  私有 ABI。信任门是 `lainir_parse` + `lainir_verify`。
- **Lain-VM**（`docs/02-lain-vm.md`）：执行 LAINIR，提供能力边界、配额、区段、stack 与
  apply。
- **Meta**（`docs/03-lain-meta.md`）：用 Lain 写的编译器库，定义源语言的语法、类型与降级
  规则；源码在 `bootstrap/`，共 28 个模块。
- **lain0**（`docs/lain0.md`）：C++ 前端，把 Lain 源码（含 `bootstrap/`）编成 LAINIR。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [development.md](development.md) | 构建、驱动 `lain-meta` 的选项、回归与闸门 |
| [plan-selfhost.md](plan-selfhost.md) | 自举进度、剩余缺口与复现命令 |
| [01-lain-ir.md](01-lain-ir.md) | LAINIR 的算子、验证规则与文本格式 |
| [02-lain-vm.md](02-lain-vm.md) | LainVM 的地址空间、能力、配额与 TCB |
| [03-lain-meta.md](03-lain-meta.md) | Meta 的职责、eval/apply 契约与模块产出 |
| [04-bootstrap-dialect.md](04-bootstrap-dialect.md) | bootstrap 方言：语法、降级形状与缺失形态 |
| [lain0.md](lain0.md) | lain0 的编译管线、CLI 与模块命名空间 |
| [plan-modules.md](plan-modules.md) | 模块身份、符号命名与加载顺序 |
| [proposal-modules-v2.md](proposal-modules-v2.md) | 每模块一份产出的设计与取舍 |
| [decisions.md](decisions.md) | 设计决策记录（D-001 起） |
| [diagnostic-codes.md](diagnostic-codes.md) | Trap 与拒绝码的号段 |
| [stdlib/concepts.md](stdlib/concepts.md) | 泛型机制的设想 |
| [stdlib/effects.md](stdlib/effects.md) | effect 机制的设想 |
