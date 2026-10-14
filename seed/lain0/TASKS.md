# lain0 任务拆分（供并行 worker 使用）

仓库根：`C:/Users/jenway/codeFiles/lain-lang`（Windows，路径一律用正斜杠）
分支：`feat/lain0`
语言标准：C++20

## 所有人必须遵守的规则

1. **只创建/修改分给你的那几个文件。** 不许改 `include/lain0/*.hpp`、不许改
   `lain0/CMakeLists.txt`、不许改 `seed/**`、不许改 `docs/**`。
   契约已经固定，改契约必须先停下来报告，而不是自己动手改。
2. **不许 `git commit`、不许建分支、不许 checkout。** 工作区由编排者统一收敛。
3. **不许 `#include` seed 的头文件。** 只能 `#include "lain0/Seed.hpp"`（它内部
   做了 `extern "C"` 包裹）。seed 的 23 个头文件全都没有 `extern "C"` 守卫，
   绕过这层会链接失败。
4. **不许拼含 `#` 的 IR 文本。** 一切 IR 经 Seed.hpp 的构造 API 产生。
5. 每个 TU 的自检命令（在仓库根目录跑，不需要别人先把他们的文件写完）：

   ```
   clang++ -std=c++20 -fsyntax-only -Wno-deprecated-anon-enum-enum-conversion \
     -I lain0/include -I seed/include lain0/src/<你的文件>.cpp
   ```

   退出码 0 才算通过。这个命令只吃头文件，不链接，所以可以完全并行。
7. 遇到分给你的范围之外的缺失/错误 → **不要自己补**，在什么都没改的地方写一份
   `lain0/BLOCKERS-<你的任务号>.md` 说明问题，然后结束。
8. 代码注释用中文，风格跟现有头文件一致：解释「为什么」，不解释「这行在做什么」。

## 共同背景

- 这门语言的语义口径在 `docs/04-bootstrap-dialect.md`，**尤其《一、已能用》那张表
  和《待补规则》**，那是唯一的语义依据。实现前先读。
- LAINIR 本身的规则在 `docs/01-lain-ir.md` 与 `docs/02-lain-vm.md`。
- 参考实现已知的行为 trivia 都在 docs/04 里，不必去读 `bootstrap/` 下的手写 IR。
- 诊断码用 `include/lain0/Diag.hpp` 里 `enum class D`，不要自造数字。

---

## T1 — Source / Diag（基础设施）

拥有文件：`lain0/src/Source.cpp`、`lain0/src/Diag.cpp`

实现 `include/lain0/Source.hpp` 与 `include/lain0/Diag.hpp`。

- `SourceMgr`：文件 id 从 1 起。`loadFile` 失败返回 0 并把原因写进 err。
  维护每个文件的行首偏移表，`position()` 二分查找；`lineText()` 返回不含换行符的整行。
  注意 Windows 的 CRLF：`\r` 不要算进文本 std::string_view 的行尾。
- `DiagSink::printAll` 输出形如：

  ```
  examples/arith.lain:3:11: error: 未知的算子 (303)
      let x = a + b;
              ^
  ```

  没有 Span 或 Span 无效时用 `<unknown>` 代替文件位置。

自检：上面第 6 条命令，分别对 `Source.cpp` 和 `Diag.cpp` 各跑一次。

---

## T2 — Lexer（词法）

拥有文件：`lain0/src/Lexer.cpp`

实现 `include/lain0/Lexer.hpp`。

- 标识符：`[A-Za-z_]` 起，`[A-Za-z0-9_]` 续。
- 整数字面量：十进制；`0x` / `0X` 前缀按十六进制读（`0xff00` → 65280 存进
  `Token::intValue`）。数字后面紧跟标识符字符（`0xZZ`、`12abc`）是 `LexBadNumber`。
- 标点：见 `Tok` 枚举。注意 `<=`、`>=`、`==`、`!=`、`<<`、`>>`、`->` 都是双字节，
  必须最长匹配。`&`、`|`、`^` 是单字节。
- 注释：`//` 起至行尾，不产生 token。
- 关键字表：`func let return if else loop for while in acc break continue struct
  enum import extern as and or true false ro`。
- `tokName` 返回稳定英文名字，诊断用。
- `binaryPrecedence` / `isRightAssociative` / `tokBinaryOp`：按 C 风格优先级，
  低到高为 `or` < `and` < `==`/`!=` < `<`/`<=`/`>`/`>=` < `<<`/`>>` < `+`/`-` <
  `*`/`/`/`%`。全部左结合。`tokBinaryOp` 把 `Tok` 映射成 `ast::BinOp`
  （定义在 `lain0/Ast.hpp`），不是二元算子返回 false。

自检：`-fsyntax-only` 通 `Lexer.cpp`。

---

## T3 — Parser（Pratt 解析）

拥有文件：`lain0/src/Parser.cpp`

实现 `include/lain0/Parser.hpp`。读 `docs/04-bootstrap-dialect.md` 的语法部分。

要覆盖的表面语法（全部）：

- 声明：`func NAME(p: T, …) -> T { … }`、`extern NAME(p: T) -> T = link_name;`
  `struct NAME { f: T  f: T }`（**空格分隔，不是逗号**）、
  `enum NAME { V(T)  V }`、`let NAME = import("a::b");`、
  顶层 `let x: T = <值>;`（含 `ro [u8]` 与 `[T]`）。
- 语句：`let NAME: T = <expr>;` / `let NAME = <expr>;`、`return <expr>;`、
  赋值 `<lvalue> = <expr>;`、裸表达式语句、`if <expr> { … } else { … }`、
  `loop LABEL(p: T = v, …) while <expr> { … }`、`for i in <a>..<b> acc = <c> { … }`、
  `break LABEL(v);` / `continue LABEL(v, …);`。（`acc =` 可省。）
- 表达式用 Pratt：`if` 当值用时解析成 `ast::ValueIfExpr`；
  调用、下标、点号（`M.answer`、`p.x`、`Shape.Circle`）是后缀位置；
  `x as T`、`-5`（负号紧跟数字）。

  注意 docs/04 的约定：**`if` 当语句时，分支以 `return` 收尾**；当值时两个分支都以值收尾。

- 出错要尽力恢复：跳到下一个可能的语句起点继续。失败才返回 nullptr。

自检：`-fsyntax-only` 通 `Parser.cpp`。

---

## T4 — Sema（类型 / 作用域 / 布局 / 算子选择）

拥有文件：`lain0/src/Sema.cpp`

实现 `include/lain0/Sema.hpp`。

- 内建标量名单与 `docs/04`《A. 算子表》必须一致：
  `i32 u32 i64 u64 i8 u8 usize bool addr`。`i8`/`u8` 只作 load/store 宽度，**不绑算子**。
  `addr` 是 TY_ADDR（width 无意义），`bool` 按 1 位 bits 处理。
- `TypeStore::resolve`：处理 `M.Point`（限定名）、`[T]`、`ro [T]`、聚合名、标量名。
- 布局：`struct` 逐字段累加（对齐取最大字段宽度与 8 取小者）；
  `enum` = tag（`u8`，偏移 0）+ 载荷（按其类型，偏移按对齐补齐）。
- `selectBinary`：给定 `(BinOp, TypeId)` 选 LAINIR 指令。**这是重点** ——
  有符号走 `INST_SDIV/INST_SREM/INST_SLT/…`，无符号走 `INST_UDIV/INST_UREM/INST_ULT/…`，
  比较类结果恒为 `#bits<1>`。`i8/u8/addr` 没有算子，命中要返回 false。
- `analyze`：遍历模块，填 `SemaResult`：每个表达式的类型、函数返回类型、
  顶层绑定类型、以及局部绑定与参数的名字 → 类型（供无标注 let 推导）。
  import 成员目前只需登记名字与 kind，不需要做跨模块加载。

自检：`-fsyntax-only` 通 `Sema.cpp`。

---

## T5 — LowerExpr（表达式降级）

拥有文件：`lain0/src/LowerExpr.cpp`

实现 `Lower.hpp` 中标为 LowerExpr 的那一组（`lowerExpr` / `lowerBinary` /
`lowerCall` / `lowerBuiltin` / `lowerCast` / `lowerAddressArithmetic`）。

- 宽度来源**只能**是 IR：`lainir_inst_result_type()`（`lain/ir/infer.h`）。
  不许自己在 Meta 里传「期望宽度」，那是 stage 0 那一整类 bug 的来源。
- `p + i` 且左操作数是 addr → 发 `INST_LEA(base, index, 1, 0)`；
  `p + i * K`（K 是无符号十进制字面量）→ K 折进 scale；`i + p` 报错。
- `load(p)` / `store(p, v)` / `lea(p, i)` / `ptr_of(p)` / `addr_of(w)` / `len(t)`
  是**内建名字**，走 `lowerBuiltin`，不是普通 `INST_CALL`。宽度由期望类型定；
  无期望类型的 `load` 必须报错（不许猜宽度）。
- `x as T`：变宽 `INST_ZEXT`、变窄 `INST_TRUNC`、等宽补一条加零复制。
- 栈地址逃逸：`return T { … }` 这类返回**刚构造的栈地址**要报错（对应 D::SemaAddressEscape）。

先做到 Slice A（整数、比较、算术、调用、名字、字面量）能成形，再铺其余。

自检：`-fsyntax-only` 通 `LowerExpr.cpp`。

---

## T6 — LowerStmt（语句与控制流降级）

拥有文件：`lain0/src/LowerStmt.cpp`

实现 `Lower.hpp` 中标为 LowerStmt 的那一组。

- 带值 `#if`：结果是 `(#bits<N>)` 的结构化区域，`then`/`else` 两支都必须以
  `INST_YIELD` 收尾；省略 `else` 时假分支给结果类型的零值。
- `#loop`：区域带 `L1RegionParam`（名字、类型、初值），仅显式 `INST_CONTINUE`
  形成回边；落到区域末尾即顺序离开。
- `for` 降级为等价的 `loop`：循环变量 + 累加器。`acc` 可省（由 `break LABEL(值)`
  带出结果，结果类型取第一个参数的声明类型）。
- `break` / `continue` 必须带标签，实参个数要校验（`break` 恰好 1、`continue`
  等于参数个数）。
- 终结判定：以 `return` / `break` / `continue` 收尾的分支不再要求 `#yield`，
  但它们之后不许再降级语句。

自检：`-fsyntax-only` 通 `LowerStmt.cpp`。

---

## T7 — LowerDecl（顶层声明降级）

拥有文件：`lain0/src/LowerDecl.cpp`

实现 `Lower.hpp` 中标为 LowerDecl 的那一组。

- `func` → `lainir_subroutine`；`extern` → `lainir_subroutine_extern`
  （link_name 原样搬，且它既是能力名也是链接符号名）。
- 顶层字节串 / 定长表 → `lainir_data`。字节原样搬、**不补末尾 0**
  （要结束符请在源码里显式写 `, 0`）。表元素按声明宽度小端拆字节。
- `struct` / `enum` 自身不产出 IR 项，只登记布局（已由 Sema 做）。你要处理的是
  顶层聚合构造：`let p = Pair { a: 1 b: 2 };` → 一块顶层静态 storage + 一个取址 proc。
- 函数体内的聚合构造降级到栈：`INST_ALLOCA`（元素 `#bits<8>`、个数 = 布局字节数）
  + 每字段一条 `INST_STORE`（偏移用 `INST_LEA` 算），绑定接一条 `INST_LEA`。
  **同一 proc 里每个构造点必须用不同的临时名**，重名会静默指向前一个。

自检：`-fsyntax-only` 通 `LowerDecl.cpp`。

---

## T8 — Driver / CLI

拥有文件：`lain0/src/Driver.cpp`、`lain0/tools/main.cpp`

实现 `include/lain0/Driver.hpp`，并给出 CLI。

固定顺序（不许调换）：
`SourceMgr.loadFile → lex → parseModule → analyze → Lowerer.run →
 lainir_verify → lainir_print_to_string`

- verify 不通过就**不产出文本**，把 `L1Diagnostic` 的 code/line/column/message 带出来。
- `CompileResult::ok` 为 true 的充要条件：无诊断错误 **且** verify 通过 **且**
  文本非空。「空产物 + 成功」在 lain0 里不允许存在。
- `roundTripCheck`：把 canonical 文本喂回 `lainir_parse`，再打印一次，要求相等。
- `main.cpp`：`lain0 <file.lain> [-o out.l1]`；无 `-o` 打印到 stdout；
  失败时打印诊断并返回非 0。诊断用 `DiagSink::printAll`。

自检：`-fsyntax-only` 通 `Driver.cpp` 与 `main.cpp`。
