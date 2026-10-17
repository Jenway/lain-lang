# Lain-Meta

> [!QUOTE]
> Meta 究竟是个什么样子，C Macro 搞了很多年，也并没有完全搞清楚。
>
> 可能 CPP 的思路比较好，搞了个静态反射，但是 `std::meta` 完整进标准不知道要哪一年。

## 概念与职责

Lain 把语言规则从编译器内核中抽离。底座（Seed 与 LAINVM）只提供无类型指令集与带权限边界的执行环境；源语言的全部语法、类型系统与降解（lowering）规则由 **Meta** 程序定义。

Meta 本身作为普通程序运行在 LAINVM 内。它的输入是宿主解析出的无语义原始语法树（RawAst），输出是扁平的 LAINIR 模块文本。

## eval 与 apply

系统不提供特权的 `#eval` IR 指令。编译期求值统一由 `eval` 组织、`apply` 承接。

```text
eval(源码形式, 环境) → 编译期值
apply(LAINIR 过程, 实参, 能力与限额) → 执行结果
```

- **eval 属于 Meta 库函数**：负责词法作用域管理与源码形式解构。当形式需要发生机器计算时，eval 负责生成自包含的临时 LAINIR 过程并调用 apply，再把执行结果包装为编译期值。
- **apply 属于 Seed 宿主服务**：负责物理执行。apply 只接受普通过程与常量参数，在独立的地址空间与沙箱中运行 LAINIR，不解释 Lain 源码语义。
- **编译期值**：eval 交付的值包含标量、类型（尺寸与布局元数据）、过程引用或 AST 节点。类型与过程的语义表示属于 Meta；用于目标代码时由 Meta 降解为具体数据或指令。

## 语义处理管线

源码在 Meta 内部的处理分为展开（Expand）、细化（Elaborate）与降解（Lower）：

- **展开**：按 Meta 规则变换语法形式。宏与展开处理器操作语法树，由宿主 AstOut 提供候选树事务与回滚支持。
- **细化**：建立符号绑定、词法作用域并推导类型。遇到结构体布局或编译期常量计算时，Meta 调用 `eval` 组织计算，经 `apply` 获得数值结果后回填入类型注册表。
- **降解**：将高级语义彻底抹平。复合类型降解为具体字节偏移与尺寸，变量与调用降解为物理寄存器、`#alloca`、`#load`、`#store` 与 `#call`。

## apply 契约

C 接口见 `seed/kernel/include/lain/meta/apply.h`。执行期约束如下：

| 项 | 规则 |
| --- | --- |
| 模块 | 已验证的 LAINIR 文本，必须自包含所有依赖过程，不引入隐式宿主状态 |
| 入口 | 模块内的普通 `#proc` |
| 实参 | 标量通过固定槽位的 Wire 传输；字节数据通过显式只读映射传递 |
| 限额 | 步数、stack、调用深度、配额与已冻结的能力表（`LainMetaApplyLimits`） |
| 结果 | 标量，或固定长度按值拷出的字节块 |
| 拒绝 | 结果类型含 `#addr` 一律拒绝，严禁裸物理地址逃逸进 Meta |

- **空间隔离**：每次 apply 运行在独立创建的 VSpace 内，不继承 Meta 的内存授权，执行结束后销毁。
- **预算继承**：嵌套 apply 共享外层总预算（Fuel / Quota）。执行时划走预留，未用份额执行后退还；配额原子预扣，拒绝执行时不扣费。

## 宿主上下文与能力模型（CSpace）

Meta 运行于受限沙箱中，通过 CSpace 调用宿主服务：

- **隐式 Context**：宿主在注册能力时绑定内部指针（context），登记完毕后能力表永久冻结。分派调用时由 VM 注入该指针，LAINIR 参数只承载业务数据，宿主指针不对 Meta 可见。
- **无特权穿透**：Meta 无法伪造或读取宿主状态。能力调用仅解决“由谁承接服务”，实际内存读写仍由 VSpace 权限判定。

## 源码结构与语法解耦

- **机制层 Reader 零语义**：底层只识别基础词法单元（Token）与括号组（Group），不包含关键字、类型或语句结构的预设。
- **RawAst 存储**：语法树由宿主线性写入只读 Arena，通过 VSpace 映射给 Meta。
- **语法绑定**：语法结构的合法性与解释方式由 Meta 内部的绑定表驱动。改变 Meta 的绑定逻辑，同一段语法树即可解释为不同的语言语义。

## 模块、清单与构建（实现口径）

Meta 本体今天就是 `bootstrap/` 下 **28 个 Lain 源码模块**（`meta.lain` 排在清单末位）：

- `bootstrap/SOURCE_ORDER` 是**模块清单**：每行 `<相对路径> [<根名> …]`，路径相对清单所在目录、
  省略 `.lain`，`#` 起注释。清单顺序即降级顺序，它是产物确定性的来源 —— 同一份清单必须给出
  逐字节相同的模块。
- `lain0 --unit bootstrap/SOURCE_ORDER -o build/lain0/bootstrap.l1` 一次编出整份 Meta 的 LAINIR。
  `scripts/build_meta_unit.py` 只做这层调用，并写出驱动要的单元清单 `build/lain0/bootstrap_unit.txt`。
- 每个模块的顶层声明发 `<键>__<名字>`（模块键 = 相对源根的路径去 `.lain`、`/` 换 `_`）；
  清单里标为**根名**的声明保持平坦 —— 宿主按平坦字符串查找入口，所以 `meta.lain` 的三个
  `lain_std_abi_version` / `lain_std_initialize` / `lain_std_lower` 是根名。
- 跑起来：

```bash
python scripts/build_meta_unit.py
./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    arith.lain bootstrap/lain/examples/arith.lain
```

  `verify subroutines=709 data=167`、`image ok`、`host_status=0`、`region grants=2`、
  `output bytes=74`。设计与取舍见 `docs/plan-modules.md`
  （决策 D-004），成员位置表与拒绝码见 `docs/04-bootstrap-dialect.md` 的《模块：成员位置与符号命名》。
