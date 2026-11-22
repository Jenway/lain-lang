# Lain-Lang 工程演进与自举路线图（PLAN.md）

> **核心原则**：源语言全部高级特性（一等模块、Concept/Witness、脱糖宏与代数效应）在前端编译器与标准库实现；自举编译器 [src/](src) 持续保持**全仓零裸 `addr`** 与 **100% 统一 `let` 声明**；长期维护纯文本 C99 自举快照 [seed/bootstrap/lainc_boot.c](seed/bootstrap/lainc_boot.c)，按“滚动两步升级 + 逐步用现代 Lain 吞并 C 内核”的路线演进，最终达成**全仓唯一的 C 代码仅剩 `lainc` 自身的自举编译产物**。

---

## 0. 当前已验证基线与开发铁律

### 0.1 已完成基线（截至 Phase 3 完成）
- **自举编译器 `lainc`（[src/](src)，共 33 个 `.lain` 模块）**：
  - **100% 零裸 `addr`**：全仓现代代码彻底淘汰裸指针 `addr`，全面采用 `Ref(T)`、`Mut(T)`、`Slice(T)`、`MutSlice(T)`、`ptr_cast`、`make_slice`、`null_ref`、`is_null`、`deref` 编写；
  - **100% 统一 `let` 声明语法**：顶层 `struct Name`、`enum Name`、`func name` 旧声明语法已彻底废弃，统一写作 `let Name = struct { ... };`、`let Name = enum { ... };`、`let name = func(...) ... { ... };` 与 `@root let ...;`；
  - 支持一等声明式结构体表达式 `return struct { ... };`、动态类型构造 `record_begin/add/end`、`comptime for`、编译期沙箱求值 `eval(...)` 与现有 `?{}` Concept 泛型单态化。
- **C99 无二进制冷启动闭环（[seed/bootstrap/lainc_boot.c](seed/bootstrap/lainc_boot.c)）**：
  - 旧种子 [archive/lain0/](archive/lain0) 与 [archive/stage1/](archive/stage1) 已归档，日常构建与验证彻底脱离 `lain0`；
  - 从 `seed/native_meta_driver.c + seed/bootstrap/lainc_boot.c + lain_seed.lib` 编译 Stage-1，编出 Stage-2 再自编 Stage-3，完成字节级不动点自举（`stage2.l1 == stage3.l1`）。
- **宿主 I/O、遥测原语与最小标准库 MVP（[std/](std)）**：
  - 在 `seed/lainir_run.c`（带 `VSpace` 边界校验）与 `seed/kernel/src/backend/c99.c` 对齐实现了 `lain_io_write`、`lain_io_print_i64`、`lain_io_print_u64`、`lain_io_newline`、`lain_perf_steps`、`lain_perf_nanos` 6 个宿主原语；
  - 落地 `std::io`（[std/io.lain](std/io.lain)）、`std::test`（[std/test.lain](std/test.lain)）、`std::bench`（[std/bench.lain](std/bench.lain)）及 4 个可运行出真实 `stdout` 的现代样例（[examples/](examples)）。
- **全量验证门禁（[scripts/verify_all.py](scripts/verify_all.py)）**：
  - `C99 自举`：`Stage-1 -> Stage-2 -> Stage-3` 固定点一致；
  - `IR 用例`：`6/6`；`Meta 扩展测试`（含旧语法拦截负测试与标准库双端一致/越界 Trap 验证）：`47/47` + `标准库 4/4`；`现代样例`（VM 与 C99 原生 `stdout`/返回值逐字节差分）：`4/4`；
  - 零 `addr` 现代文件检查：`47/47` 通过；全轮验证耗时约 `22.6` 秒。

### 0.2 工程铁律
1. **文档先于实现并保持同步**：语言与内核规范集中在 [docs/](docs/README.md)，任何特性修改同步更新对应规范文档。
2. **零裸 `addr` 与统一 `let` 约束**：所有 `src/*.lain`、`std/*.lain`、`examples/*.lain` 及现代测试用例严禁回退使用裸 `addr` 或废弃顶层声明语法。
3. **双边等价与正负例配齐**：每项新特性须配齐可验证 `stdout`/返回值等价（`lainir-run` VM 与 C99 原生可执行一致）的正例与明确报错、产物 0 字节的负例。
4. **两步滚动升级协议**：任何修改 `src/` 自身语法或导出规范的改动，严格按 `Step A（新增支持 + 刷新 lainc_boot.c） -> Step B（迁移全仓源码 + 切除旧兼容 + 再次刷新 lainc_boot.c）` 两步执行，保证提交历史上每一步都能无二进制从头冷启动。
5. **停止扩张手写 C 代码**：除自举与必要 Bug 修复外，不再向 `seed/kernel/` 堆叠新的 C 业务子系统（如优化器或复杂前端），把精力留给 Phase 4~6 用纯 Lain 吞并内核。

---

## 1. 已完成阶段摘要（Phase 1 ~ Phase 3）

| 阶段 | 核心交付 | 关键提交 |
| :--- | :--- | :--- |
| **Phase 1：C99 自举快照** | 固化 [seed/bootstrap/lainc_boot.c](seed/bootstrap/lainc_boot.c)，支持 `--update-bootstrap` 与零 `lain0` 冷启动 | `f4a74ff` |
| **Phase 2：宿主 I/O 与标准库 MVP** | 打通 VM / C99 6 个 `lain_io_*` / `lain_perf_*` 原语、`std::` 路径解析、`std/io,test,bench.lain` 与 `stdout` 差分 | `36a1ee0` |
| **Phase 3：统一 `let` 与极速迭代** | 归档 `lain0`/`stage1`（验证从 92.8s 降至 22.6s），实现 `let Name = struct/enum/func` 与编译期 `return struct {}`，全仓迁移并禁止旧顶层声明 | `2292b71`, `dca162b` |

### 1.1 后续阶段完成状态

| 阶段 | 状态 | 已验证交付 |
| :--- | :--- | :--- |
| **Phase 4：一等模块系统** | 已完成 | `pub let` 可见性、`module { ... }`、多级路径、`sig` 签名视图、函子与 `use` 见证合成；`given` 关键字已删除；`tests/meta/` 正负用例与 `examples/modular_telemetry_engine.lain` 通过 |
| **Phase 5.1：AstOut 事务与 Expand 管线** | 已完成 | `AstOut` 的 `tx_begin / tx_commit / tx_rollback` 与独立 Expand 遍次接入，`tests/meta/ast_out_expand.lain` 通过 |
| **Phase 5.2：标准语法脱糖** | 已完成 | UFCS、`f"..."` 零堆分配格式化、`match` 决策树与穷尽性核验；`ufcs_*`、`fmt_interp.lain`、`match_enum.lain` 通过 |
| **Phase 5.3：单次代数效应** | 已完成 | `effect` / `perform` / `!` / `!{}` 效应集检查与 `handle ... with`；中止性、可恢复、效应行多态用例通过 |
| **Phase 6.1：纯 Lain LAINIR 校验器与 C99 后端** | 已完成 | `src/ir/verify.lain`、`src/backend/c99.lain`，`tests/meta/phase6_ir_c99.lain` 覆盖合法模块与四类畸形模块 |
| **Phase 6.2：纯 Lain Reader 与 AstOut** | 已完成 | `src/frontend/reader.lain`、`src/meta/ast_out.lain`，`tests/meta/phase6_reader_astout.lain` 覆盖词法/跨行位置/事务回滚 |
| **Phase 6.3：纯 Lain 确定性沙箱执行引擎** | 已完成 | `src/vm/vspace.lain`、`src/vm/image.lain`、`src/vm/engine.lain`；`tests/meta/phase6_vm_sandbox.lain` 覆盖多过程映像执行与五类确定 Trap；`examples/self_hosted_pipeline.lain` 串起 Reader、事务化 AstOut、LAINIR 校验、VM 执行与 C99 发射，并在 VM 与 C99 上给出逐字节一致的报告

---

## Phase 4：一等模块系统（`module` / `pub let` / `sig` / 函子）与 `given` 退场

> **设计依据**：[docs/stdlib/modules.md](docs/stdlib/modules.md) 与 [docs/stdlib/concepts.md](docs/stdlib/concepts.md)。
> **核心目标**：建立真正的封装边界与任意深度命名空间，并用“导出 `pub let Self = T;` 的 `module`”同时驱动命名空间、UFCS 与 `?{}` Concept Witness 自动合成，从而彻底砍掉此前临时引入的 `given` 关键字。

### 4.1 Step A：前端能力落地（滚动兼容与自举刷新）
1. **`pub let` 可见性与跨模块访问校验**（[src/modules.lain](src/modules.lain)、[src/eval.lain](src/eval.lain)）：
   - 在符号表项 `SymEntry` 增加导出标志 `is_pub`（`pub let ...`、`pub extern ...` 为公开，其余仅本模块可见）；
   - 引入模块跨界访问私有符号时，编译期立即报错 `"private symbol"`（Step A 过渡期先校验明确标出了 `pub` 的模块或经受控开关控制，到 Step B 全量启用）。
2. **一等模块表达式 `module { ... }` 与多级路径 `a.b.c`**：
   - 支持文件内声明嵌套模块：`let math = module { pub let sub = module { ... }; ... };`（复用虚拟子源编号将声明隔离登记）；
   - 将此前仅限一跳 `M.item` 的解析点统一升级为链式多级路径消解（`resolve_mod_path`）：覆盖类型位置（`src/ctx.lain`）、运行期表达式与构造器位置（`src/member.lain`、`src/expr`）、编译期工厂与调用位置（`src/ctfn.lain`）。
3. **基于 `module` + `pub let Self = T;` 的 Witness 合成与作用域导入 `use` / `open`**：
   - 支持 `use M;`（在当前词法作用域激活 `M` 的 `Self` 关联导出与默认值查找）、`use M.{ a, b };`、`open M;`；
   - 在 `?{ m: Concept(T) }` 解析时，按当前词法作用域搜寻导出 `pub let Self == T` 的可见模块，从模块 `pub` 导出中按字段名与函数签名校验并自动构造 Witness 字典；
   - 支持调用点直接传模块显式覆盖：`fn(...) ?{ m: OtherMod }`；同作用域存在多个匹配且未显式覆盖时，报歧义错误。
4. **模块签名 `sig { ... }` 与函子 `func(T: Type, ...) -> Module`**：
   - 支持 `let S = sig { type Target; op(...) -> Ret; };` 与 `let M: S = module { ... };` 静态签名完整性校验及视图裁剪；
   - 支持返回 `Module` 的编译期纯函数（记忆化单例缓存 + `@Functor$Arg$fn` 符号修饰，如泛型容器 `Vec(T)`）。
5. **验收与刷新**：配齐正负向用例（仍保持 `src/` 可由当前 `lainc_boot.c` 编译），跑通固定点后刷新 [seed/bootstrap/lainc_boot.c](seed/bootstrap/lainc_boot.c)。

### 4.2 Step B：全仓边界收敛与彻底删除 `given` 关键字
1. **淘汰 `given`**：
   - 将 [examples/concepts_monoid.lain](examples/concepts_monoid.lain) 与 `tests/meta/` 中所有 `given` 示例迁移为标准 `module { pub let Self = ...; ... };` + `use` / `?{ m: CustomMod }`；
   - 将 `tests/meta/` 中的 `std::test` 与 Concept 测试整理对齐新机制；
   - 从 `src/given.lain`、`src/eval.lain`、`src/sym.lain` 中**彻底删除 `given` 关键字语法与分支**，新增拒绝 `given` 的负向测试。
2. **全仓补齐 `pub let` 封装边界**：
   - 给 `src/*.lain`（33 模块）与 `std/*.lain` 精确标注对外导出 `pub let`，隐藏内部辅助过程；
   - 开启严格跨模块私有符号拦截并补充负测试（验证访问未加 `pub` 的成员必然编译失败）。
3. **刷新自举快照**：验证 `Stage-1 -> Stage-2 -> Stage-3` 固定点通过后再次更新 [seed/bootstrap/lainc_boot.c](seed/bootstrap/lainc_boot.c)。

---

## Phase 5：`AstOut` 展开阶段（Expand Phase）、三大脱糖与单次代数效应 `!{}`

> **设计依据**：[docs/03-lain-meta.md](docs/03-lain-meta.md)、[docs/stdlib/desugaring.md](docs/stdlib/desugaring.md)、[docs/stdlib/effects.md](docs/stdlib/effects.md)。
> **核心目标**：用 `AstOut` 结构化语法树变换取代文本拼接 `inject`，把 UFCS、`f"..."` 惰性格式化与 `match` 决策树统一落在展开管线，并兑现签名第三区 `!{}` 单次代数效应。

### 5.1 接入 `AstOut` 事务与真正的多阶段管线 (`Expand -> Elaborate -> Lower`)
1. 将内核已有的 `AstOut` 游标分配与 `mark / commit / rollback` 事务原语暴露给 `src/`，实现零文本拼接的结构化 AST 构造；
2. 建立独立的 **Expand（展开）** 遍次：宏展开输出受事务保护的 `RawAst` 节点交回细化器，逐步淘汰早期的 `inject(...)` 8KB 文本缓冲机制。

### 5.2 标准语法脱糖落地（[desugaring.md](docs/stdlib/desugaring.md)）
1. **UFCS 方法调用糖**：支持 `receiver.method(args...)` 自动检索 `pub let Self = typeof(receiver)` 的活跃可见模块并改写为 `M.method(receiver, args...)`（含 `Ref(Self)` / `Mut(Self)` 左值引用自适应与只读安全检查）；
2. **零堆分配惰性格式化代理 `f"..."`**：通过 `AstOut` 原位将 `f"total: {x}"` 重写为栈上代理构造 `std.fmt.bind((x)) ?{ fmt: "total: {}" }`，配合 `std::io` 实现零堆分配格式化输出；
3. **`match` 决策树编译与穷尽性核验**：收敛 `enum`/标签分支匹配的穷尽性与冗余分支诊断，直接降级为 `#switch` 与负载提取。

### 5.3 单次代数效应落地（[effects.md](docs/stdlib/effects.md)）
1. 完善 `effect { op ... }`、`perform`、`!` 传播操作符与函数签名第三区 `!{ ... }` 效应集检查（包含默认纯度检查与效应消除律）；
2. 落地 `handle ... with { return(...) => ..., op(...) => ... }`：
   - **Abortive 效应（`-> Never`）**：直接降解为多路状态码 `(tag, payload)` 与 `#switch`，零栈展开开销（支持 Value-Error-Stopped 三通道模型）；
   - **Tail-Resumptive & One-Shot 续体** 与 **效应行多态（`!{ Iter.Effects }`）**：实现无函数染色的同步/异步复用接口。

---

## Phase 6：纯 Lain 吞并内核（走向 Zero Handwritten C 终局）

> **设计目标**：当模块系统、Concept、宏与代数效应完备之后，分三步用纯现代 Lain 重写 `seed/kernel/` 全部底层组件并并入 [src/](src)，实现真正意义上的完全自托管。

1. **Step 6.1：用 Lain 重写 `LAINIR` 校验器与 `C99` 原生后端**
   - 在 `src/ir/` 与 `src/backend/c99.lain` 中直接对内存中的 IR 模块做静态类型/SSA 校验并输出等价标准 C99 代码；
   - 将运行期所需的数十行最小 OS 原语（`fwrite` / `malloc` / 单调时钟 / `main(argc, argv)` 入口驱动）随后端前导文本统一发射，从而取消对外部 `lainir-c`、`lainir-verify` 以及 `native_meta_driver.c` 胶水的独立构建依赖。
2. **Step 6.2：用 Lain 重写零关键字 `Reader` 与 `AstOut`**
   - 用 `Slice(u8)` 切片在 `src/frontend/reader.lain` 与 `src/meta/ast_out.lain` 实现词法切分、行号 Span 映射与游标回滚缓冲，彻底摆脱 C 侧语法树构筑代码。
3. **Step 6.3：用 Lain 重写确定性沙箱执行引擎 `Lain-VM`**
   - 基于零裸 `addr` 切片、枚举与 `( ?{ caps: VmCaps } )` 能力接口在 `src/vm/` 实现 `Image` 装载、`VSpace` 窗口仲裁、`TCB`、`Endpoint` 与燃料限额（Fuel）单步引擎；
   - `eval` / `apply` 编译期沙箱求值直接在纯 Lain 编译器进程内闭环完成，不再跨 C ABI 边界；
   - **终局验收**：经历充分验证期并确保 `Stage-1 -> Stage-2 -> Stage-3` 稳定后，归档 `seed/kernel/`。全仓库中唯一保留的 `.c` 文件只剩 `lainc` 自己编出来的 **[bootstrap/lainc_boot.c](seed/bootstrap/lainc_boot.c)**——任何平台只需一行 `cc -std=c99 -O2 bootstrap/lainc_boot.c -o lainc` 即可完成全系统冷启动。
