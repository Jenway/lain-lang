# Lain-Meta 规范

> [!QUOTE]
> Meta 究竟是个什么样子，C Macro 搞了很多年，也并没有完全搞清楚。
> 
> 可能 CPP 的思路比较好，搞了个静态反射，但是 std::meta 完整进标准不知道要哪一年。

---

## 0. 架构定位与核心-标准库硬边界

Lain-Meta 确立了“微内核编译器 + 库化语言特性”的架构。整个编译系统的物理层级划分为三层：

1. **宿主底座（Driver / Seed）**：提供零关键字词法切分器（`Reader`）、构建调度与目标文件输出；
2. **执行引擎（Lain-VM `kernel/`）**：无状态的底层抽象机，提供结构化 `LAINIR` 验证、优化、沙箱解释执行（`VSpace`、`CSpace`、`Sched::apply`）与 C99 后端；
3. **元编译器（Lain-Meta `frontend/`）**：源语言前端，包含语法展开、解析、语义细化与 IR 降级，并在编译期直接复用 `kernel::vm::Sched::apply` 执行宏与编译期函数。

### 0.1 统一声明体系（7 种一等构造）

所有顶层与局部声明统一为 `let Name = <构造>;`（导出加 `pub`），**彻底移除 `comptime` 关键字**：

```lain
let S = struct { ... };                 // 1. 积类型（结构体）
let E = enum { ... };                   // 2. 和类型（Tagged Union 枚举）
let M = module { ... };                 // 3. 模块（命名空间 + 关联类型 Self）
let C = sig { ... };                    // 4. 概念契约（约束 Module 的接口签名）
let F = effect { ... };                 // 5. 代数效应声明
let f = func(...) ?{...} !{...} -> R {};// 6. 函数（编译期 / 运行时双栖）
let m = macro(...) !{...} -> Syntax {}; // 7. 语法宏（仅在 Expand 阶段操作 Syntax）
```

### 0.2 两大元能力的严格物理分治

| 维度 | 能力一：编译期 AST 操作（`macro` + `std::syntax`） | 能力二：双栖 `func` 与静态反射（`func` + `std::reflect`） |
| :--- | :--- | :--- |
| **载体** | `let m = macro(...)` | 普通 `let f = func(...)` |
| **所在阶段** | **Expand 阶段**（在 `Parser` 和 `Sema` **之前**） | **Sema / Elaborate 阶段**（在 `Parser` **之后**） |
| **可见信息** | 仅可见 Token 与括号树（`RawAst` / `Syntax`），**无类型、无布局、无符号表** | 拥有完整强类型、作用域与内存布局（`Type`、`TypeInfo`、`@field_ptr`） |
| **执行引擎** | 编译为普通 `Proc`，通过 `kernel::vm::Sched::apply` 执行 | 编译为普通 `Proc`，通过 `kernel::vm::Sched::apply` 执行（**无 AST 解释器**） |
| **典型用途** | `f"..."` 插值、`unless` / `repeat` / `for`、`assert_eq`、DSL | `Vec(T)` 泛型模块、`size_of(T)`、递归 `drop`、`struct_eq`、常量折叠 |

---

## 1. 零关键字 Reader、`RawAst` 与事务化 `AstOut`

### 1.1 Reader 文法模型

`Reader` 不设任何预留关键字、类型名或语句结构语法，仅按纯形式文法将源码切分为嵌套的括号树：

```ebnf
Form       ::= Token | Group
Group      ::= '(' Form* ')'
             | '[' Form* ']'
             | '{' Form* '}'
Token      ::= Ident | IntLit | FloatLit | StrLit | CharLit | Punct
```

### 1.2 `RawAst` 的平坦 `NodeId` 布局

`RawAst` 将节点线性写入连续数组（`Vec(RawNode)`、`Vec(Span)`、`Vec(NodeId)`），严禁包含裸内存地址，全部使用相对下标 `NodeId` 寻址；宿主通过 `VSpace::map_external` 将其以只读属性映射至执行沙箱。

### 1.3 `AstOut` 游标事务机制（`mark` / `commit` / `rollback`）

`AstOut` 是单向追加的语法树构建缓冲区，提供 $O(1)$ 游标事务：

- `mark() -> u32`：记录当前节点与子列表水位检查点；
- `commit(tx: u32)`：保留新增节点；
- `rollback(tx: u32)`：游标复位，废弃节点立即失效且无内存泄漏。

---

## 2. 能力一：语法宏系统（`macro`、`Syntax` 与 `quote`）

### 2.1 `Syntax` 句柄与对齐签名

在宏参数中，实参可能是一个标识符、一串中缀表达式（如 `a + 1`）或一个代码块。因此 `Syntax` 定义为连续节点的轻量切片句柄：

```lain
// std/syntax.lain
pub let Syntax = struct {
  start: u32, // 在 RawAst.children 中的起始下标
  len: u32    // 连续节点的个数
};
```

`macro` 的参数列表与调用点实参 **1:1 对齐**，底层由编译器隐式注入 `?{ cx: SyntaxCtx }`，**彻底隐藏 `RawAst` / `AstOut` 管道参数**：

```lain
let assert_eq = macro(lhs: Syntax, rhs: Syntax) -> Syntax { ... };
```

### 2.2 三种触发形态

当 `expand` 扫描到绑定为 `macro` 的标识符 `m` 时，按以下规则切分实参：

1. **括号调用 `m(arg0, arg1, ...)`**：将 `Paren` 组内子节点按顶层逗号 `,` 切分为 $N$ 个 `Syntax`；
2. **紧邻字面量调用 `m"..."`**（如 `f"hello {x}"`）：将紧贴的字符串字面量作为 1 个 `Syntax` 传入单参宏；
3. **块调用 `m { ... }`**（或 `m(args...) { ... }`）：将尾部 `Brace` 组作为最后一个 `Syntax` 传入。

### 2.3 `quote { ... $x ... }` 准引用脱糖

`quote { ... }` 是宏体内的准引用语法糖，在编译该 `macro` 时直接降级为对隐式 `cx: SyntaxCtx` 的 `AstOut` 追加调用：

- 普通 Token 与括号原样发射（`cx::emit_*`、`cx::begin_group` / `cx::end_group`）；
- `$ident` 将对应的 `Syntax` 片段（或 `Slice(u8)` 字符串）原样拼接进 `AstOut`（`cx::splice`）；
- 返回 `Syntax` 值交由 `expand` 替换原调用点；若宏返回 `Option(Syntax)::none()` 或失败，触发 `AstOut::rollback`。

### 2.4 宏的沙箱执行与库化清单

宏本身经由 `sema -> lower -> kernel::ir` 编译为普通 `Proc`，在 `expand` 阶段通过 `kernel::vm::Sched::apply` 执行。以下特性**全部移出编译器内核，由标准库以 `macro` 实现**：

- `f"..."` 字符串插值（`std::fmt`）
- `unless`、`repeat`、`for` 等控制流语法糖（`std::macros`）
- `assert` / `assert_eq` 等诊断宏（`std::debug`）

```lain
// 示例：std/debug.lain
pub let assert_eq = macro(lhs: Syntax, rhs: Syntax) -> Syntax {
  let lhs_src: Slice(u8) = lhs.text();
  let rhs_src: Slice(u8) = rhs.text();
  return quote {
    if ($lhs) != ($rhs) {
      io::print("assertion failed: ");
      io::print($lhs_src);
      io::print(" != ");
      io::println($rhs_src);
      os::abort();
    }
  };
};
```

---

## 3. 能力二：无 `comptime` 关键字的双栖 `func` 与静态反射

### 3.1 双栖求值铁律（废除 AST 解释器）

所有普通 `func` 默认既可在运行时执行，也可在编译期由 `kernel::vm::Sched::apply` 执行。**编译器前端严禁另写一套 AST 解释器**；任何编译期求值统一走 `sema -> lower -> kernel::ir -> kernel::vm::Sched::apply`。

### 3.2 四类编译期触发上下文

`sema` 仅依据调用点上下文与参数元类型自动触发编译期求值：

1. **类型 / 模块 / 契约位置**：如 `let V = Vec(u64);`、`Array(u8, calc_cap(4))`、`?{ al: MyAlloc() }`；
2. **包含元类型参数（`Type` / `Module` / `Sig`）**：如 `size_of(Pt)`、`struct_eq(a, b)`，按 `(fn_id, 编译期实参)` 查单态化缓存 `MonoTable`；
3. **顶层常量定义**：如 `let PageSize: u64 = align_up(3000, 4096);`；
4. **普通 `if` 的常量条件折叠**：当 `if cond { ... } else { ... }` 的 `cond` 不依赖任何运行期局部变量时，直接通过 `Sched::apply` 求值并裁剪死分支（不再需要 `comptime if`）。

### 3.3 静态反射最小原语（仅 2 个）

编译器只内置 2 个以 `@` 开头的底层反射原语，全部由 `std::reflect` 封装为普通 `func` 对外提供：

1. **`@type_info(T: Type) -> TypeInfo`**：在 `sema` 阶段从 `HirModule` 类型表生成编译期常量描述：
   - 字段包含 `name: Slice(u8)`、`size: u64`、`align: u64`、`has_drop: bool`、`kind: TypeKind`（`Void`、`Bool`、`Int(bits, signed)`、`Float(bits)`、`Ptr(mutable, Type)`、`Struct(Slice(FieldInfo))`、`Enum(Slice(VariantInfo))`）。
2. **`@field_ptr(base: Ref/Mut(T), idx: u32) -> Ref/Mut(FieldTy)`**：要求 `idx` 在单态化/折叠后为编译期已知整数，直接返回第 `idx` 个字段的指针类型，并在 `lower` 阶段降级为单条 `Op::Lea(base, 0, 1, field.offset)`。

```lain
// 示例：std/reflect.lain —— 零样板代码的通用库能力
pub {
  let size_of = func(T: Type) -> u64 {
    return @type_info(T).size;
  };

  let align_of = func(T: Type) -> u64 {
    return @type_info(T).align;
  };

  let drop_in_place = func(p: Mut(T)) ?{ T: Type } {
    let info: TypeInfo = @type_info(T);
    if info.has_drop {
      T::drop(p);
    }
    match info.kind {
      Struct(fields) => {
        let i: u32 = 0;
        while i < (fields.len as u32) {
          drop_in_place(@field_ptr(p, i));
          i = i + 1;
        };
      },
      _ => {}
    };
  };
};
```

---

## 4. 编译器核心内置类型与内存原语最小集

### 4.1 内置类型（不可约集合）

- **标量与底类型**：`bool`、`u8..u64`、`i8..i64`、`f32..f64`、`()`、`Never`
- **单指针**：`Ref(T)`、`Mut(T)`
- **复合与元类型**：`struct`、`enum`、`func`、`Type`、`Module`、`Sig`
- **移出编译器**：`Slice(T)` 与 `MutSlice(T)` 不再作为编译器内置类型，回归 `std::mem` 中的普通泛型结构体 `struct { ptr: Ref(T), len: u64 }`。

### 4.2 内存与生命周期原语（仅 6 个，1:1 对应 IR）

| 原语 | 语义与 IR 映射 | 收归 `std::mem` 的原内置函数 |
| :--- | :--- | :--- |
| `ref_of(x)` / `mut_of(x)` / `deref(p)` | 取地址与解引用（`Op::Load` / `Op::Store`） | `ptr_of`, `addr_of`, 裸 `load`/`store` |
| `@ptr_offset(p, count)` | 类型化指针偏移（`Op::Lea`） | 裸 `lea` |
| `@ptr_cast(DstPtrTy, src)` | 指针/整数强转（`Op::Bitcast`） | `null_ref`, `is_null` |
| `reloc(x)` | 标记局部变量所有权已转移（置 moved 位） | — |
| `init_at(dst, val)` | 向未初始化槽位写入（不触发旧值 `drop`） | `init_zero` |
| `drop_at(dst)` | 显式触发槽位析构 | — |

### 4.3 宿主上下文 Witness 化

`target_os`、`is_windows`、`is_linux`、`is_macos`、`is_portable_c99` 等平台查询不再作为全局内置函数，统一通过 `std::target` 的默认 `?{ target: Target = HostTarget }` witness 提供。
