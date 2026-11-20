# Lain-Lang 工程推进计划（PLAN.md）

> **核心原则**：物理内核保持极简（`LAINIR` + `Lain-VM` + 零关键字 `Reader` + `AstOut`），高级特性与类型系统由 Meta 库和结构化原语逐步承载；任何阶段必须由真实 VM/原生执行断言与负测试看护，严控单文件体量与隐藏状态，小步提交、绝不失控。

---

## 0. 当前仓库基线与架构红线

### 0.1 现存代码盘面（已验证现状）
- **`seed/kernel/`（C 底座）**：
  - `ir/`（`core.h`, `build.c`, `verify.c`, `infer.c`）与 `text/`（`parse.c`, `print.c`, `opname.c`）：结构化 SSA 构建、类型推导与校验。
  - `vm/`（`engine.c`, `image.c`, `space.c`, `tcb.c`, `caps.c`, `quota.c`, `trap.c`）：单线程沙箱虚拟机，支持直接与间接 `#call [(ParamTypes) -> RetType](%fn_ptr, ...)`（注：向量仅实现 6 个整数车道算子，其余浮点向量/原子为 STUB）。
  - `meta/`（`tree.c`, `sym.c`, `ast.h`, `ast_out.c`, `apply.c`, `host_v2.c`）：零关键字括号树切分、一次 `SymId` 驻留、`AstOut` 事务构造、隔离 `apply`、以及基于不透明 `u32` 句柄的 50+ 项 `lain2_*` 宿主能力。
- **`seed/lain0/`（C++20 Stage-0 引导器）**：
  - 负责将 `src/` 编译为 `.l1` 单元；**原则上冻结**，仅修阻断 `src/` 自身的严重 Bug，不同步新增高级语言特性；`src/` 自身源码须保持在 `lain0` 支持的子集内（例如暂不在 `src/` 源码内写 `\"` 转义或 `Ref(T)`）。
- **`seed/` 工具与驱动**：
  - `meta_v2_driver.c`（当前默认参数：`fuel=2e9`, `slice=5e7`, `stack=4 MiB`, `quota=512 MiB`, `workspace=256 MiB`, `depth=1024`, `symbols=131072`, `symbol-bytes=8 MiB`）、`lainir_verify.c`、`lainir_run.c`。
- **`src/`（Meta 编译器本体，21 文件共 ~5901 行）**：
  - 核心链路：`entry(35) -> eval(519) -> decl(219) / anon(83) -> core(1646) -> ctx(436) -> agg(380) / builtins(355) / modules(194) / lval(135) / comptime(35) / reflect(204) -> types(216) / tyreg(293) / ctypes(231) / env(49) / ir(206) / ast(138) -> state(295) / sym(97) / ir_codes(135)`。
- **当前测试门禁（`scripts/verify_all.py`）**：
  - `examples/*.lain`：**38/38** 编译 + `lainir-verify` + 与 `lain0` 逐条差分一致，**111/111** `Lain-VM` 真值用例（覆盖 37 个样例；`extern` 因依赖宿主能力不运行）；
  - `seed/tests/`：**6/6** IR 间接 `#call` 与 Trap 测试；
  - `tests/meta/`：**28/28**（含 `eval`、反射、`inject`、`icall`、`Ref`/`Mut`/`Slice`/`MutSlice`/`Array`/`Fn`、`lval`、`sret`、`let = func`、匿名无捕获过程及负例）。

### 0.2 开发铁律（防失控硬约束）
1. **门禁即红线**：每步结束必须运行 `python scripts/verify_all.py` 并保持全绿；新特性必须同时配齐**正例（VM 真实执行断言值 + 产物结构检查）**与**负例（期望编译报错、产物 0 字节、不触发意外 VM Trap）**。
2. **文件体量与依赖 DAG**：
   - `src/` 必须维持严格单向无环（DAG）；
   - 新增 `.lain` 文件单文件 **≤ 350 行**；禁止继续向 `src/core.lain` 堆砌新功能，Phase 1 需将其拆瘦至 **≤ 700 行**。
3. **三禁止**：禁止按字节重扫源文本（必须用 `AstId` + `SymId`）、禁止在 Meta 库中手拼 `LAINIR` 文本（必须用 `IRBuilder`）、禁止新增跨模块隐式魔法偏移。
4. **小步存档**：每个 Phase（或独立子步）全绿后立即 `git commit`。

---

## Phase 1：基础语言体感抛光与 `core.lain` 拆瘦

**目标**：消除当前 `.lain` 日常写法中的冗余噪音与半成品细节，并解决 `core.lain`（1646 行）过胖及驱动默认资源上限过高的问题。

### 1.1 局部 `let` 类型推断
- **语法**：支持 `let x = expr;` 与原有的 `let x: Type = expr;`。
- **语义与边界规则**：
  - 解析 `stmt_let`（及顶层 `p2_let`）时若未见 `:`，传 `want = tnone` 给表达式求值，采用右侧推导出的类型 `vty`。
  - **字面量默认规则**：裸整数字面量在无预期类型时维持现有 `core.lain` 规则（默认 `i32`），若无法确定类型必须报 `e_type` 编译错误。
  - **字符串字面量推断边界（关键兼容）**：
    - `examples/bytes.lain` / `strinline.lain` 中存在显式标注 `let g: addr = "hi";`，此时 `want` 为 `addr`，保留其兼容降级；
    - 当写 `let s = "hi";`（无 `: addr` 标注）时，字符串字面量应自然推导为 **`Slice(u8)`**（直接支持 `s.len` 和 `s[i]`）。

### 1.2 `void` 语句调用、忽略返回值与独立 `loop` 语句
- **调用语句**：
  - 允许 `-> void` 过程与普通有返回值过程在语句位置直接调用（如 `set_y(r, 9);`），不再强制绑定 `let ok = ...`。
  - **关键边界（`sret` 返回值被忽略时）**：若语句位置调用了一个返回聚合体（走 `sret`）的过程，调用方仍必须在栈帧上为其分配一块临时 `#alloca` 传给 `%_ret` 首参，避免被调方往非法/空地址写数据。
- **独立 `loop` 语句**：
  - 允许 `loop it(i: u64 = 0) while i < n { ... };` 直接在 `stmt_mode` 作为语句运行而不绑定到 `let r = loop ...`（结果槽传 `no_slot()`）。

### 1.3 `Array(T, N)` 下标直接返回元素值
- **兼容区分**：
  - 保留顶层老式静态表（`let t: [u64] = ...;` / `ro [u8]`）下标返回 `#addr` 的行为，确保 `examples/table.lain` 与 `modules_main.lain` 100% 不退化；
  - 所有使用 `Array(T, N)` 声明的数组（局部、参数、结构体字段 `deref(s).x[idx]`），在右值表达式位置 `arr[idx]` **直接返回元素 `T` 的值**（标量发 `#lea` + `#load`；若 `T` 为嵌套聚合则返回指向第 `idx` 项的带形状地址视图，支持 `arr[idx].x`）。左值位置 `arr[idx] = v` 继续由 `src/lval.lain` 处理。
  - 同步清理 `tests/meta/soa_clean.lain` 等测试中多余的 `load(deref(s).x[idx])` 外层 `load`。

### 1.4 过程名直接作为 `Fn(...)` 值
- **规则**：当 `var_ref` 遇到一个标识符，未在局部 `env` 命中、后不跟 `(`、且在模块表中查到为 `func` 或 `let = func` 或 `extern` 时：
  - 直接根据其已知签名（需确保 `phase1` 扫描阶段已把签名挂到模块符号条目的 `sig` 槽位）通过 `ctypes.fn_t` 构造 `Fn((ParamTys...) -> RetTy)` 类型，发射过程地址引用（`#bitcast[#addr](@qual_name)`）。
  - 于是 `VT { area: circle_area, scale: circle_scale }` 与 `apply(triple, 7)` 均可直接写过程名，彻底消除 `symbol_addr("...")` 字符串绕路（旧 `symbol_addr("...")` 保留作 `examples/symbol_addr.lain` 兼容）。

### 1.5 结构体声明与构造支持可选逗号
- 在 `struct Foo { a: i64, b: i64 }` 定义及 `Foo { a: 1, b: 2 }` 构造中，允许可选 `,` 分隔符（遇到 `,` 自动跳过），完全兼容原有空格分隔写法。

### 1.6 `core.lain` 结构化拆瘦与驱动资源盘点
- 按 DAG 将 `src/core.lain`（1646 行）拆出独立子模块（每个控制 ≤ 400 行）：
  - `src/ctrl.lain`：`if_compile`、`branch_block`、`if_else_part`、`loop_compile`、`switch_compile`；
  - `src/call.lain`：`call_expr`、`icall_call`、`fn_call`、`eval_call`；
  - `src/core.lain` 仅留 Pratt 表达式主循环、`expr_prefix`、`expr_tail`、`stmt_mode`（目标 **≤ 700 行**）。
- **资源上限核查**：在 `meta_v2_driver.c`（或 `-v` 统计输出）测量编译 38 个样例与 `tests/meta/` 时的真实 `fuel`、`workspace` 与 `quota` 峰值，验证不存在无限增长的内存泄漏，将默认上限收回到合理余量。

### 1.7 Phase 1 验收测试与门禁
- 新增 **`tests/meta/polish_syntax.lain`**（及对应负测试）：
  - 全程不使用 `symbol_addr("...")`、不使用 `let dummy = ...`、不写重复 `: Type`、使用 `Array[i]` 直接取值、使用带逗号的 `struct` 与 `void` 语句调用、`let s = "hi";` 推导 `Slice(u8)`；
  - 负例：调用 `Fn` 值过程名时签名不匹配、忽略或误用错误类型的 `let` 推导报明确诊断且产出为空。
- `python scripts/verify_all.py` 全绿。

---

## Phase 2：形态一 —— `TypeBuilder`、静态反射、`?{}` 泛型与编译期插值

**目标**：落地 `docs/stdlib/concepts.md` 的编译期参数机制，以及零字符串拼接的 `Type` 构造与反射成员展开，使 `SoA(T, N)` 与 `max ?{ T: Type, ord: Ord(T) }` 成为纯标准库级代码。

### 2.1 打破 `agg` / `tyreg` 硬上限并统一反射接口
- **数据结构重构**：
  - 将 `agg.lain` 固定 608 字节（最多 8 成员）的形状记录改为「定长头部 + 动态分配成员数组」（从持久区分配，打破 8 字段上限，至少支持任意常规结构体如 64+ 字段）；
  - 将 `tyreg.lain` 每类型 320 字节的定长反射表改为变长紧凑布局或直接服务于统一 `TypeId` 查询；
  - 消除重复接口：反射查询统一返回 **`Slice(u8)`** 与 **`FieldInfo { name: SymId, ty: Type, offset: u64 }`**，提供：
    - `size_of(T: Type) -> u64`、`align_of(T: Type) -> u64`、`type_name(T: Type) -> Slice(u8)`
    - `field_count(T: Type) -> u64`、`field_at(T: Type, idx: u64) -> FieldInfo`（或直接配合 `comptime for f in fields_of(T)`）。

### 2.2 `Type` 一等值与 `TypeBuilder`
- **内置元类型 `Type`**：
  - 仅存在于编译期位置（`TypeId` 的包装值）；若尝试将 `Type` 值作为运行期函数返回值或运行期变量存储，报明确编译诊断（`cannot materialize Type at runtime`）。
- **`TypeBuilder` 原语**：
  - `record_begin() -> u64`（返回构造器句柄）
  - `record_add(tb: u64, name: SymId, ty: Type) -> void`（同时提供 `sym_from_slice(s: Slice(u8)) -> SymId`、`sym_cat(a: SymId, b: SymId) -> SymId`）
  - `record_end(tb: u64) -> Type`（按字段对齐规则计算总大小、对齐与各偏移，登记进 `tyreg`/`agg` 并返回唯一 `TypeId`；字段为空或重名报编译错误）。
- **执行模型边界（必须明确，禁止 VM 嵌套互调死结）**：
  - `Type` 工厂函数（如 `let SoA = func(T: Type, N: u64) -> Type { ... };`）与 `comptime for`、`[:expr]` 涉及读取/写入编译器内部 `tyreg`/`agg`/`env`，**在 `src/` 编译器的编译期求值器（Meta Evaluator）中直接解释执行其 AST**（只操作宿主句柄与 `src/` 内部表，不穿过 `apply` 进入无能力的 `VSpace_Sub`）；
  - 纯数值/纯数据计算（如 `eval(fib(10))`）继续走 `Lain-VM` 的 `apply_module` 沙箱执行。

### 2.3 `?{ ... }` 三区签名、`given` 实例解析与单态化缓存
- **语法**：
  ```lain
  let max = func(a: T, b: T) ?{ T: Type, ord: Ord(T) } -> T {
    if ord.less(a, b) { return b; }
    return a;
  };
  ```
  - `?{ ... }` 位于 `(runtime_params)` 之后、`-> RetType` 之前（无返回类型时在 `{ body }` 之前）；`?{ ... }` 内参数仅限编译期已知类型（`Type`、标量常量、或以 `Type` 工厂构造的 Witness 结构体如 `Ord(T)`）。
- **编译期实例化 4 步算法**：
  1. **模板登记**：定义期遇到带 `?{ ... }` 的 `func`，不立刻调用 `compile_func`，而是登记为 `GenericFn` 模板（保存参数 AST、`?{}` AST、返回类型 AST、函数体 AST 及定义时的作用域快照）。
  2. **参数推导与显式覆写**：
     - 若调用点显式携带 `?{ ... }`（如 `max(a, b) ?{ ord: rev_ord }`），先绑定显式传入的编译期实参；
     - 对尚未绑定的 `Type` 参数（如 `T`）及常量参数（如 `N`），递归将调用实参类型与形参类型模式结构化合一（如 `Mut(SoA(T, N))` 对 `Mut(SoA(Vec3, 4))` 自动解出 `T = Vec3, N = 4`）。若推导冲突或未解出，报错并标明缺失参数名。
  3. **隐式 Witness（`given`）查找**：
     - 允许在模块顶层或局部作用域声明编译期实例（如 `given ord_i64: Ord(i64) = ...;` 或 `given` 修饰编译期 `let`）；
     - 对 `?{}` 中剩余未填的非 `Type` 参数（如 `ord: Ord(T)`，此时 `T` 已解出为 `i64`），在作用域链中按精确目标类型 `Ord(i64)` 自内向外查找 `given` 绑定：0 个命中报 `"missing given instance"`，同层多个命中报 `"ambiguous given instance"`。
  4. **单态化缓存与递归深度保护**：
     - 以 `(template_id, [comptime_arg_vals...])` 为键查缓存表（对 `Ord(T)` 等结构体按字段常量值或唯一实例 ID 规范化）；
     - 命中则直接发 `#call` 到已缓存的 `#proc`；
     - 未命中则在子 `Env` 写入所有编译期常量绑定后生成唯一过程名并编译函数体（同时设置最大泛型递归实例化深度上限，如 64，防止无限递归模板把编译器打爆）。

### 2.4 `comptime for` 与 `[:expr]` 插值
- **`comptime for item in list_expr { body }`**：
  - 支持两类编译期序列：`fields_of(T)`（每次迭代绑定 `item` 为编译期 `FieldInfo`）与整数区间 `0..N`（`N` 为编译期整数，每次绑定 `item` 为编译期 `u64` 常量）；
  - 求值时依次展开编译 `body`（每次建独立内层作用域，支持内部的 `if`、局部 `let`、`return` 等），不发射运行期 `#loop`。
- **`[:expr]` 成员/符号插值**：
  - 支持在点号成员位置使用 `obj.[:expr]`（包含左值 `deref(s).[:expr][idx] = ...`）；
  - 编译期求值 `expr`（结果必须为 `SymId` 或编译期 `Slice(u8)` 自动 intern 为 `SymId`），然后按标准成员偏移/类型规则解析，若目标结构体没有该字段则报正常字段不存在诊断。

### 2.5 Phase 2 验收测试与门禁
- 新增 **`tests/meta/concepts_max.lain`**：泛型类型工厂 `Ord(T)`、`given` 实例自动匹配、`max(10, 20)`、显式 `?{ ord: rev_ord }` 覆写、以及单态化缓存验证（同类型两次调用只生成一份 `#proc`）。
- 新增 **`tests/meta/soa_comptime.lain`**：用 `SoA(T: Type, N: u64) -> Type`（`TypeBuilder`）+ `comptime for` + `[:f.name]` 实现真·列式 SoA，**全文件 0 字符串拼接、0 `inject`**，在 VM 真实执行通过。
- 新增负测试：推导冲突（`max(a_i64, b_u8)`）、缺少 `given`、同层重复 `given` 歧义、运行期变量写进 `[:expr]`、泛型无限递归深度超限。
- `python scripts/verify_all.py` 全绿。

---

## Phase 3：形态二 —— 基于 `AstOut` 的结构化 `quote` 与 `Operative`

**目标**：淘汰 8KB 字符串缓冲 `meta_out()` 与文本重解析 `inject`，把 `host_v2.c` 现成的 `AstOut` 包装成结构化 `quote { ... }` 模板与可注册 `Operative` 宏，让语法扩展和批量声明生成同样优雅。

### 3.1 `Ast` 与 `AstOut` 语言级一等句柄
- 在 Meta 编译期环境中引入内置元类型 `Ast`（底层为 `AstId: u32` 不透明句柄）与 `Env`（仅用于 Operative 宏，底层为 `EnvId`）；
- 若允许在 `Lain-VM` 沙箱执行宏/Operative，为沙箱构建受限 `CSpace_Meta`（仅开放只读 `ast_*`、`AstOut` 事务 `ast_tx_*`/`ast_mk_*`、`sym_*`、`reflect_*`/`record_*` 句柄原语），严禁任何宿主文件/网络能力与 `#addr` 逃逸。

### 3.2 `quote { ... }`、`$(expr)` 插值与 `inject_ast(ast)`
- **`quote { ... }`**：
  - 接收代码模板 AST 组，返回在 `AstOut` 中构造出的新 `Ast`（`AstId`）；
  - 模板内除 `$` 标记外原样复制 `AstOut` 节点结构（含组定界符与普通 Token）。
- **`$(expr)`（及解包多语句/多声明的 `$*(ast_list)`）**：
  - 对 `expr` 在编译期求值并按类型自动转为对应 `AstOut` 节点：
    - `Ast` $\rightarrow$ 原样插入该节点；
    - `SymId` $\rightarrow$ 插入 `TOKEN_IDENT`；
    - `Slice(u8)`（字符串常量）$\rightarrow$ 插入字符串字面量 Token（若需当标识符，先调 `sym_ident(s)`）；
    - `u64` / `i64` / `bool` 编译期常量 $\rightarrow$ 插入对应数字/布尔字面量 Token；
    - **`Type`** $\rightarrow$ 插入**卫生类型绑定节点**（内部携带 `TypeId` 引用而非纯名字文本，保证即使在泛型单态化内部或私有模块也能精准指向目标类型，不发生名字遮蔽）。
- **`inject_ast(ast)`**：
  - 顶层位置调用 `inject_ast(ast)`：将 `Ast` 内的顶层声明序列直接接入模块 `phase1`/`phase2` 编译流（替代旧的文本版 `inject`）；
  - 局部/语句位置调用 `splice(ast)`：在当前块作用域原位展开编译。

### 3.3 开放 `Operative` 语法扩展机制
- **声明形式**：`let my_syntax = operative(args: Ast, env: Env) -> Ast { ... };`（或带状态/直接操作接口）；
- **开放分派表**：
  - 将 `eval.lain`（`top_list`）、`core.lain`（`stmt_mode`、`expr_prefix`）中硬编码的 `if sym == ...` 分派链改为查询作用域/模块中的 **`Operative` 注册表**；
  - 遇到头部标识符绑定为 `Operative` 时，不提前对后面实参求值，直接把未求值的参数 `AstId` 切片与当前 `env` 传给该 `Operative`，拿到其返回的 `AstOut` `AstId` 后回滚失败事务或原位送入 `eval` 编译！
- **旧 `meta_out()` 退场**：确认新旧用例迁移完成后，移除 8KB `meta_out()` 字节缓冲与 `lain2_ast_parse_snippet` 的文本重解析拐杖。

### 3.4 Phase 3 验收测试与门禁
- 新增 **`tests/meta/quote_interface.lain`**：
  - 用 `operative` + `TypeBuilder` + `quote` 实现声明式 `interface Shape { area(Ref(void)) -> i64, scale(Ref(void), i64) -> i64 }` 及 `impl_interface(Shape, Circle)`；
  - 编译期通过反射校对具体方法签名与接口要求严格一致（签名不符必须报编译错误，禁止绕过检查）。
- 新增 **`tests/meta/operative_for.lain`**：用 `operative` + `quote` 在标准库实现 `for i in 0..n { ... }` 语法糖，展开为标准 `loop` 并在 VM 跑通。
- 负测试：`impl_interface` 方法缺失或参数类型不匹配、`$()` 插入非法运行期值、宏展开失败时 `AstOut` 事务完整回滚不残留脏节点。
- `python scripts/verify_all.py` 全绿。

---

## Phase 4：原生输出后端、代数效应 `!{}` 与公理驱动优化

### 4.1 重建干净的 C99 输出后端（原生可执行产物）
- 在 `seed/kernel/src/backend/` 重新实现结构清晰的 C99 后端（直接消费经过 `lainir_verify` 的 `L1Module`）：
  - 忠实遵循 `01-lain-ir.md` 零 UB 语义：整数算术走无符号回绕 + 显式移位/除零/有符号除溢出 Trap 检查（除非已有断言消除）；
  - 结构化区域（`#if`/`#loop`/`#switch`）直译为嵌套 C 块与状态更新循环；
  - 间接 `#call [(ParamTypes) -> RetType](%fn_ptr, ...)` 翻译为精确强转后的 C 函数指针调用；
  - 支持链接 `#extern` 原生 C 符号与生成可执行入口，配合本机 C 编译器产出独立原生 `.exe`/二进制。
- **双边差分验证**：扩展 `scripts/verify_all.py`，对全部 `111+` 条（含 `extern` 真实 C 调用测试！）用例同时跑 **VM 解释结果** 与 **C 后端编译出的原生程序结果**，断言逐位一致。

### 4.2 代数效应 `!{ ... }`（`docs/stdlib/effects.md`）
- **第一档（编译期校验 + 零栈切换运行时）**：
  - 在三区签名增加 `!{ Eff1, Eff2 }` 效应集检查，并在调用链做效应传播与 Handler 覆盖验证；
  - 编译期 Effect 直接与 `Lain-VM` `CSpace` 能力边界绑定（语言级声明 + 物理层能力授权双闸门）；
  - 运行时 **Tail-Resumptive（尾恢复/服务依赖注入）** 走 **Evidence Passing**（隐式追加 Handler 指针参数，无堆/栈切换开销）；**Abort/Error** 效应改写为标签联合返回值与自动冒泡检查。
- **第二档（可挂起/恢复 `resume`）**：在 Meta 库中通过局部状态机或 CPS（续体传递）转换降级为标准 `LAINIR`，保证运行期无需虚拟机环境。

### 4.3 LAINIR 零猜测公理驱动优化器（`seed/kernel/src/ir/opt.c`）
- Meta 为 `Ref`/`Mut`/`Slice`/`?{}` 闭包的 `LAINIR` 产出自动贴附 `01-lain-ir.md` 第 4 章公理（`nonnull`, `align N`, `dereferenceable N`, `noalias`, `range`, `nonzero`, `pure`, `#unreachable`）与强制 **`#call[inline]`**；
- **Debug VM 公理核验模式**（`lainir-run --check-axioms`）：在 VM 执行时动态检验公理是否属实（违背立即触发 `TRAP_AXIOM_VIOLATION`），防止 Meta 库发出假公理导致 Release 误优化；
- **Release 确定性优化 Pass**（全部不做启发式瞎猜）：
  1. **`[inline]` 强制展开**：做 `[inline]` DAG 环校验；单出口直贴，多出口用一次 `#loop __inl` + `#break` 保持结构化；严格检查或外提循环体内被调过程的常量 `#alloca` 防止栈泄漏；
  2. **SROA（聚合标量化）**：将未逃逸的局部 `#alloca`（`Slice` 16 字节头、小 `struct`、闭包 `__Env`）拆散进纯 SSA 寄存器；
  3. **公理驱动的 LICM 与 Trap 剪枝**：依赖 `[noalias]` + `[dereferenceable]` 外提循环内不变只读 `#load`，依赖 `[range]` / `[nonzero]` / `#unreachable` 消除越界、除零、移位 Trap 分支。

---

## Phase 5：终局自举 `lainc`

**前置门槛**：Phase 1–4 稳定，标准库已完备基础容器（`Vec`/`Map`/`StringBuf`）、反射、`?{}`、`quote`/`Operative` 及可用 C 输出后端。

1. **用现代 Lain 编写自举编译器 `lainc`**：
   - 使用 `Ref`/`Mut`/`Slice`、`sret`、`let = func`、`?{}`、`quote` 与结构化集合重写前端与驱动（彻底淘汰旧 `state.lain` 512 字节裸偏移和 `lain0` 限制下的各种临时绕路）；
2. **三代自举校验（Bootstrapping Fixpoint）**：
   - `Stage-0 (lain0)` 编译现存 `src/` $\rightarrow$ `Stage-1 (lain-meta)`；
   - `Stage-1` 编译新版 `lainc` 源码 + Phase 4 C 后端 $\rightarrow$ 原生可执行 `lainc_stage2`；
   - `lainc_stage2` 自编译 `lainc` 源码 $\rightarrow$ `lainc_stage3`；
   - 验证 `lainc_stage2` 与 `lainc_stage3` 产出的 `.l1` / C 源码逐字节一致，且全部测试集 100% 通过；