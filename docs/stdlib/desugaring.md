# Lain-Lang 标准语法脱糖协议 (desugaring.md)

Lain-Lang 的机制层 `Reader` 仅产生零关键字的形式树 `RawAst`（`Token` 与 `Group`），不内置高级面向对象方法语法、字符串插值或模式匹配状态机。本规范定义语言表层语法在 **Expand（展开）** 与 **Elaborate（细化）** 阶段基于 `AstOut`（[../03-lain-meta.md](../03-lain-meta.md)）的标准重写协议。

---

## 1. 统一方法调用糖 (UFCS)

为了在保持过程语义纯粹（函数永远是一等可传的静态过程、不搞运行时方法虚表绑定）的同时提供自然的点号链式调用体验，语言在 Meta 层提供 UFCS 脱糖。

### 1.1 重写规则

当调用表达式呈现 `receiver.method(args...)` 且 `receiver` 并非静态 `Module` 名，而是普通值表达式时：

```lain
// 源码形式
let first: i64 = deref(v.get(0));
let scaled = pt.scale(10);

// Expand / Elaborate 重写为（假设 v: IntVec.Self, pt: Vec2）：
let first: i64 = deref(IntVec.get(ref_of(v), 0));
let scaled = Vec2Mod.scale(pt, 10);
```

### 1.2 解析与引用自适应算法

1. **推导接收者类型**：确定 `receiver` 的静态类型 `T`（若 `receiver` 为 `Ref(T)` 或 `Mut(T)`，同步记录其基础实体类型 `T`）；
2. **检索关联模块**：按 [modules.md](modules.md) 规则扫描当前词法作用域可见的模块，查找同时满足以下条件的唯一模块 `M`：
   * 导出了 `pub let Self = T;`（且与调用点作用域内可见）；
   * 导出了可调用的公开成员 `pub let method = ...;`；
3. **首参自适配（Value / `Ref(Self)` / `Mut(Self)`）**：
   * 若 `M.method` 首参期望 `Self`，而实参为值 `T`，直接传入 `receiver`；
   * 若首参期望 `Ref(Self)` 而调用方持有可读左值 `T`，自动补写 `ref_of(receiver)`；
   * 若首参期望 `Mut(Self)` 而调用方持有局部可变左值 `T`，自动补写 `mut_of(receiver)`（不可变左值传给 `Mut(Self)` 仍在编译期严格拒绝！）；
4. **冲突与歧义诊断**：若存在两个活跃可见模块均绑定同一个 `Self` 并导出了同名 `method`，立即报错拒绝，要求显式写成 `M.method(...)` 消除歧义。

---

## 2. 零堆分配惰性格式化代理 (`f"..."`)

为了在不依赖堆分配 `String`、不引入编译器内置格式化魔法宏的前提下支持字符串插值，Meta 库中的 `f` 语法扩展在 `Expand` 阶段将 `f"..."` 改写为**未装箱栈上代理结构体**。

### 2.1 展开协议

```lain
// 用户源码
io.print(f"hello {name}, id: {id}");

// Expand 阶段由 AstOut 事务缓冲原位重写为：
io.print(
  std.fmt.bind((name, id)) ?{ fmt: "hello {}, id: {}" }
);
```

### 2.2 构造步骤

1. **扫描模板**：`f` 展开器扫描字符串字面量内部的 `{...}` 插值槽；
2. **提取并解析子表达式**：截取 `"name"`、`"id"` 文本片断交由 `Reader` 解析为合法的 `RawAst` 子表达式；
3. **构造编译期静态模版**：生成剥离插值符的纯切片模版 `"hello {}, id: {}"`；
4. **通过 `AstOut` 生成代理构造**：生成对 `std.fmt.bind((name, id)) ?{ fmt: ... }` 的调用树并提交事务；
5. **流式降解**：目标写入函数按 `Formattable` 协议单态化后，按模版字面量分段直接向 `std::io` 或内存切片写入，代理元组在优化与栈槽化阶段被彻底抹平，**全程零堆内存申请**。

---

## 3. 模式匹配穷尽性与决策树脱糖 (`match`)

`match` 是基于 `AstOut` 将代数和类型（`enum` / 整数字面量 / 嵌套模式）确定性降解为平坦控制流的展开规则：

### 3.1 源码与语义契约

```lain
let val: i64 = match opt {
  Some(x) => x + 1,
  None    => 0,
};
```

### 3.2 编译管线步骤

1. **构建模式矩阵**：采集全部 `arm` 的构造子标签与绑定变量列表；
2. **穷尽性验证（Exhaustiveness）**：基于枚举形参表确认全部合法构造变体均已覆盖（或末尾存在 `_` / 默认绑定分支），缺漏任意构造变体在编译期立即报错并指出未覆盖标签；
3. **冗余分支告警（Redundancy）**：静态检查是否有分支被前面更宽泛的分支完全遮蔽；
4. **降级生成控制流**：
   * 读取 `enum` 值首偏移处的 Tag 标量；
   * 发射等价于 LAINIR **`#switch`** 的多路分支；
   * 针对带荷载的构造分支，以精确偏移计算从源聚合体取出负载值绑定到局部不可变符号；
   * 将各分支结果汇聚到统一返回临时槽（支持把 `match` 作为带返回值表达式使用）。
