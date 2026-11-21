# Lain-Lang 一等模块系统规范 (modules.md)

Lain-Lang 的物理内核（Seed、Lain-VM 与 LAINIR）不内置任何模块、命名空间或可见性语法。模块系统完全作为**编译期一等实体（`Module`）**在前端编译器与标准库层面实现。在降解为 LAINIR 时，所有模块边界彻底消解拍平，不产生任何运行时对象或虚表开销。

> 说明：全文源码示例遵循现代 Lain 零裸指针规则（使用 `Ref(T)`、`Mut(T)`、`Slice(T)`、`MutSlice(T)`）与统一 `let` 声明语法；仅在第 6 节展示降解后的 LAINIR 物理符号。

---

## 1. 语法形式、文件边界与可见性

### 1.1 一等模块表达式 (`module`)

模块是一个静态词法作用域兼编译期字典，用于封装类型、常量、函数与子模块。所有顶层绑定统一写作 `let`：

```lain
let MyModule = module {
  pub let Elem = u32;

  pub let Header = struct {
    magic: u32,
    len: u64,
  };

  let DEFAULT_MAGIC: u32 = 0x4C41494E; // 未加 pub，模块私有

  pub let init_header = func(len: u64) -> Header {
    return Header { magic: DEFAULT_MAGIC, len: len };
  };
};
```

### 1.2 源文件即隐式 `module`

每个 `.lain` 源文件在语义上等价于一个隐式的顶层 `module { ... }`：
* `let io = import("std::io");` 与文件内的 `let MyModule = module { ... };` 返回完全同构的 `Module` 值；
* 所有未标 `pub` 的辅助类型、辅助函数或底层常量仅在当前源文件（或当前内联 `module`）内部可见。

### 1.3 可见性与多级路径解析 (`pub let` / `a.b.c`)

1. **`pub` 导出契约**：
   * 只有标记了 `pub let`（或 `pub extern`）的成员对外可见；
   * 跨模块访问未导出的私有成员（如 `MyModule.DEFAULT_MAGIC`），编译器在解析点立即报错（`"private symbol"`），严禁泄漏私有实现；
   * 入口函数 `@root let main = func() -> i64 { ... };` 作为宿主链接根导出。
2. **多级路径消解（`A.B.C`）**：
   * 支持任意深度的点号链访问，同时适用于**类型位置**（`A.B.Header`）、**运行期表达式**（`A.B.init_header(10)`、`A.B.Enum.Variant(...)`）与**编译期求值**（`A.B.Factory(T)`）；
   * 若路径中间任意一跳遇到非公开子模块或私有成员，立即报可见性错误。
3. **作用域导入 (`use` / `open`)**：
   * `use M;`：在当前词法作用域内激活模块 `M`（用于其 `pub let Self` 的 UFCS 与 Concept Witness 自动检索，而不污染普通短名称）；
   * `use M.{ Header, init_header };`：选择性将指定公开成员引入当前词法作用域；
   * `open M;`：将模块内全部 `pub` 符号直接引入当前词法作用域。

---

## 2. 核心统一：`module` + `pub let Self`（命名空间 / UFCS / Witness 三位一体）

Lain-Lang 不设独立的 `trait`、`impl` 或 `given` 关键字。当一个模块导出 `pub let Self = T;` 时，这同一个 `module` 同时承担三种角色：

```lain
let Vec2 = struct { x: i64, y: i64 };

// 为 Vec2 定义关联模块：导出 pub let Self = Vec2
let Vec2Mod = module {
  pub let Self = Vec2;

  pub let zero = func() -> Vec2 {
    return Vec2 { x: 0, y: 0 };
  };

  pub let combine = func(a: Vec2, b: Vec2) -> Vec2 {
    return Vec2 { x: a.x + b.x, y: a.y + b.y };
  };

  pub let scale = func(self: Vec2, k: i64) -> Vec2 {
    return Vec2 { x: self.x * k, y: self.y * k };
  };
};
```

> 惯用缩写：也可以直接在模块内部声明结构体 `let Vec2 = module { pub let Self = struct { x: i64, y: i64 }; ... };`。当绑定了 `Self` 的模块被放在类型标注位置（如 `v: Vec2`）或构造器位置（`Vec2 { x: 1, y: 2 }`）时，自动指代 `Vec2.Self`。

上述这一个模块直接打通三套能力：

1. **命名空间调用**：`Vec2Mod.zero()`、`Vec2Mod.combine(a, b)`。
2. **UFCS 方法调用（[desugaring.md](desugaring.md)）**：当 `v` 类型为 `Vec2` 时，展开器在当前词法作用域查找导出 `pub let Self == Vec2` 且含方法 `scale` 的可见模块，将 `v.scale(10)` 原地重写为 `Vec2Mod.scale(v, 10)`。
3. **Concept Witness 自动合成与显式覆盖（[concepts.md](concepts.md)）**：
   * 若函数签名要求 `?{ T: Type, m: Monoid(T) }` 且调用点省略了 `?{...}`：编译器在当前作用域搜索导出 `pub let Self == T` 的模块（如 `Vec2Mod`），检查它是否导出并类型匹配 `Monoid(T)` 所需的全部字段（`zero`、`combine`），命中后自动将函数引用组装为 Witness 字典传入；
   * **打破孤儿规则与局部多实现**：对于 `i64`，可以在各自模块内写任意多份代数实现：
     ```lain
     let IntSum = module {
       pub let Self = i64;
       pub let zero = func() -> i64 { return 0; };
       pub let combine = func(a: i64, b: i64) -> i64 { return a + b; };
     };

     let IntProd = module {
       pub let Self = i64;
       pub let zero = func() -> i64 { return 1; };
       pub let combine = func(a: i64, b: i64) -> i64 { return a * b; };
     };
     ```
     * 在当前作用域写 `use IntSum;`：将 `IntSum` 激活为该作用域内 `i64` 的默认 Witness，调用 `fold_arr(ref_of(nums))` 自动合成加法幺半群；
     * 需要局部切换时无需全局改写，直接显式传入模块：`fold_arr(ref_of(nums)) ?{ m: IntProd }`，编译器从 `IntProd` 提取并校验 `Monoid(i64)` 所需的每个导出项。
   * **歧义与缺失断定**：若同一作用域内存在多个活跃模块同时满足同一类型的 `Self` 与同一 `Concept(T)` 且未显式指定 `?{...}`，编译器报歧义错误；若零个匹配，报缺少 Witness 错误。

---

## 3. 模块签名 (`sig`)

签名（Signature）是模块的静态接口契约，规定实现模块必须公开导出的类型、只读值与函数签名：

```lain
let Serializer = sig {
  type Target;
  serialize(val: Ref(Target), buf: MutSlice(u8)) -> u64;
  deserialize(buf: Slice(u8)) -> Target;
};

// 显式标注模块签名：编译期校验并收窄对外可见成员
let JsonUserSerializer: Serializer = module {
  pub let Target = User;

  let encode_header = func(buf: MutSlice(u8)) -> u64 { ... }; // 私有实现细节

  pub let serialize = func(val: Ref(Target), buf: MutSlice(u8)) -> u64 { ... };
  pub let deserialize = func(buf: Slice(u8)) -> Target { ... };
};
```

* **抽象与具象类型规格**：`sig` 内支持抽象 `type Target;` 以及具象约束 `type Target = User;`；
* **静态核验与接口裁剪**：当模块显式标注 `: Sig` 时，编译器检查模块是否实现了 `Sig` 规定的全部成员与完整签名（包括参数类型、编译期参数 `?{}` 与效应 `!{}`）；不匹配则在模块定义处立即报错。同时，即使模块内部标了额外的 `pub let`，经 `: Sig` 视图向外暴露时仅保留签名中声明的成员。

---

## 4. 函子：参数化模块 (`func(...) -> Module`)

Lain-Lang 不设独立的 `functor` 关键字。**函子就是接收编译期参数并返回 `Module` 的普通纯函数**：

```lain
let Vec = func(T: Type) -> Module {
  return module {
    pub let Self = struct {
      ptr: Mut(T),
      len: u64,
      cap: u64,
    };

    pub let empty = func(backing: Mut(T), cap: u64) -> Self {
      return Self { ptr: backing, len: 0, cap: cap };
    };

    pub let get = func(self: Ref(Self), idx: u64) -> Ref(T) {
      let base: Mut(T) = deref(self).ptr;
      return ref_of(deref(ptr_offset(base, idx)));
    };
  };
};

// 实例化具体模块：Vec(i64) 与 Vec(f64) 生成彼此隔离的独立类型与命名空间
let IntVec = Vec(i64);
let FloatVec = Vec(f64);
```

### 函子记忆化规则 (Memoization)

* 与类型工厂（[concepts.md](concepts.md)）一致，函子遵循纯函数单例缓存规则：以 `(函子符号, 规范化编译期实参元组)` 为键全局记忆化；
* 不同模块调用 `Vec(i64)` 得到的永远是**物理同一份 `Module` 与同一组修饰符号**，使得基于 `IntVec.Self` 的类型检查可以跨编译单元严格互通；
* 不同实参（如 `Vec(i64)` 与 `Vec(u64)`）产出的 `Self` 属于完全独立的 `Type`，严禁混用。

---

## 5. 与类型工厂的分工边界

| 需求 | 选用机制 | 说明 |
| :--- | :--- | :--- |
| 仅生成新的数据布局 / Concept 契约结构 | **类型工厂** `func(T: Type) -> Type` | 返回 `struct { ... }` / `record_end(b)`，例如 `Monoid(T)`、`SoA(T, N)` |
| 同时打包类型布局、方法集与 Witness 接口 | **函子** `func(T: Type) -> Module` | 返回完整 `module { pub let Self = ...; ... }`，例如泛型容器 `Vec(T)`、`Map(K, V)` |

---

## 6. 降解与符号修饰 (Lowering to LAINIR)

模块与函子仅存在于编译期 Meta 阶段，在降解为 LAINIR 时执行彻底扁平化：

1. **过程与静态数据名修饰（Mangling）**：所有嵌套模块与函子实例内的函数统一用唯一前缀展平为顶层全局 `@` 符号：
   ```text
   // 源码调用
   MyModule.SubModule.compute(x)
   // 降解为 LAINIR
   #call @MyModule$SubModule$compute(%x)

   // 函子实例化函数
   Vec(i64).get(v, 0)
   // 降解为 LAINIR
   #call @Vec$i64$get(%v, 0)
   ```
2. **结构体与布局消解**：模块内部定义的结构体在降解时完全抹除字段名与模块路径，字段访问直接变为基于结构物理偏移的 `#lea` / `#load` / `#store`。
3. **零运行时开销**：模块字典本身、`pub` 标志与 `: Sig` 核验结果在通过 Meta 检查后全部消融，不在最终机器码或 IR 中生成任何运行期 `Module` 对象或方法表。
