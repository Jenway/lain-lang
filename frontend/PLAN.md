# Lain-Lang Frontend 架构与实现计划

## 1. 定位与硬边界

`frontend/` 是 Lain 源语言的完整前端实现，负责将 `.lain` 源码经由严格单向的五层管线转化为通过 `kernel::ir::builder` 构造的合法 `IrModule`。完整元编程与核心-库边界规范见 [docs/03-lain-meta.md](file:///D:/codeFiles/lain-lang/docs/03-lain-meta.md)。

- **负责**：
  1. 零关键字词法切分与事务化语法树构建（`syntax/`）；
  2. 能力一（AST 操作）：基于 `let Name = macro(...)`、`Syntax` 与 `quote` 的宏展开（`expand/`）；
  3. 强类型表面语法树与解析（`ast/`）；
  4. 能力二（双栖 `func` 与静态反射）：模块解析、`@type_info` / `@field_ptr` 静态反射、类型推导、`?{}` Witness、`!{}` 效应检查与基于 `kernel::vm::Sched::apply` 的编译期求值（`sema/`）；
  5. 降级与发射：单态化、效应降级、值语义 `reloc`/RAII `drop`、调用 `kernel::ir::builder` 构造 LAINIR（`lower/`）。
- **明确排除（Non-Goals）**：
  - **严禁在前端另写 AST 解释器**：所有编译期 `func` 与 `macro` 求值统一走 `lower -> kernel::vm::Sched::apply`；
  - **严禁把库特性硬编码进编译器**：`Slice(T)` / `MutSlice(T)`、`f"..."` 插值、`for`/`unless`/`assert_eq`、`size_of`/`align_of`、`null_ref`/`is_null` 全部由 `std/` 实现；
  - **严禁直接触碰 `IrModule` 内部存储或 `Inst` 编码细节**（`lower/` 只消费 `kernel::ir::builder`）；
  - 不做 IR 校验、IR 优化、VM 执行或 C99 代码生成（全部归 `kernel/`）；
  - 不维护全局可变状态池（`SourceMap`、增量缓存、诊断渲染、CLI 全部归 `driver/`）。

---

## 2. 目录结构与严格单向依赖（DAG）

```text
frontend/
├── PLAN.md
├── syntax/                # 层 1：零关键字 Reader、只读 RawAst、事务化 AstOut（仅依赖 std）
│   ├── span.lain          # SourceId / Span（含 Span::slice，统一源码切片）
│   ├── raw_ast.lain       # Delim / RawNode / RawAst（平坦 Vec Arena）
│   ├── reader.lain        # Slice(u8) -> RawAst（零关键字，纯 while 迭代）
│   └── ast_out.lain       # 游标事务构建器（mark / commit / rollback）
├── expand/                # 层 2：能力一 —— AST 变换（依赖 std + syntax + kernel::vm）
│   ├── env.lain           # 语法绑定表（Macro 符号 vs Core Forms）
│   └── expand.lain        # 宏展开循环：切分 Syntax 实参 -> Sched::apply 执行 macro -> AstOut 事务提交
├── ast/                   # 层 3：硬分水岭 —— 强类型 AST + Parser（依赖 std + syntax）
│   ├── model.lain         # 强类型 tagged union：Decl / Stmt / Expr / Pattern（无 ComptimeIf / Try）
│   └── parser.lain        # Core RawAst -> Ast（:: 与 . 分离、foo().bar 链式、quote 脱糖、零深递归）
├── sema/                  # 层 4：能力二 —— 语义分析与静态反射（依赖 std + ast + kernel::ir::builder + kernel::vm）
│   ├── hir.lain           # 不可变 TypedHir + Type / Module / Effect / FieldInfo 模型
│   ├── resolve.lain       # 模块图、pub { } 可见性、use import("...")::{ }、词法作用域
│   ├── comptime.lain      # 双栖 func 编译期求值桥接（lower -> kernel::vm::Sched::apply）
│   ├── types.lain         # 类型推导、@type_info/@field_ptr、sig 校验、?{} Witness 合成、普通 if 常量折叠
│   └── effects.lain       # !{} 效应行推导、默认纯度校验、perform / handle 检查
└── lower/                 # 层 5：降级管线（依赖 std + sema + kernel::ir::builder）
    ├── mono.lain          # 泛型函数与函子单态化
    ├── eff_lower.lain     # 单次代数效应降级（中止性 -> 状态码 + #switch；尾恢复）
    ├── drop.lain          # 值语义 reloc 检查与 RAII drop 插入
    └── emit.lain          # 物理布局计算、短路 and/or、通过 IrBuilder/ProcBuilder 构造 LAINIR
```

### 依赖红线
- `syntax/` 只依赖 `std/`；
- `ast/` 只依赖 `std/` + `syntax/`，**从 `ast/` 往下严禁再出现 `RawNode` / `AstOut`**；
- `sema/` 不感知 `LAINIR` 物理布局与指令流（编译期求值通过 `lower + builder + vm::sched::apply` 运行临时沙箱）；
- `lower/` 不感知 `RawAst` 与表面语法，**只通过 `kernel::ir::builder` 产出 IR**。

---

## 3. 双阶段 Meta 的正交分工

```text
RawAst ──► [expand: macro + Syntax + quote] ──► Core RawAst ──► [ast: Parser] ──► [sema: 双栖 func + @type_info/@field_ptr] ──► TypedHir
```

1. **能力一：AST 变换（`frontend/expand`）**
   - 运行在类型检查之前，操作对象为 `Syntax`（`RawAst` 连续切片）与事务化 `AstOut`；
   - `let m = macro(a: Syntax, ...) -> Syntax` 隐藏底层管道参数，支持 `m(...)`、`m"..."`、`m { ... }` 与 `quote { ... $x ... }`；
   - 宏本身经 `sema -> lower` 编译为 `Proc`，在 `expand` 阶段交由 `kernel::vm::Sched::apply` 执行；`f"..."`、`unless`、`for`、`assert_eq` 等全部作为标准库宏实现。
2. **能力二：双栖 `func` 与静态反射（`frontend/sema`）**
   - 彻底取消 `comptime` 关键字：普通 `func` 在类型/模块位置、元类型传参（`Type`/`Module`/`Sig`）、顶层常量或纯常量 `if` 条件时，自动通过 `lower -> kernel::vm::Sched::apply` 求值；
   - 编译器仅提供 `@type_info(T)` 与 `@field_ptr(base, idx)` 两个反射原语，`size_of`、`align_of`、递归 `drop`、`struct_eq` 全部收归 `std::reflect`。

---

## 4. 彻底根治旧 `src/` 痛点的工程铁律

1. **零 $O(N)$ 栈深递归**：顶层声明扫描、块内语句解析、表达式后缀链、符号查找全部采用 `while` 循环；调用栈深度严格受限于语法嵌套深度（通常 < 30 层）。
2. **核心-库极简边界**：
   - 内置类型仅保留标量、`Ref(T)`/`Mut(T)`、`struct`/`enum`/`func`/`Type`/`Module`/`Sig`（`Slice`/`MutSlice` 归 `std::mem`）；
   - 内存与生命周期内置原语仅保留 6 个：`ref_of`/`mut_of`/`deref`、`@ptr_offset`、`@ptr_cast`、`reloc`、`init_at`、`drop_at`；
   - `?{}` 带值静态能力与自动透传，零虚表开销且消除 `?{ al: al }` 样板。
3. **现代 Lain 代码卫生**：
   - 错误传播统一使用 `std::err::Fail(E)`，fail fast and clear；
   - 全量使用 `Vec(T)` / `Option(T)` / tagged union `enum` / `module { pub { let Self = ...; }; }`；
   - 零裸 `u64` 句柄与魔数、零 `ptr_cast` 动态数组、零 1 行转发壳、零注释、`lain_tidy` 零告警。

---

## 5. 分阶段实施里程碑

- **Phase F1（`syntax/` + `ast/`）**：已落地零关键字 `Reader`、`RawAst`、`AstOut`、强类型 `Ast` 与 `Parser`；下一步清理冗余 `Expr::Try` / `ComptimeIf` 并补齐 `AstOut` 事务与 `Span::slice`。
- **Phase F2（`expand/`）**：落地 `let Name = macro(...)`、`Syntax` 切分、`quote` 脱糖与 `Sched::apply` 宏执行，将 `f"..."` 移出编译器。
- **Phase F3（`sema/`）**：砍掉 AST 解释器，接通双栖 `func -> lower -> Sched::apply`，落地 `enum`/`match`、模块系统、`@type_info`/`@field_ptr`、`?{}` Witness 与 `!{}` 效应检查。
- **Phase F4（`lower/`）**：落地单态化、效应降级、`reloc`/RAII `drop` 插入，经由 `kernel::ir::builder` 接通 `kernel/ir/verify -> opt -> vm / backend/c99`。
- **Phase F5（合龙与清退旧 `src/`）**：接入新 `driver/`，通过全量自举不动点（`stage2 == stage3`）与全套回归后，整目录删除旧 `src/`。
