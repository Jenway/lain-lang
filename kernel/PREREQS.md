# Kernel 前置语言 / 编译器 / 标准库改动清单

目标：使 [kernel/ir/model.lain](ir/model.lain) 与 [kernel/vm/trap.lain](vm/trap.lain) 及后续 `kernel/` 模块能以纯现代 Lain、零迁就的方式直接编译运行。

---

## 一、编译器前端（`src/`，按 Step A 滚动自举落地）

- [ ] **1. `pub { ... };` 导出块语法**
  - **位置**：`src/sema/eval.lain`（顶层声明循环与 `module { ... }` 内声明循环）
  - **要求**：当 `pub` 后紧跟 `{ ... }` 时，遍历块内所有声明并自动标记 `is_pub = 1`；兼容现有 `pub let`。

- [ ] **2. `::` 静态路径分隔符**
  - **位置**：`src/driver/sym.lain`（公开 `p_dcolon`）、`src/sema/member.lain`、`src/sema/ctx.lain`、`src/sema/effects/lower.lain`
  - **要求**：`reader.lain` 已将 `::` 切为单个 token。在以下静态路径位置支持 `::`（Step A 期间兼容 `.`）：
    - 模块成员访问：`str::eq`、`io::println`
    - 函子静态调用：`Vec(DataSeg)::new()`、`Option(u64)::some(i)`
    - 枚举变体与嵌套类型：`Trap::Reason::Unreachable`、`Ty::Bits`
    - 效应操作：`perform VmTrap::trap(...)`

- [ ] **3. `use import("...")::{ A, B };` 导入语法**
  - **位置**：`src/sema/eval.lain`、`src/comptime/ctfn.lain`
  - **要求**：让 `use` 支持任意求值为 `Module` 的表达式（含 `import("...")`）配合 `::{ A, B }` 选择性导入，将导入的符号（含函子 `Vec`/`Option`、效应 `OomErr`）登记到当前作用域，保证裸名 `Vec(T)` / `Option(T)` 在类型位置和表达式位置均可直接解析。

- [ ] **4. 匿名函子实例的默认 UFCS**
  - **位置**：`src/sema/witness.lain`、`src/sema/member.lain`
  - **要求**：解析 `v.method(...)` 时，若当前作用域显式 `use` 未命中，自动回退查找 `typeof(v)` 对应 `Shape` 的定义模块 `agg.shp_mod(sh)`，使 `m.datas.push(...)`、`m.procs.len()` 无需 `let DataVec = ...; use DataVec;`。

- [ ] **5. `enum`（tagged union）补全**
  - **位置**：`src/sema/`（`match` 降级与二元比较逻辑）
  - **要求**：
    1. 修复嵌套 `match` 触发 `ir_host: state mismatch` 的内部崩溃；
    2. 为值语义 `enum`（tagged union）实现内置结构相等 `==` / `!=`（tag 相等且各负载字段逐一相等）。

- [ ] **6. 验证嵌套 `Vec` 字段的自动递归 `drop`**
  - **位置**：`src/sema/agg.lain`（`agg.lain:222, 599` 已有递归 drop 逻辑）
  - **要求**：验证 `IrModule -> Vec(Proc) -> Proc -> Vec(Inst)` 多层嵌套值语义移动与作用域退出自动析构无误；如发现边界缺陷一并修复。

---

## 二、标准库（`std/`）

- [ ] **7. `Vec` 自身传播 `!{ OomErr }`**
  - **位置**：`std/collections/vec.lain`
  - **要求**：`reserve` / `push` 在分配失败时直接 `perform OomErr::oom(...)`，不再返回 `bool` 甩给调用方；需要不抛效应版本时另提供 `try_push -> bool`。

- [ ] **8. `std::algo` 补齐 `find_if` 并清理旧迁就**
  - **位置**：`std/algo.lain`
  - **要求**：去掉 `OptIdx` / `R: Type` 迁就，直接返回 `Option(u64)`，并补充按谓词查找的 `find_if`（替代上层手写线性查找循环）。

---

## 验收标准

1. `kernel/ir/model.lain` 与 `kernel/vm/trap.lain` 原样通过 `.\build\lainc_stage2.exe` 编译且 `.\build\lain_tidy.exe` 零告警；
2. `.\build\build_runner.exe --bootstrap` 全量自举不动点（`stage2.l1 == stage3.l1`）与现有测试套全部通过；
3. 刷新 `bootstrap/lainc_boot.c`。
