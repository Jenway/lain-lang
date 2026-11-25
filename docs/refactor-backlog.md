# Lain-lang 重构与清理清单（已核实基线）

> 以下条目均基于当前仓库 `rg` 与三份全库只读类型审计整理，精确到 `file:line`：
> - 后端/IR/VM/Host/Driver：[audit_types_1.md](file:///C:/Users/jenway/.gemini/antigravity-cli/brain/18a10c66-627e-4690-a90f-48f7dc7e507a/scratch/audit_types_1.md)
> - 前端/语法/Sema/Comptime/Meta：[audit_types_2.md](file:///C:/Users/jenway/.gemini/antigravity-cli/brain/18a10c66-627e-4690-a90f-48f7dc7e507a/scratch/audit_types_2.md)
> - 标准库/Tools/Examples/Tests：[audit_types_3.md](file:///C:/Users/jenway/.gemini/antigravity-cli/brain/18a10c66-627e-4690-a90f-48f7dc7e507a/scratch/audit_types_3.md)

## 全库六类类型/设计问题汇总（实测统计）

1. **假整型常量 getter（`func() -> u64/i64 { return N; }`）**：共 **~339 处**（`vm/driver/host/ir` ~150、`sema/comptime` 136、`std/tools/tests` 53）
2. **模块内 C 式重复前缀类型名**：共 **81 个类型**（`Vm*`/`Host*`/`Ir*`/`C99*` 27、`Ct*`/`Ast*`/`SigRec` 24、`Coro*`/`Json*`/`Fmt*`/`Sched*` 30）
3. **裸 `struct` + 全局过程（未用 `module { pub let Self = struct ... }`）**：**>110 个结构体、700+ 个顶层过程**（`driver/state` 130、`vm/engine/state` 50、`vspace` 45、`image` 40、`ir_host` 60、`sema` 56 struct / ~330 fn、`std/tools` 33 struct）
4. **多余二次枚举/标签转换（`enum -> enum` / 裸数字 `switch`）**：`ops.lain`（`IntBinOp`/`IntCmpOp`/`IntConvOp`/`OpClass` 已清）、`c99/inst.lain`（6 组平行算术/比较枚举已清）、`sema`（`lk_of_shape`、`conv_kind`、`mode_of`、52 处 `CtVal.kind` 裸数字比较）、`std`（4 份 `kind_code`/`delim_code`、42 处标签转换）
5. **1 行转发门面 & 重复手搓工具**：`vm/engine.lain`（96/96）、`backend/c99.lain`（38/38）、`sema/effects.lain`（42）、`sema` 局部 1 行转发器（264）、`driver/host_rt`（31+）、`std/collections.lain` & `std/meta.lain` 兼容门面；5 份 `Out` 缓冲、5 份十进制格式化、4 份 JSON 转义、2 份手写 `argv` 解析
6. **裸 `u64` 假冒指针/句柄/`bool` & 全局 1 元素数组单例**：191 个返回 `u64` 的 `is_*`/`has_*` 谓词、47 个 `u64` 布尔字段、`std/os` `eprint`/`print -> ()` + `std/mem` 裸 `u64` 指针、55 处 `+8`/`+16`/`+24`/`+56` 裸偏移、`Array(T, 1)` 全局单例（`g_syms`、`drain_gate`、`g_rt`、`g_pidx_meta`、`g_inl_meta` 等）、以及 `sema` 缺失 `Never` 发散导致的 `trap_mut`/`die_*`/`dead_*` + `let dead` 死代码

---

## P0：错误处理与 `std/mem` 违例清理（当前阻塞项）

### 1. 彻底移除 `mem.halt_now`（47 处调用点 -> `Trap` / `Result` / `Option`）
`std/mem.lain` 中的 `halt_now` 已删除，以下剩余调用点需全部迁移到 `mem.trap_abort`（`!{ Trap }`）、`Result`/`Option` 或明确错误返回：
- `std/mem.lain:47, 203`（`carve` 内部、`BumpArena`）
- `std/collections/map.lain:102`
- `std/fmt.lain:35`
- `std/sched.lain:119`
- `std/meta/ast.lain:18, 36, 71, 79, 84`
- `std/meta/out.lain:72, 77, 156, 181, 193, 210, 312, 317, 322, 328`
- `std/meta/reflect.lain:58`
- `std/meta/view.lain:14, 85, 87`
- `src/driver/host_rt.lain:17, 23, 751`
- `src/driver/inspect.lain:29, 37`（随 `inspect` 剥离一并消除）
- `src/ir/emit_text.lain:9`
- `src/meta/ast.lain:32, 50`
- `tools/lain_lsp.lain:22, 42, 57, 185, 713, 729, 772, 799, 867, 932, 1137, 1620, 1650, 1657, 1688`（15 处）

### 2. 删除 `std/mem.lain` 的 `carve`
- `std/mem.lain:41`：`pub let carve`（裸 `u64` 强转读写头字做偏移）
- 唯一外部调用方：`std/build.lain:42`（改用 `mem.BumpArena` / `mem.Allocator`）

### 2.5 `src/vm/engine/ops.lain` & `state.lain` 明显反模式（用户已标出）
- `src/vm/engine/ops.lain`：`int_bin_op` / `int_cmp_op` 把 `codes.InstKind`（`enum(u64)`）先转成 `IntBinOp` / `IntCmpOp` ADT 再 `match`，直接去掉中间枚举，直接匹配 `InstKind`
- `src/vm/engine/state.lain`：
  - `VM_*` / `SLICE_*` / `TRAP_*` / `ST_*`（约 100 个 `func() -> u64` 常量过程）全部改为 `enum(u64)`
  - C 式 `struct` + 全局 `*_new` 函数（`VmQuota`/`quota_new` 等）全部改为 `module { pub let Self = struct { ... }; pub let new = ...; }` 封装，并按实体拆分出 `state.lain`

---

## P1：架构边界与模块拆分

### 3. `inspect` 从 `src/driver/` 剥离为独立库 + `src/compiler.lain` 查询接口
- 删除 `src/driver/inspect.lain`（消除 16 MiB 裸指针 `Out`、`mem.halt_now`、12 个假 `-> u64` 返回与 `let sp/op/t/f: u64 = ...` 垃圾接收、尾递归 `i + 1`、ASCII 魔数、`ModCell`/`SymCell` 裸算偏移）
- 从 `src/driver/cli.lain` 剥离 `--dump-deps` / `--dump-docs`，保持 `lainc` 为纯编译器
- 新增只读编译器查询门面 `src/compiler.lain`（暴露类型化的模块、依赖边、导出符号查询）
- 将依赖图/文档生成实现为可复用库（`tools/inspect.lain`）+ `std::cli` 驱动的独立 CLI（`tools/lain_inspect.lain`），使用 `io.Sink`（`sig { write: func(s: Slice(u8)) -> (); }` + `?{ out: io.Sink }` witness，支持 `StdoutSink`/`FileSink`/`BufSink`）、`while`、`-> ()`

### 4. `src/vm/engine/state.lain` 按领域实体拆分与去 C 式命名
- 去掉 C 式类型/函数前缀（如 `VmCapTable` -> `CapTable`、`VmEndpoint` -> `Endpoint`、`VmQuota` -> `Quota`、`VmTraceEvent` -> `TraceEvent`、`VmValue` -> `Value`，`quota_new`/`ep_new` -> `Quota.new`/`Endpoint.new`）
- 用 `module { pub let Self = struct { ... }; ... }` 封装实体方法，从笼统的 `state.lain` 拆出独立子模块（`tcb.lain`、`caps.lain`、`ipc.lain` 等）
- 后续同步消除其他子系统的同类 C 式重复前缀（如 `HostSymTab`、`HostSources`、`HostIr` 等）

### 5. 跨模块重导出能力（消除 29 处直接耦合 `src::ir::ir_codes`）
- **编译器能力缺口（Step A -> Step B）**：支持 `pub let Alias = sub_mod.Enum;`（及模块透传 `pub let sub = import(...)`）跨模块访问变体（解决 `error: unbound module member`）与函数值别名导出
- 然后在 `src/ir/ir.lain`（或 `src/ir/mod.lain`）建立统一 IR 包边界，让 `sema/`、`comptime/`、`vm/`、`backend/c99/`、`host/` 统一通过 `ir` 引用，不再直接 `import("src::ir::ir_codes")`

---

## P2：残余兼容转发层与假 `-> u64` 清理

### 6. 消除剩余 1 行转发门面
- `src/sema/agg.lain:53-64`：12 个 `pub let kind_* = func() -> u64 { return ShapeKind.* as u64; };` 迁到 `ShapeKind.*` 并删除转发器
- `src/vm/engine.lain`（~97 个 `st.*` 1 行转发函数）、`src/backend/c99.lain`（~38 个）、`src/sema/effects.lain`（~42 个）：依托 P1.5 重导出能力或直接导入子模块彻底消除
- `src/ir/emit_text.lain:23`：`let seen: u64 = tab.trap_report(...)` 改为 `-> ()` 并删掉 `trap_report` 假返回（同步 `src/host/symtab.lain`）

### 7. 假 `-> u64` 过程、尾递归、重复小函数与 `loop` 清理
- 全仓大量仅返回 `0/1` 的过程改 `-> ()`，清除 `let ignored/dead/seen: u64 = ...` 垃圾绑定
- 复用 `std::cli` 替代各工具手写 `argv` 解析（`tools/lain_lsp.lain`、`tools/ast_codemod.lain`、`tools/lainfmt.lain`）
- 在 `std/fmt.lain` 沉淀可复用的可增长文本写入器（消除 `inspect` / `lain_lsp` 各自手搓的 `Out`）
- 利用已验证的 [tools/ast_codemod.lain](file:///D:/codeFiles/lain-lang/tools/ast_codemod.lain)（`--pass loop-to-while`）分批清理 60+ 文件中的无引用标签 `loop`
- 统一重复底层辅助函数（`word_at`/`u64_at`、`copy_bytes`、`align*`、`bytes_eq`、`byte_at`）与魔法偏移算术

---

## P3：工具链、增量编译与编辑器体验

### 8. `lain-tidy` 规则检查与 `--fix` 引擎
- 基于 `reader.RawAst` + `Sema` 符号/类型/Effect 信息 + 逆序无冲突 `FixIt` 补丁器，实现结构层（L1）、语义层（L2：假 `-> u64`、未读丢弃绑定、`bool` 谓词、同前缀 `enum(IntTy)`）与安全层（L3：魔法步长、裸指针）规则，并可向 `lain_lsp` 提供实时诊断与 QuickFix

### 9. 真正的模块级增量编译（`Wave H` 剩余部分）
- 为每个模块生成 `.iface`（导出签名/类型/effect 哈希），上游 `.iface` 未变时跳过下游重编译，配合分模块 IR/C99 缓存实现秒级增量构建

### 10. `lain_lsp` 与 Zed 扩展持续完善
- `lain_lsp`：补齐 `textDocument/references`（查找引用）、`textDocument/rename`、以及接入 `sema` 完整类型错误诊断（目前仅含 `reader` 词法诊断）
- `editors/zed-lain/`：将 Zed 编译产物（`extension.wasm`、`grammars/`）从 git 跟踪中移出并加入 `.gitignore`，补全 `highlights.scm` / `outline.scm` 细节
