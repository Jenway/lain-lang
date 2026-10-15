# lain0 — C++ 前端计划

> lain0 = 用 C++ 写的 Lain 编译器，直接把 Lain 源码编译为 LAINIR。
> 它**不在 LainVM 里运行**，也不使用 CSpace / apply / region_grant。
> bootstrap（手写 LAINIR Meta）就此冻结，保留作为语义参照，不再是唯一实现。

## 阶段划分

```
stage 0（已冻结）  bootstrap/ 手写 LAINIR Meta，跑在 LainVM
stage 1（本计划）  lain0：C++，Lain 源码 → LAINIR，不碰 VM        ← 现在
stage 2（之后）    Meta 用 Lain 写，被 lain0 编译后在 LainVM 里跑
```

## 为什么不是拼字符串

seed 已经有一整套可用的 IR 构造 + 验证 + 打印 API（`seed/include/lain/ir/`、`seed/include/lain/text/`），
全部是 PUBLIC 的。lain0 的正确形态是：

```
Lain 源码 → lexer/parser → AST → 语义层 → L1Builder 在内存搭 IR
         → lainir_verify() → 通过才 lainir_print_to_string()
```

带来的四条硬性质：

1. **不可能产出非法 IR** —— 不过 verify 就到不了打印。
2. **print/parse 已闭环** —— `text/print.h` 与 `text/parse.h` 定义了 canonical 固定点
   `print(parse(text)) == text`，走 printer 的输出天然进得了闸门。
3. **诊断有位置** —— `L1Diagnostic` 带 line/column，且 `L1Inst` 本身就带 line/column 字段。
4. **宽度推导不用重写** —— `ir/infer.h` 是 docs/01 规定的「宽度唯一来源」，
   直接 `lainir_inst_result_type()`，消灭 bootstrap 时代那一整类「期望宽度传播」bug。

## 目录

```
lain0/
  include/lain0/
    Seed.hpp      extern "C" 边界（务必经它 include seed）
    Source.hpp    源文件管理、Pos/Span
    Diag.hpp      诊断 sink、诊断码
    Lexer.hpp     token
    Ast.hpp       AST 节点 + arena
    Parser.hpp    Pratt 解析
    Sema.hpp      作用域、类型表、布局
    Lower.hpp     AST → L1Builder
    Driver.hpp    编排 CLI
  src/*.cpp
  tools/main.cpp CLI：lain0 <file.lain> [-o out.l1] | lain0 --unit <清单>
  CMakeLists.txt
```

## 关键约束（写 lain0 必须遵守）

- **不能直接 `#include` seed 头文件**，一律 `#include "lain0/Seed.hpp"`。
  （实测：23 个 seed 头文件里 `extern "C"` 守卫数量 = 0。）
- **源码里不出现 `#` 形态**。这是 docs/04《判据》第一条，不是洁癖。
- 所有名字串用 `lainir_builder_string()` 驻留，不能直接传栈/临时字符串。
- 诊断码用 lain0 自己的号段，不许占用 `codes.h` 里已登记的 1xxx/2xxx/3xxx/9xxx。

## 垂直切片优先（push 的准线）

**Slice A（第一次 push 的准线）** —— 能端到端编译 `bootstrap/lain/examples/` 的
`arith.lain`、`flow.lain`、`inc/bump/lit` 一类最小样例，产物过 `lainir_parse + lainir_verify`：

- lexer / parser：func、param、return、let（标注可省）、if-语句、`+ - * / %`、`< > <= >= == !=`、整数字面量
- sema：标量表（i32/u32/i64/u64/usize/bool），宽度推导走 `infer.h`
- lower：表达式、调用、return、let、if
- driver + CLI

**Slice B（之后）** —— 补齐 `docs/04-bootstrap-dialect.md`《一、已能用》的全部行：
带值 if、loop/for + 标签/参数/break/continue、struct/enum（声明体、字段访问、变体投影、
函数体内构造降级到 `#alloca`）、`load/store/lea`、`ptr_of/addr_of`、`as`、负数字面量、
十六进制、顶层静态数据（字节串 / 定长表 → `#data`）、`extern`、`import` 成员访问。

## 任务拆分

| ID | 拥有文件 | 内容 | 依赖 |
|---|---|---|---|
| T1 | 全部 header + CMake | 契约（已完成者 RM 本人） | — |
| T2 | `src/Lexer.cpp` | 词法：标识符、整数、十六进制 `0x`、标点含 `<< >> ->`、注释；UTF-8 无关 | T1 |
| T3 | `src/Parser.cpp` | Pratt 表达式 + 语句/声明解析 | T1 |
| T4 | `src/Sema.cpp` | 作用域、类型表、struct/enum 布局、import 解析 | T1 |
| T5 | `src/LowerExpr.cpp` | 表达式降级（含算子表、宽度、`p+i*K` → `#lea`、`as`、构造器） | T1,T4 接口 |
| T6 | `src/LowerStmt.cpp` | 语句与控制流降级（if / loop / for / jump / return） | T1,T4 接口 |
| T7 | `src/LowerDecl.cpp` | 顶层：func / extern / struct / enum / import / 静态数据 | T1,T4 接口 |
| T8 | `src/Driver.cpp` + `tools/main.cpp` | 编排、verify、print、CLI | T1 |
| T9 | `tests/` | 回归 runner + 样例比对 | T8 |

T5/T6/T7 可以并行开发（接口由 T1 的 header 固定），集成在之后统一做。

## 模块命名空间与 `--unit`（已落地）

```text
lain0 <file.lain> [-o out.l1] [--dump-tokens] [--dump-ast] [--round-trip]
lain0 --unit <清单> [-o out.l1] [--round-trip]
```

- **模块键** = 被解析到的文件相对**源根**的路径去 `.lain`、`/` 换 `_`（`bootstrap/std/lex.lain`
  → `std_lex`，`bootstrap/meta.lain` → `meta`）。源根：`--unit` 时是清单文件所在目录，
  单文件入口时是入口文件所在目录；规范化后跑出源根、或键里含 `__`，都报 312。
- **符号命名**：每个模块的顶层声明发 `<键>__<名字>` —— `func` 是 `<键>__f`，静态数据是
  `data <键>__x_storage` 加访问器 `#proc <键>__x()`，类型表键是 `<键>__<类型名>`。清单里标为
  **根名**的少数声明保持平坦（宿主按平坦字符串找入口，`bootstrap/SOURCE_ORDER` 只给 `meta` 的
  三个 `lain_std_*` 标根名），宿主 `extern` 的 `link_name` 照旧平坦。
- **跨模块访问**一律写 `M.名字`：`M.f(x)` 是调用、`M.NAME` 是访问器调用、`M.table[i]` 给元素地址、
  `p: M.Point` 的字段访问可用；`symbol_addr("X_storage")` 只解析**本模块**的静态数据，跨模块写
  别的模块的数据名报 301（消息里给出 `M.<名字>` 的写法）。
- **清单格式**：每行 `<相对路径> [<根名> …]`，路径相对清单所在目录、省略 `.lain`，`#` 起注释；
  清单顺序即降级顺序，同一份清单必须给出逐字节相同的模块（`bootstrap/SOURCE_ORDER` 就是这样一份：
  28 行、`meta` 行带三个根名，连跑两次产物 SHA-256 相同）。
- `--unit` 与位置参数互斥（两者都给或都不给 → 退出码 2）；清单打不开 → 退出码 2 并打印
  `lain0: 打不开清单 <路径>`。成员位置表与实测拒绝码见 `docs/04-bootstrap-dialect.md` 的
  《模块：成员位置与符号命名》，整份设计的取舍见 `docs/plan-modules.md`（决策 D-004）。

## 验收

1. `build/` 出 `lain0.exe`
2. 对 `bootstrap/lain/examples/*.lain`（38 个）：产出文本能 `lainir_parse` + `lainir_verify` 通过
   （当前 **38/38**，`bash scripts/regress.sh` 与 `--round-trip` 同）
3. **不需要与 bootstrap 产物逐字节相同** —— 但语义结构必须可比（过程集、专有或不 associated 指令类别一致）
4. `check_unit` 角色由 lain0 内建的 verify 步骤取代

## 分支

`feat/lain0`（不直接动 master）。stage 1 全部收敛在里面，push 后人工 review。
