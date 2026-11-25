# Lain-Lang Frontend 架构与实现计划

## 1. 定位与硬边界

`frontend/` 是 Lain 源语言的完整前端实现，负责将 `.lain` 源码经由严格单向的五层管线转化为通过 `kernel::ir::builder` 构造的合法 `IrModule`。

- **负责**：
  1. 零关键字词法切分与事务化语法树构建（`syntax/`）；
  2. 语法层元编程：宏展开与表面语法脱糖（`expand/`）；
  3. 强类型表面语法树与解析（`ast/`）；
  4. 语义层元编程与静态检查：模块解析、静态反射、`comptime`、类型推导、`?{}` Witness、`!{}` 效应检查（`sema/`）；
  5. 降级与发射：单态化、效应降级、值语义 `reloc`/RAII `drop`、调用 `kernel::ir::builder` 构造 LAINIR（`lower/`）。
- **明确排除（Non-Goals）**：
  - **严禁直接触碰 `IrModule` 内部存储或 `Inst` 编码细节**（`lower/` 只消费 `kernel::ir::builder` 提供的 `IrBuilder` / `ProcBuilder`）；
  - 不做 IR 校验、IR 优化、VM 执行或 C99 代码生成（全部归 `kernel/`）；
  - 不维护全局可变状态池（`SourceMap`、增量缓存、诊断渲染、CLI 全部归 `driver/`）。

---

## 2. 目录结构与严格单向依赖（DAG）

```text
frontend/
├── PLAN.md
├── syntax/                # 层 1：零关键字 Reader、只读 RawAst、事务化 AstOut（仅依赖 std）
│   ├── span.lain          # SourceId / Span（字节偏移、行列定位）
│   ├── raw_ast.lain       # Delim / RawNode / RawAst（平坦 Vec Arena）
│   ├── reader.lain        # Slice(u8) -> RawAst（零关键字，纯 while 迭代）
│   └── ast_out.lain       # 游标事务构建器（mark / commit / rollback）
├── expand/                # 层 2：能力一 —— AST 变换（依赖 std + syntax + kernel::vm）
│   ├── env.lain           # 语法绑定表（Macro 规则 vs Core Forms）
│   ├── sugar.lain         # 内置表面语法脱糖（f"..." 等）
│   └── expand.lain        # 宏展开循环：挂载只读 RawAst + 注入 AstOut CSpace -> Sched::apply
├── ast/                   # 层 3：硬分水岭 —— 强类型 AST + Parser（依赖 std + syntax）
│   ├── model.lain         # 强类型 tagged union：Decl / Stmt / Expr / Pattern / TyExpr / EffExpr
│   └── parser.lain        # Core RawAst -> Ast（:: 与 . 分离、foo().bar 链式、完整优先级、零深递归）
├── sema/                  # 层 4：能力二 —— 语义分析与静态反射（依赖 std + ast + kernel::ir::builder + kernel::vm）
│   ├── hir.lain           # 不可变 TypedHir + Type / Module / Effect / FieldInfo 模型
│   ├── resolve.lain       # 模块图、pub { } 可见性、use import("...")::{ }、词法作用域
│   ├── comptime.lain      # 静态反射（fields_of / comptime for / comptime if / 函子 / eval->apply）
│   ├── types.lain         # 类型推导、sig 校验、?{} Witness 合成、匿名函子默认 UFCS、enum 结构 ==
│   └── effects.lain       # !{} 效应行推导、默认纯度校验、perform / handle 检查
└── lower/                 # 层 5：降级管线（依赖 std + sema + kernel::ir::builder）
    ├── mono.lain          # 泛型函数与函子单态化
    ├── eff_lower.lain     # 单次代数效应降级（中止性 -> 状态码 + #switch；尾恢复）
    ├── drop.lain          # 值语义 reloc 检查与结构体字段自动递归 RAII drop 插入
    └── emit.lain          # 物理布局计算、短路 and/or、通过 IrBuilder/ProcBuilder 构造 LAINIR
```

### 依赖红线
- `syntax/` 只依赖 `std/`；
- `ast/` 只依赖 `std/` + `syntax/`，**从 `ast/` 往下严禁再出现 `RawNode` / `AstOut`**；
- `sema/` 不感知 `LAINIR` 物理布局与指令流（仅 `comptime` 求值时通过 `builder` + `vm::sched::apply` 运行临时沙箱）；
- `lower/` 不感知 `RawAst` 与表面语法，**只通过 `kernel::ir::builder` 产出 IR**。

---

## 3. 双阶段 Meta 的正交分工

```text
RawAst ──► [expand: 语法层 AST 变换] ──► Core RawAst ──► [ast: Parser] ──► [sema: 语义层静态反射] ──► TypedHir
```

1. **能力一：AST 变换（`frontend/expand`）**
   - 运行在类型检查之前，操作对象为无语义的 `RawAst` 与事务化 `AstOut`；
   - 执行宏时通过 `VSpace::map_external` 零拷贝挂载只读 `RawAst`，通过 `CSpace` 仅授予 `AstOut` 追加/回滚能力，交给 `kernel::vm::sched::apply` 求值；
   - 负责扩展表面语法与脱糖，收敛为纯 `Core RawAst` 后交出控制权。
2. **能力二：静态反射与 `comptime`（`frontend/sema/comptime`）**
   - 运行在强类型信息完备之时，操作对象为一等 `Type`、`Module`、`Effect`、`FieldInfo`；
   - 负责 `comptime if`、`comptime for` 遍历字段、泛型函子（`Vec(T)`、`Option(T)`、`Fail(E)`）、声明式类型构造与编译期常量计算；
   - 复杂编译期计算降级为临时 `IrModule`，交给**空 `CSpace`（零外部副作用）**的 `kernel::vm::sched::apply` 求值，保证 100% 确定性构建。

---

## 4. 彻底根治旧 `src/` 痛点的工程铁律

1. **零 $O(N)$ 栈深递归**：顶层声明扫描、块内语句解析、表达式后缀链、符号查找全部采用 `while` 循环；调用栈深度严格受限于语法嵌套深度（通常 < 30 层），彻底告别旧 `src/` 动辄吃爆 8MB 默认栈的问题。
2. **现代 Lain 语法一次做正**：
   - 短路求值 `and` / `or`；
   - 任意后缀链式访问（`foo().bar`、`arr[i].field`、`v.get_mut(i).x = 1`）；
   - 静态路径 `::` 与运行期访问/UFCS `.` 清晰分离；
   - `pub { ... };`、`use import("...")::{ A, B };`、泛型模块在类型位置自动解包 `Self`、匿名函子默认 UFCS；
   - **`?{}` 带值静态能力与自动透传**：`?{ al: Allocator = SystemAlloc }` 同时绑定静态实现与可选运行期状态（ZST 时编译期完全擦除、零寄存器占用），支持 `with ?{ al: expr } { ... }` 作用域绑定，并在调用同名 `?{ al }` 的函数或类型构造（`RawAst`、`AstOut`、`Vec`）时自动透传，零虚表开销且消除 `?{ al: al }` 样板；
   - 值语义 tagged union `enum` 原生结构 `==` / `!=` 与无崩溃嵌套 `match`；
   - 结构体嵌套 `!Copy` 字段自动递归 RAII `drop`。
3. **现代 Lain 代码卫生**：
   - 错误传播统一使用 `std::err::Fail(E)`，fail fast and clear；
   - 全量使用 `Vec(T)` / `Option(T)` / tagged union `enum` / `module { pub { let Self = ...; }; }`；
   - 零裸 `u64` 句柄与魔数、零 `ptr_cast` 动态数组、零 1 行转发壳、零注释、`lain_tidy` 零告警。

---

## 5. 分阶段实施里程碑

- **Phase F1（`syntax/` + `ast/`）**：落地零关键字 `Reader`、`RawAst`、`AstOut`、强类型 `Ast` 与 `Parser`；第一时间可供 `tools/lainfmt` 与 `tools/lain_tidy` 复用。
- **Phase F2（`expand/`）**：接入语法绑定表、`f"..."` 脱糖与基于 `kernel::vm::sched::apply` + `AstOut` 事务的宏展开。
- **Phase F3（`sema/`）**：落地模块图与可见性、类型推导、`?{}` Witness、`!{}` 效应检查与 `comptime` 静态反射，产出不可变 `TypedHir`。
- **Phase F4（`lower/`）**：落地单态化、效应降级、`reloc`/RAII `drop` 插入，经由 `kernel::ir::builder` 接通 `kernel/ir/verify -> opt -> vm / backend/c99`。
- **Phase F5（合龙与清退旧 `src/`）**：接入新 `driver/`，通过全量自举不动点（`stage2 == stage3`）与全套回归后，整目录删除旧 `src/`。
