# LAIN-IR 编译器路线图

> 计划版本：2026-09-02
>
> 本文只规划 LAIN-IR 编译器，不规划 Lain 前端、Meta 高级特性、标准库迁移、
> 所有权或 borrow checker。

## 1. 固定目标

唯一要完成的链条是：

```text
seed/（C 版 LAIN-IR 解释器源码）
    -> lainir-seed.exe 执行 seed/lainir/compiler.l1
    -> 生成 zig-out/bin/lainir-compiler.exe
    -> lainir-compiler.exe 编译 LAIN-IR 源文件
    -> 生成 C
    -> C 编译器生成最终程序
```

seed 负责底层能力：

- LAIN-IR 文本解析。
- LAIN-IR 验证。
- `#eval` 的解释执行。
- IR 对象的创建、持有和释放。
- 文件、内存、诊断和 artifact 输出。

`compiler.l1` 负责编译流程：

- 请求输入源文件。
- 调用 seed 的解析和验证接口。
- 遍历解析后的 IR。
- 生成确定性的 C。
- 把错误状态传回命令行。

## 2. 当前真实状态

### 已完成

- [x] seed 的 C 工程可以构建 `lainir-seed.exe`。
- [x] `scripts/run_lainir_self_host.py` 固定把产物写入仓库根目录
      `zig-out/bin/lainir-compiler.exe`。
- [x] seed 可以执行 `seed/lainir/compiler.l1` 并生成独立的编译器。
- [x] 生成的编译器可以再次编译自己的 LAIN-IR 源文件。
- [x] 连续两次自举生成的 C 内容一致。
- [x] seed 提供 IR 的不透明句柄和只读访问器。
- [x] `bootstrap.eval_source` 和 `bootstrap.eval-next` 已接入 seed 的解释器，
      `#eval` 的标量结果可以返回给编译流程。
- [x] native/bootstrap host 在入口处预先创建并验证 module handle；后续
      `validate-source` 复用这个 handle，不再为同一输入重复解析。
- [x] `compiler.l1` 中重复实现的简化求值器已经删除。
- [x] `compiler.l1` 已拆为 `seed/lainir/compiler_parts/` 下的职责文件，
      `SOURCE_ORDER` 可以无损重建入口文件。
- [x] `compiler.l1` 已移除私有 parser、IR 记录布局、verifier 和旧 C emitter；
      当前只保留 seed 句柄访问、C 输出和命令入口。
- [x] 常量返回、普通算术、函数调用和标量 `#eval` 样例已经验证。
- [x] seed-backed emitter 已覆盖第一批结构化程序：嵌套 `#if`、常量大小
      `#alloca`、`#lea`、typed `#store`/`#load` 均可生成并运行 C。

### 尚未完成

- [x] `compiler.l1` 不再维护自己的 parser、IR 记录和 verifier 调度逻辑。
- [x] 编译器入口全面遍历 seed 创建的 module/procedure/block/instruction/expression
      句柄；当前 emitter 覆盖常量、算术、调用、分支、循环、内存和标量 `#eval`。
- [x] seed 的句柄 API 已定义空值、越界和借用生命周期约定；诊断通过
      `L1Diagnostic` 只读访问器读取，module handle 由 seed 统一释放。
- [x] 生成编译器的 host 依赖、错误退出码和资源释放已形成文档。
- [x] `scripts/check_lainir_compiler.py` 已整理成单一、短小的回归入口；它在
      临时目录生成常量、算术、函数调用、`#eval`、unit 和非法输入，不留下
      fixture 产物。

## 3. 设计约束

### 3.1 一份 IR

解析结果只能有一个所有者：seed。

`compiler.l1` 只保存句柄，不手工分配或复制 IR 节点。句柄对应的对象由
seed 创建，由 seed 释放。编译器只能通过访问器读取内容。

### 3.2 句柄不暴露布局

不得在 L1 代码中用 `#load` 读取 seed 的 C 结构体字段。所有读取都经过命名
访问器，例如：

```text
module_first
procedure_first
block_first
expr_kind
expr_type
expr_left
expr_right
expr_next
diagnostic_message
```

访问器遇到空句柄、错误类型或越界索引时，必须返回明确的失败结果，不能让
编译器进程崩溃。

### 3.3 `#eval` 只有 seed 执行

`compiler.l1` 不实现表达式解释器。它只负责：

```text
#eval 源码
    -> seed parser/verifier
    -> seed lainir_eval_block / lainir_fold_module
    -> 标量结果或诊断
    -> compiler.l1 读取结果并继续生成 C
```

第一阶段只承诺 `bits`、`unit` 和诊断；地址、函数值、AST/IR 值列入后续阶段。

### 3.4 host 只提供机制

C host 可以提供文件、内存、解析、验证、解释执行和 artifact I/O 的调用入口。
host 不实现 Lain 语义，也不维护第二套编译器逻辑。

## 4. 分阶段实施计划

### 阶段 A：冻结可工作的基线

目标：每次后续修改都有一个短小、可重复的起点。

- [x] 固定 seed 构建命令和两个 Zig cache 路径。
- [x] 固定根目录产物路径，只允许 `zig-out/bin/lainir-seed.exe` 和
      `zig-out/bin/lainir-compiler.exe`。
- [x] 自举脚本不再使用 `lainir-c-gen1`；它会把 seed 构建出的解释器同步到
      根目录 `zig-out/bin/lainir-seed.exe`，后续正式产物只从根目录取。`seed/zig-out`
      仍是 Zig 的内部构建目录，不作为交付路径。
- [x] Windows 链接器产生的同名 `.pdb` 只作为临时调试文件，脚本在正式产物生成后
      清理它；根目录 `zig-out/bin` 的交付内容保持为两个 exe。
- [x] 保存四个最小输入：常量返回、算术、函数调用、`#eval`。
- [x] 验收：seed 构建通过；自举通过；四个输入各自生成可编译的 C。

### 阶段 B：完成 seed IR 句柄 API

目标：让 L1 代码有足够的信息生成 C，但不依赖 C 结构体布局。

涉及文件：

- `seed/include/lainir/core.h`
- `seed/include/lainir/interpreter.h`
- `seed/include/lainir/parse.h`
- `seed/include/lainir/verify.h`
- `seed/src/core/lainir.c`
- `seed/src/text/parser.c`
- `seed/src/text/emitter.c`

任务：

- [x] 明确 module、procedure、block、instruction、expression、type、
      diagnostic 的句柄类型和空值规则。
- [x] 补齐 module/procedure/block 的链表访问器。
- [x] 补齐表达式种类、类型、左右操作数、调用参数和名称访问器。
- [x] 补齐指令种类、值、条件、分支块和循环块访问器。
- [x] 补齐 store 目标访问器；store 的目标地址不再借用 `condition` 槽位。
- [x] 补齐类型种类、位宽、参数类型和返回类型访问器。
- [x] 补齐诊断消息、位置和状态访问器。
- [x] 统一对象释放入口，释放顺序由 seed 管理。
- [x] 为每个访问器添加空值、越界和错误类型行为说明；句柄查找在 bootstrap
      和 native host 两侧都会先验证归属，再调用 seed 访问器。

验收：用一个 C 小程序调用这些 API，遍历一个包含过程、局部变量、调用、
分支和 `#eval` 的模块；错误输入只能返回诊断，不能访问越界内存。

### 阶段 C：整理 seed 的解析、验证和 `#eval` 调用边界

目标：形成 compiler 唯一需要调用的三个入口。

推荐接口形态：

```text
parse(source) -> module_handle + diagnostic
verify(module_handle) -> status + diagnostic
eval(module_handle/block_handle) -> result_handle + diagnostic
```

任务：

- [x] 把当前分散的 parse/verify/fold 调用整理为稳定入口。
- [x] 明确 `#eval` 的执行范围，只收集外层 `#eval` 结果。
- [x] 明确解释步数、递归深度、分配量和诊断限制；seed 提供执行步数、调用深度、
      分配字节数和 `#eval` 次数上限接口，bootstrap 通过环境变量设置前两类预算。
- [x] 明确连续多个 `#eval` 的结果顺序和消费规则。
- [x] 明确 `bits` 的有符号/无符号宽度转换规则：IR 内部统一保存低 64 位；
      `#sext`、`#zext`、`#trunc` 在 seed interpreter 中按目标宽度处理，C emitter
      按声明宽度选择固定宽度整数类型。
- [x] 让所有失败路径释放 module 和临时结果。

验收：同一个模块含两个 `#eval` 时，结果按源顺序返回；错误 `#eval` 有诊断；
超出限制时返回可识别状态；进程不增长占用内存。

### 阶段 D：把 `compiler.l1` 变成纯编译流程

目标：删除第二套 parser/IR，只保留调用 seed 和输出 C 的代码。

文件范围：

- `seed/lainir/compiler_parts/parser_interface.l1`
- `seed/lainir/compiler_parts/ir_access.l1`
- `seed/lainir/compiler_parts/c_emitter.l1`
- `seed/lainir/compiler_parts/integer_literal.l1`
- `seed/lainir/compiler_parts/command_entry.l1`

任务顺序：

1. [x] 保留命令入口、源文件读取和错误输出。
2. [x] 为 seed parser、verifier、`#eval` 增加 L1 外部声明。
3. [x] 让入口取得 module handle，并在失败时输出 seed diagnostic。
4. [x] 用访问器遍历 procedure、block、instruction、expression 和 type。
5. [x] 把 C emitter 接到这些访问器上。
6. [x] 删除 L1 自己的 lexer、parser 状态、IR 节点布局和 verifier 重复逻辑。
7. [x] 删除只服务于旧 IR 的复制、替换和名称猜测函数。
8. [x] 保留自然职责文件名，不重新引入数字前缀文件名。

验收：`compiler.l1` 中不再出现私有 IR 节点分配；不再通过裸内存偏移读取
IR；parser 和 verifier 的行为来自 seed；四个最小输入的 C 输出保持不变。

### 阶段 E：完成 `#eval` 编译路径

目标：确认编译器生成的常量确实来自 seed 解释器。

任务：

- [x] 识别表达式中的 `#eval` 节点。
- [x] 调用 seed eval 入口，不在 L1 中重算表达式。
- [x] 读取 `bits` 结果并按目标类型生成 C 字面量。
- [x] 处理 `unit` 结果：seed 保留已执行的 unit `#eval` 节点，emitter 输出
      `((void)0)`；解析/验证/执行失败通过诊断和非零状态返回。
- [x] 禁止把运行时表达式误当成编译期结果。
- [x] 增加结果消费计数和资源释放。

验收输入：

```lainir
#proc main() -> #bits<32> {
  #let %x: #bits<32> = #eval { #return 40 }
  #return #add(%x, 2)
}
```

要求生成的 C 返回 `42`；执行失败时由 seed 诊断返回，编译器不自行解释表达式。

### 阶段 F：固定自举构建和 host 边界

目标：seed 只做第一次生成，之后统一使用正式编译器。

任务：

- [x] 保持 `scripts/run_lainir_self_host.py` 为唯一自举入口。
- [x] 第一阶段生成 `zig-out/bin/lainir-compiler.exe`。
- [x] 第二阶段由该可执行文件编译自身源文件。
- [x] 第三阶段再次编译同一源文件并比较 C 字节。
- [x] native host 只保留 source、memory、diagnostic、eval 和 artifact 能力。
- [x] 失败时清理临时目录和 module handle。
- [x] 在文档中写清楚 exe 的命令行参数和输入输出约定。

验收：删除临时 stage 文件后，根目录的两个正式 exe 仍可独立使用；连续两次
编译同一源文件的 C 输出完全一致。

### 阶段 G：短小回归和收尾

目标：用分钟级检查替代长时间全量回归。

固定验收顺序：

也可以直接运行：

```text
python scripts/check_lainir_compiler.py
```

1. 构建 seed。
2. 生成 `lainir-compiler.exe`。
3. 编译常量返回样例。
4. 编译算术样例。
5. 编译函数调用样例。
6. 编译 `#eval` 样例并运行生成程序。
7. 用生成的编译器编译自身源文件。
8. 比较两次生成的 C 文件。
9. 编译 unit `#eval` 的 module 样例。
10. 确认非法输入返回诊断且不留下 artifact。
11. 对修改过的 C 文件执行 `git diff --check`。

不把 Lain 前端、`src/lainc`、标准库迁移或大型 compiler closure 放入这条
回归入口。

## 5. 完成定义

只有同时满足以下条件，才称为“LAIN-IR 编译器完成”：

- `lainir-compiler.exe` 固定生成在仓库根目录 `zig-out/bin`。
- `compiler.l1` 不再维护第二套 parser、IR 或表达式解释器。
- seed 是 parser、verifier、`#eval` 和 IR 对象生命周期的唯一实现者。
- 常量、算术、函数调用、分支、内存操作和 `#eval` 样例可生成并运行 C。
- 生成的编译器可以编译自身源文件。
- 连续两次编译的 C 输出字节一致。
- 错误输入返回诊断并退出，不崩溃、不无限增长内存。

## 6. 明确不做的事

- 不实现 Lain 编译器。
- 不迁移 `src/lainc` 或任何 archive 代码。
- 不实现 Meta AST 操作能力。
- 不实现所有权和 borrow checker。
- 不把 `#eval` 解释器复制到 `compiler.l1`。
- 不通过 C host 写死 Lain 语义。
- 不引入新的 `gen1`、`gen2`、`archive` 等最终产物命名。

## 7. 下一项工作

后续工作只扩展 seed 访问器的边界诊断和更多 LAIN-IR 表达式种类；不回到 Lain
前端或 Meta 路线。

## 8. 实施记录

### 2026-09-02

- seed 增加了 `lainir_module_handle_eval_values`，解析、验证和 `#eval` 结果
  收集可以复用同一个 module handle；`lainir_eval_source_values` 已改用这条路径。
- seed 增加了 `lainir_module_handle_eval_block`，编译器适配层可以在已有
  module handle 上直接执行借用的 `#eval` block，不必重新解析源文件。
- seed 增加了 `lainir_module_handle_destroy`，可在释放后把调用方句柄置空，
  减少悬空句柄继续流入编译流程的机会。
- seed 增加了带入口名称的 module verify API，调用方可以分别验证普通模块和
  要求 `main` 的可执行模块。
- seed 增加诊断的 code、line、column、message 只读访问器和清空函数。
- `compiler.l1` 的正式编译入口会先调用 seed parser/verifier 做输入预校验；
  lexer contract 等故意测试非法文本的入口不走这条预校验。
- 每次改动后已验证 seed 构建和 `lainir-compiler.exe` 自举通过。
- `target/ir_handle_smoke.c` 已直接验证 module handle 的 parse/verify、procedure
  访问器、block/instruction/expression 只读访问、空值/越界返回，以及同一个
  handle 上的标量 `#eval` 结果收集。
- program mode 的 `main` 检查已经改为遍历 seed module/procedure 句柄；生成的
  编译器已通过常量和算术样例，native host 对伪造过程句柄会返回失败值而不会
  直接解引用。
- `compiler.l1` 已加入第一条真正的 seed-backed C emission 路径：简单标量程序
  直接遍历 seed 的 procedure/block/instruction/expression 句柄生成 C。
- 修复 native host 预解析读取源文件时缺少 C 字符串终止符的问题；现在带调用
  和多个 `#eval` 的输入也能通过 seed 预解析、生成 C 并运行。
- host 的 source/eval 入口增加长度上溢检查；非法长度会返回错误，不会进入
  分配或解析路径。
- seed-backed emitter 不再执行额外的递归预扫描；seed verifier 负责结构合法性，
  emitter 单次遍历输出，遇到未支持种类返回失败并清理 artifact。
- 当前短回归已覆盖常量、算术、直接调用和含 `#eval` 的线性过程；四个样例均由
  根目录 `zig-out/bin/lainir-compiler.exe` 生成 C，再编译运行得到预期返回值。
- 短回归已扩展为七个样例，新增嵌套 `#if`、内存读写和结构化 `#loop`；它们均由
  根目录编译器生成 C 并运行得到预期结果。nested block 的 host 句柄查找已改为递归遍历，避免
  分支体被错误当成空块。
- 新增 `ir-instruction-destination` C/L1 访问器，明确区分 store 目标和 if 条件；
  内存样例已实际验证 `#store` 写入、`#load` 读取和生成 C 的 `memcpy` 路径。
- 修正 LEA 句柄访问：`expr_left`/`expr_right` 对 `EXPR_LEA` 分别表示 base/index；
  内存样例确认走 seed-backed emitter。
- procedure、block、instruction、expression 句柄在 L1 中以整数形式借用；host
  在解引用前校验其归属，生命周期由 module handle 管理。
- 当前已完成这次迁移：`compiler.l1` 仅保留 18 个 seed-backed 过程，源码从约
  214 KB 缩减到约 34 KB；`compiler_parts/` 只保留职责命名的五个文件。
- 为减少自举耗时，标识符转义移到 artifact host 的单次调用；整数文字输出使用
  可变局部值，负数和循环计数均已验证。
- 自举实测通过：seed 生成根目录 `zig-out/bin/lainir-compiler.exe`，随后该
  编译器再次编译自身源码并生成相同 C；七个短样例全部生成并运行成功。
- unit `#eval` 已加入 module-mode 检查：生成的 C 使用 `((void)0)`，并通过 Zig
  C 编译器语法检查。
- 短回归脚本现在还检查非法输入：编译器返回非零诊断状态且不会留下输出文件。

## 9. 可执行文件约定

正式编译器使用以下命令行：

```text
lainir-compiler.exe <output.c> <input.l1>
lainir-compiler.exe --module <output.c> <input.l1>
```

不带 `--module` 时要求输入含有合法的 `main() -> #bits<32>`；`--module` 允许
只含库过程的模块。成功退出码为 `0`，输入解析/验证失败为 `2`，seed-backed
emitter 不支持的 IR 形状为 `3`。输出文件只在成功完成 artifact 后保留。
