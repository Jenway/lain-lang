# lain0 — C++ 前端

> lain0 = 用 C++ 写的 Lain 编译器，直接把 Lain 源码编译为 LAINIR。
> 它**不在 LainVM 里运行**，也不使用 CSpace / apply / region_grant。
> `bootstrap/` 是 Lain 写的 Meta 库，由 lain0 编译成 LAINIR 后交给 LainVM 执行。

## 编译管线

seed 已经有一整套可用的 IR 构造 + 验证 + 打印 API（`seed/kernel/include/lain/ir/`、
`seed/kernel/include/lain/text/`），全部是 PUBLIC 的：

```text
Lain 源码 → lexer/parser → AST → 语义层 → L1Builder 在内存搭 IR
         → lainir_verify() → 通过才 lainir_print_to_string()
```

带来的四条硬性质：

1. **不可能产出非法 IR** —— 不过 verify 就到不了打印。
2. **print/parse 已闭环** —— `text/print.h` 与 `text/parse.h` 定义了 canonical 固定点
   `print(parse(text)) == text`，走 printer 的输出天然进得了闸门。
3. **诊断有位置** —— `L1Diagnostic` 带 line/column，且 `L1Inst` 本身就带 line/column 字段。
4. **宽度推导不用重写** —— `ir/infer.h` 是 docs/01 规定的「宽度唯一来源」，
   直接 `lainir_inst_result_type()`，不存在「期望宽度传播」那一类 bug。

## 目录

```text
seed/lain0/
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

## 关键约束

- **不能直接 `#include` seed 头文件**，一律 `#include "lain0/Seed.hpp"`。
- **源码里不出现 `#` 形态**。这是 docs/04《判据》第一条。
- 所有名字串用 `lainir_builder_string()` 驻留，不能直接传 stack 上的临时字符串。
- 诊断码用 lain0 自己的号段，不许占用 `codes.h` 里已登记的 1xxx/2xxx/3xxx/9xxx。

## 模块命名空间与 `--unit`

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
  三个 `lain_std_*` 标根名），宿主 `extern` 的 `link_name` 同样保持平坦。
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

1. `build/` 出 `lain0.exe`。
2. 对 `bootstrap/lain/examples/*.lain`（38 个）：产出文本能 `lainir_parse` + `lainir_verify` 通过
   （当前 **38/38**，`bash scripts/regress.sh` 与 `--round-trip` 同）。
