# Lain-Lang 工程演进与自举维护计划（PLAN.md）

> **核心原则**：物理内核保持极简（`LAINIR` + `Lain-VM` + 零关键字 `Reader` + `AstOut` + `C99` 后端），源语言特性与标准库持续向“统一 `let`、显式可变性引用/切片、零开销 Concept/Witness、干净模块与命名空间”收敛；**同步维护纯 C99 自举快照**，确保仓库在无任何预编译二进制的情况下，仅凭宿主 C/C++ 编译器即可冷启动并完成自举固定点验证。

---

## 0. 当前已验证基线与开发铁律

### 0.1 已完成基线
- **`seed/kernel/`（C 底座）**：
  - `LAINIR` SSA 构建、类型推导与校验（`ir/build.c`, `ir/verify.c`, `ir/infer.c`, `text/parse.c`, `text/print.c`）；
  - `Lain-VM` 确定性隔离沙箱引擎（`vm/engine.c`, `vm/space.c`, `vm/tcb.c`, `vm/caps.c`, `vm/quota.c`, `vm/trap.c`）；
  - `C99` 原生后端（`backend/c99.c`, `lainir_c.c`）与原生 Meta 驱动胶水（`seed/native_meta_driver.c`）；
  - `AstOut`、零关键字形式树解析与隔离 `apply`（`meta/tree.c`, `meta/ast_out.c`, `meta/apply.c`, `meta/host_v2.c`）。
- **`seed/lain0/` 与 `seed/stage1/`（Stage-0 / 冻结 Stage-1 种子）**：
  - `seed/lain0/` C++20 前端与 `seed/stage1/*.lain` 冻结于早期引导语法，用于构建第一代 Stage-1 自举编译器，后续不再跟随现代语法做破坏性同步改动。
- **`src/`（现代 Lain 自举编译器 `lainc`，共 33 个模块）**：
  - **全量零裸 `addr` 类型**（`rg -n "\baddr\b" src` 为 `0` 匹配），全面采用 `struct`、`Mut(T)`、`Ref(u8)`、`Slice(T)`、`MutSlice(T)`、`ptr_cast`、`make_slice`、`null_ref`、`is_null`、`deref` 编写；
  - 入口经 [src/entry.lain](src/entry.lain)，覆盖 AST、类型注册、反射、编译期类型工厂（`ctfor` / `ctprim` / `ctfn`）、Concept `given` / `?{}` 泛型单态化（`genunify` / `generics`）与控制流降级。
- **已通过的验证门禁（[scripts/verify_all.py](scripts/verify_all.py)）**：
  - `seed/lain0/tests/`：`38/38` 编译与校验通过，`111/111` VM/C 真值用例一致；
  - `seed/tests/` IR 用例：`6/6`；
  - `tests/meta/` Meta 扩展与负向测试：`45/45`；
  - `examples/*.lain`（`comptime_soa.lain`, `concepts_monoid.lain`, `eval_sandbox.lain`, `refs_slices.lain`）：`4/4` 编译、校验、VM 与 C99 原生执行一致；
  - 现代源码零 `addr` 审计：`38/38` 通过（33 份 `src/*.lain` + 4 份 `examples/*.lain` + `tests/meta/modern_no_addr.lain`）；
  - 自举固定点验证：`Stage-1 -> Stage-2 -> Stage-3` 逐字节一致（`stage2.l1 == stage3.l1`，`3630 KB IR / 7610 KB C99`）。

### 0.2 工程铁律
1. **零裸 `addr` 约束**：所有 `src/*.lain`、`std/*.lain`、`examples/*.lain` 禁止出现 `\baddr\b`（连同 `tests/meta/` 现代用例），由 `verify_all.py` 自动阻断。
2. **双边等价与正负例配齐**：每项新特性或新语法必须同步配齐正例（VM 真实执行断言 + C99 原生可执行输出结果比对）与负例（明确报错、产物为 0 字节）。
3. **单向无环（DAG）与模块边界**：保持 `src/` 模块单向无环依赖，禁止为了图快往 `core.lain` 或 `eval.lain` 堆叠大段补丁逻辑。
4. **先滚动升级自举快照、再废弃旧语法**：任何破坏向下兼容的 `src/` 语法演进，必须严格遵循 Phase 1 的两步滚动自举协议，严禁把自举链“锯断”。

---

## Phase 1：C99 自举快照维护机制（无二进制冷启动契约）

**核心动机**：`seed/lain0/` 与 `seed/stage1/` 停留在旧语法（顶层 `func name` / `struct Name`）。一旦后续 Phase 3 将 `src/*.lain` 全面迁移到统一 `let` 语法并移除旧的顶层 `func` / `struct` / `enum` 声明语句，冻结的 Stage-1 将不再能解析新版 `src/`。因此必须在仓库中长期维护一份纯文本、跨平台可编译的 **C99 引导产物 `seed/bootstrap/lainc_boot.c`**，让任何环境无需任何预编译 `.exe` / 二进制，仅凭 C 编译器即可从源码冷启动出最新编译器并继续迭代 `src/`。

### 1.1 自举产物落盘与冷启动链路
- **托管文件**：在仓库纳入 **`seed/bootstrap/lainc_boot.c`**（由经校验的 `build/stage2.l1` 经 `lainir-c` 生成的标准 C99 单文件源码，禁止提交 `.exe`、`.dll`、`.lib` 等二进制到仓库）。
- **无二进制冷启动链（Trusting-Trust Clean Bootstrap）**：
  1. CMake / C 编译器编译 `seed/kernel/`（`lain_seed.lib`）与 `seed/lainir_c.c`、`seed/lainir_verify.c`、`seed/lainir_run.c`、`seed/meta_v2_driver.c`；
  2. 直接用宿主 C 编译器编译 `seed/native_meta_driver.c + seed/bootstrap/lainc_boot.c + lain_seed.lib` $\rightarrow$ **`build/lain-stage1.exe`**；
  3. `build/lain-stage1.exe` 编译当前 `src/entry.lain` $\rightarrow$ `build/stage2.l1` $\rightarrow$ `lainir-c` 生成 `build/stage2.c` $\rightarrow$ C 编译器编出 **`build/lain-stage2.exe`**；
  4. `build/lain-stage2.exe` 自编译 `src/entry.lain` $\rightarrow$ `build/stage3.l1` $\rightarrow$ `lainir-c` 生成 `build/stage3.c`；
  5. **固定点断言**：检查 `stage2.l1 == stage3.l1` 与 `stage2.c == stage3.c` **逐字节完全一致**。

### 1.2 破坏性语法演进的“两步滚动升级协议”
当后续 Phase 需要修改 `src/` 本身使用的语法（例如迁移到纯 `let` 或 `pub let`）时，严格按以下两步提交，保证每一步在 git 历史上都可独立从头冷启动：
1. **Step A（新增新语法支持，保留旧语法兼容）**：
   - 在 `src/` 中仍使用**当前 `seed/bootstrap/lainc_boot.c` 支持的语法**，实现新特性；
   - 运行冷启动自举 + 全部新增正负例测试，确保 `stage2.l1 == stage3.l1` 固定点通过；
   - 运行刷新脚本将验证通过的 `build/stage2.c` 覆盖写入 `seed/bootstrap/lainc_boot.c` 并单独存档提交。
2. **Step B（迁移 `src/` 自身写法，淘汰旧语法）**：
   - 此时 `seed/bootstrap/lainc_boot.c` 已具备编译新语法的能力；
   - 将 `src/*.lain`、`examples/`、`tests/` 全面重写为新语法，并移除 `src/` 中的旧语法兼容分支（同步补充拒绝旧语法的负向测试）；
   - 使用 `seed/bootstrap/lainc_boot.c` 编出 Stage-1 编译新版 `src/`（Stage-2），再用 Stage-2 自编 Stage-3 验证固定点一致；
   - 将新版 `build/stage2.c` 再次同步至 `seed/bootstrap/lainc_boot.c`。

### 1.3 工具与自动化脚本
- 新增 **`scripts/update_bootstrap.py`**（或为 `scripts/verify_all.py` 增加 `--update-bootstrap` 参数）：
  - 仅在 `verify_all.py` 全部测试与 `stage2.l1 == stage3.l1`、`stage2.c == stage3.c` 固定点均通过后，才允许将 `build/stage2.c` 同步为 `seed/bootstrap/lainc_boot.c`；
  - 调整 `scripts/verify_all.py` 启动逻辑：默认以 `seed/bootstrap/lainc_boot.c`（若尚未初始化则回退 `seed/stage1/`）构建 Stage-1，并保持 `seed/lain0/tests/`（`38/38`）继续通过 `seed/lain0/` 回归测试看护旧种子工具。

---

## Phase 2：`extern` 宿主 I/O 打通与 `std::io` 标准库

**核心目标**：消除示例“只能靠 `main` 返回数字猜语义”的现状，让所有现代样例通过显式 `extern` 与 `std::io` 输出人类可读的运行过程与结果，且保证 **LainVM (`lainir-run`)** 与 **C99 原生可执行程序** 的 `stdout` 逐字一致。

### 2.1 双端统一的底层 `extern` I/O 原语
在遵循 `Lain-VM` 沙箱隔离原则的前提下，为执行器与 C99 运行时注册一组最小化标准输出外部符号（编译器自身与 `eval(...)` 子沙箱保持无 I/O 隔离不变）：
- **`lain_io_write(ptr: Ref(u8), len: u64)`**：按显式字节长度输出文本，不依赖 C `\0` 结尾，直接匹配 `Slice(u8)` 的 `(ptr, len)` 内存表示；
  - [seed/lainir_run.c](seed/lainir_run.c)：通过 `LainVmCaps` 能力表注册宿主回调，读取字节前严格经 `VSpace` 边界与可读权限校验（越界立即触发 VM Trap）；
  - [seed/kernel/src/backend/c99.c](seed/kernel/src/backend/c99.c)：在 C99 Preamble 中提供 `static inline` / 运行时实现 `lain_io_write`（使用 `fwrite(ptr, 1, len, stdout)`）。
- **`lain_io_print_i64(v: i64)` / `lain_io_print_u64(v: u64)` / `lain_io_newline()`**：方便数值与换行输出（两端的负数符号与格式化规则严格统一）。

### 2.2 `std::` 标准库路径解析与 `std/io.lain`
- **逻辑标准库根寻址**：
  - 在 [seed/meta_v2_driver.c](seed/meta_v2_driver.c) 与 [seed/native_meta_driver.c](seed/native_meta_driver.c) 中扩展 `lain2_source_import`：当导入路径以 `"std/"`（源码写作 `import("std::io")`）开头时，自动从仓库标准库目录 `<repo_root>/std/` 加载对应 `.lain` 文件，不受用户源码所在目录深度影响。
- **创建 [std/io.lain](std/io.lain)**：
  - 基于上述 4 个 `extern` 原语封装类型安全 API：
    - `print(s: Slice(u8))` / `println(s: Slice(u8))`（字符串字面量可自动强制为 `Slice(u8)`）
    - `print_i64(v: i64)` / `println_i64(v: i64)`
    - `print_u64(v: u64)` / `println_u64(v: u64)`

### 2.2b 测试与性能标准库：`std/test.lain` 与 `std/bench.lain`
- **`std::test`**：测试本身是普通 Lain 程序，断言逻辑写在标准库里，不再依赖 Python 字典里的“魔数返回值”：
  - `Suite { name: Slice(u8), passed: u64, failed: u64 }`；`test.suite(name)`、`test.check(s: Mut(Suite), label, cond)`；
  - 基于 Concept 的 `test.eq(s, label, got: T, want: T) ?{ T: Type, c: TestCmp(T) }`：失败时经 `std::io` 打印 `[FAIL] suite :: label: expected X, got Y`；`i64`/`u64`/`bool`/`Slice(u8)` 提供内置 `given`，自定义 `struct` 自行 `given TestCmp(Vec2) { ... }`；
  - `test.finish(s) -> i64`：打印 `[PASS] suite (n/n)` 或 `[FAIL] suite (k failed)`，返回失败数（`0` 即成功）。
- **`std::bench` + 遥测 extern**：VM 与 C99 运行时同步提供只读 `lain_perf_steps() -> u64`（VM 步数，原生为 0）与 `lain_perf_nanos() -> u64`（单调纳秒）；`bench.measure` 在 Lain 代码里直接打印步数与耗时。
- **脚本职责收缩**：`verify_all.py` 对 `std::test` 用例只做三件事：编译运行（VM 与 C99 原生）、断言返回值为 0 且两端 `stdout` 逐字节一致；对 `compile-fail` 负例断言编译失败、产物 0 字节、无 VM Trap；固定点与零 `addr` 审计。期望值不再写在 Python 里。

### 2.3 `examples/*.lain` 升级与双端 `stdout` 差分测试
- 改造 [examples/](examples) 4 个展示样例，使用 `let io = import("std::io");` 在运行时清晰打印每一步演练内容、计算摘要与最终结果（并将新增交互式示例编排到工作流验证中）。
- 更新 `scripts/verify_all.py`：
  - 对使用了 `std::io` 的测试与样例，同时捕获 `lainir-run`（VM）与 `clang` 编译产物（原生 `.exe`）的 **stdout 输出**与返回值，断言两者 **逐字节一致**；
  - 为 VM `lain_io_write` 越界指针补充一条 Trap 负测试，验证沙箱保护有效。

---

## Phase 3：统一 `let` 声明语法与 Concept / `given` 现代化

**核心目标**：实现 [docs/stdlib/concepts.md](docs/stdlib/concepts.md) 约定的“一切声明收敛为 `let`”，允许 `struct { ... }` 与 `enum { ... }` 直接作为返回 `Type` 的一等类型表达式，使 Concept 定义和 `given` 注册不再臃肿。

### 3.1 新增匿名 `struct { ... }` / `enum { ... }` 与匿名 `given`（滚动 Step A）
1. **`struct { ... }` 与 `enum { ... }` 升级为一等类型表达式**：
   - **顶层类型声明**（[src/eval.lain](src/eval.lain) `p1_let`）：
     ```lain
     let Vec2 = struct { x: i64, y: i64 };
     let Boxed = enum { Int(i64), Empty };
     ```
     在 `phase1` 解析 `let Name = struct { ... }` / `let Name = enum { ... }` 时，直接沿用形状建表逻辑登记为具名结构体/枚举（`sym_kind_struct` / `sym_kind_enum`）。
   - **编译期类型工厂内的 `struct { ... }` 求值**（[src/ctfn.lain](src/ctfn.lain)）：
     - 当编译期函数返回 `Type` 时，允许在函数体/表达式位置求值 `struct { field: TypeExpr, ... }`：内部自动调用 `TypeBuilder` 构造完整布局并在 `tyreg` 注册为 `TypeId`，使 Concept 无需再手写 `record_begin / record_add / record_end`：
       ```lain
       let Scale = func(T: Type) -> Type {
         return struct {
           scale: Fn((T, i64) -> T)
         };
       };
       ```
     - （注：`record_begin / record_add / record_end` 保留作 `comptime for` 动态增删字段的底层反射构造原语，用于 `make_soa` 等高阶元编程。）
2. **统一 `@root let` 函数声明**：
   - 在 [src/eval.lain](src/eval.lain) 与 [src/decl.lain](src/decl.lain) 中支持 `@root let main = func() -> i64 { ... };`，补齐此前只有 `@root func` 的缺口。
3. **简化 `given` 语法（[src/given.lain](src/given.lain)）**：
   - **匿名 `given`**：当仅用于向作用域注册默认 Witness 实例、无需名字引用时，省去冗余变量名与冒号类型标注：
     ```lain
     given Monoid(i64) {
       zero: i64_zero,
       combine: i64_add
     };
     ```
   - **具名 `given` 去冗余标注**：需要局部名字或手动 `?{ m: prod_i64 }` 传入时，支持省略冒号重复类型：
     ```lain
     given prod_i64 = Monoid(i64) {
       zero: i64_one,
       combine: i64_mul
     };
     ```
4. **刷新 C99 自举快照**：跑通全部测试及 `Stage-2 == Stage-3` 固定点后，更新 `seed/bootstrap/lainc_boot.c`。

### 3.2 迁移现代代码并废弃旧 `struct` / `enum` / `func` 语句（滚动 Step B）
1. 将全部 **33 个 `src/*.lain` 编译器模块**、**`std/*.lain`**、**`examples/*.lain`**、**`tests/meta/*.lain`** 全面迁移为纯 `let` 语法与新版 `struct { ... }` Concept / 匿名 `given`。
2. 在 [src/eval.lain](src/eval.lain)（`phase1` 与 `phase2`）中**移除**顶层旧声明语句 `struct Name { ... }`、`enum Name { ... }`、`func name(...)`（保留表达式位置的 `struct { ... }`、`enum { ... }`、`func(...)`）。
3. 补齐负向测试：验证旧写法 `struct Foo { x: i64 }` 与 `func bar() {}` 现被现代编译器明确拒绝并报错。
4. 用更新后的 `seed/bootstrap/lainc_boot.c` 验证 `Stage-1 -> Stage-2 -> Stage-3` 固定点通过，并再次刷新 `seed/bootstrap/lainc_boot.c`。

---

## Phase 4：Module / Namespace（`mod { ... }`）、多级路径（`a.b.c`）与 `pub` 可见性

**核心目标**：确立清晰的封装边界，消除“导入即全暴露”的问题，同时支持文件内逻辑命名空间与任意深度的 `M.sub.item` 访问。

### 4.1 `pub` 可见性与封装边界（[src/modules.lain](src/modules.lain)、[src/eval.lain](src/eval.lain)）
- 在 `modules.SymEntry` 增加导出标志 `is_pub`（`0` 私有，`1` 公开）：
  - 顶层 `pub let ...` 与 `pub given ...`、`pub extern ...` 对外导出（`@root` 入口默认公开或显式写 `pub`）；
  - 未加 `pub` 的 `let` / `given`（如编译器内部辅助函数 `word_at`、`val_idx`）仅在本模块与本命名空间内可见；
  - 当跨模块通过 `M.priv_item` 访问未导出的私有符号时，编译器报明确错误（`"private symbol: ..."`），不生成 IR。
- **跨模块 `given` 语义**：`pub given` 在被导入模块时按可见性规则进入可见实例表（或支持通过显式导入路径解析），确保 Concept 生态跨文件工作且不发生私有实现意外污染。

### 4.2 文件内命名空间 `let ns = mod { ... };`
- 在 `phase1` / `phase2`（[src/eval.lain](src/eval.lain)）中支持模块表达式 `mod { ... }`：
  ```lain
  let math = mod {
    pub let Vec2 = struct { x: i64, y: i64 };
    let internal_bias = func() -> i64 { return 1; };
    pub let add = func(a: Vec2, b: Vec2) -> Vec2 { ... };
  };
  ```
- 编译器为每个 `mod { ... }` 分配一个虚拟子模块源编号，将其内部声明隔离登记在该子模块符号表中，并允许嵌套 `mod { pub let sub = mod { ... }; }`。

### 4.3 多级路径消解（`a.b.c`）
- 将目前仅支持 1 层 `M.name` 的解析点统一升级为循环链式消解（`resolve_mod_path`）：
  1. 类型标注位置（[src/ctx.lain](src/ctx.lain) `qual_type`）：支持 `a.b.Vec2` 与泛型 `a.b.Scale(a.b.Vec2)`；
  2. 运行期表达式位置（[src/member.lain](src/member.lain) `dotted` / `member_ref`）：支持 `a.b.fn(...)`、`a.b.Enum.Variant { ... }`、`a.b.Struct { ... }`；
  3. 编译期求值与类型工厂位置（[src/ctfn.lain](src/ctfn.lain) `eval_factory_call`、`src/given.lain`）：支持 `a.b.Scale(T)` 与 `a.b.scale_fn`。
- 若路径中间遇到模块内部私有子命名空间或私有符号，在解析点即刻报告可见性错误。

### 4.4 `src/` 与 `std/` 清理迁移与固定点验收
- 给 `src/*.lain`（33 模块）与 `std/*.lain` 补齐 `pub let` 导出声明（遵循滚动升级协议先增兼容再切全量）。
- 增加可见性与多级命名空间的正负测试集，验证 `Stage-1 (lainc_boot.c) -> Stage-2 -> Stage-3` 固定点一致后更新 `seed/bootstrap/lainc_boot.c`。

---

## Phase 5：后续演进（结构化宏、代数效应与优化器）

在统一语法、标准库 I/O、模块可见性与 C99 自举固化之后，继续推进以下高阶语言与编译器基础设施：
1. **结构化宏（`AstOut` `quote { ... }` / `$(expr)` / `Operative`）**：用结构化 AST 构造取代 `inject` 文本拼接与 8KB 字符串缓冲（对应 [docs/03-lain-meta.md](docs/03-lain-meta.md)），让语法扩展可在开放分派表中注册；
2. **单次代数效应（`!{ ... }`）**：实现 [docs/stdlib/effects.md](docs/stdlib/effects.md) 定义的三区签名第三区（`Effect` 检查、Tail-Resumptive Evidence Passing 与 Abort 状态码降解）；
3. **公理驱动 `LAINIR` 优化器（`seed/kernel/src/ir/opt.c`）**：围绕 `Ref`/`Mut`/`Slice`/`?{}` 自动附着的 `nonnull`、`noalias`、`dereferenceable`、`range` 公理实现 `[inline]`、SROA、LICM 与安全检查分支剪枝（配合 `lainir-run --check-axioms` 校验公理真实性）。

---

## Phase 6：测试标准库与三维 Profiler

### 6.1 测试体系（以 std::test 为核心）
1. **目录按正交特性划分**（每个特性只留高密度正例 + 边界负例，不写凑数用例）：
   	ests/lang/{types,comptime,concepts,modules}/；底座回归保留 seed/tests/ 与 seed/lain0/tests/；端到端展示为 xamples/。
2. **正例**：用 std::test 编写，运行器只断言“返回 0 且 VM 与 C99 原生 stdout 一致”。结构性断言（常量已折叠、无间接调用、已单态化）同样用文件头注释声明：// @expect-ir-has: ... / // @expect-ir-absent: ...。
3. **负例**：文件头 // @test: compile-fail + // @expect-error: <关键词>；断言编译失败、诊断含关键词、产物 0 字节。
4. **自举与审计层**：Stage-1 -> Stage-2 -> Stage-3 固定点 + 全仓零 ddr 扫描。

### 6.2 三维 Profiler（--profile）
1. **IR 静态质量**（lainir-verify --stats）：指令总数、#alloca 数与栈帧字节、访存/转换占比、直接与间接调用数、**死栈槽率**（声明后从未被引用的 #alloca 占比）、最重过程 Top-10。已知靶点：src/decl.lain 固定预留 16 + 32 + count_tree 个栈槽，导致一行函数也发射 48 条 #alloca；局部变量全走栈槽使 38 样例在 VM 里约 4037 万步（lain0 约 360 万步）。
2. **VM 运行时热点**（lainir-run --profile / lain-meta-v2 --profile）：按过程统计调用次数、自身步数与占比、含子调用步数、栈峰值；指令分类占比（ALU / 访存 / 控制流）。用于同时剖析用户程序与 src/ 编译器自身的热点。
3. **编译阶段耗时与内存水位**（lain-stage2 --profile）：Reader/符号驻留、phase1+phase2、comptime pply 沙箱、verify、C99 发射与 clang 编译各阶段耗时；arena/ralloc/workspace 峰值；产物体积。
4. **回归基线**：把 VM 总步数、IR 体积、死栈槽率记入基线文件，erify_all.py 对显著劣化（如 +10%）告警，让优化有数可查。
