# Lain Meta 元编程系统

Scheme meta 负责语言改写和静态语义建模。compile-time lain 负责真正有副作用的编译期执行。编译器只负责调度二者并提供最小 substrate。三者必须分开。

## Scheme meta 管什么

Scheme meta 操作语言，不承担构建系统或宿主脚本的职责。

管这些：

- 解析后语法树的规范化
- 顶层声明改写
- 名字解析与绑定重写
- `module` / `signature` / `interface` / `effect` 等对象模型构造
- 静态约束检查
- lowering 到更低层表示

不管这些：文件系统扫描、网络请求、包下载、调外部进程跑 bridge generator，以及任何大型副作用型宿主能力。

## 为什么还在用 Scheme

Scheme 对"改语法、拼 AST、组织变换"的表达力很高，作为 meta 语言是合适的。

两点坚持：

- 标准 R7RS 风格，不发明私有 Lisp
- 保留 `(lain-quote ...)` 桥接，让 meta 库能直接构造语法对象

一个典型的 Scheme meta 过程：

```scheme
(define (derive-clone decl)
  (let* ((name   (struct-name decl))
         (fields (struct-fields decl)))
    (lain-quote
      impl Clone for ,name {
        fn clone(self: & ,name) -> ,name {
          ,name {
            ,@(map (lambda (f)
                     (lain-quote
                       ,(field-name f): self.,(field-name f).clone()))
                   fields)
          }
        }
      })))
```

目的很明确：生成语言结构，不干别的。

## meta 库里的语义对象

`type`、`module`、`effect`、`signature`、`interface` 都是 meta 库对象——由 `std/meta/...` 定义，关系由库代码决定，检查规则由库层实现。编译器不直接暴露"创建一个 module object"的语义 API。

- `module` 是 meta 层命名空间对象
- `signature` 是 module 的接口类型
- `interface` 是运行时动态分发协议
- `effect` 是 effect row 中的语义对象

## compile-time lain

编译期任务需要副作用时，不应继续留在 Scheme 里，转成 compile-time lain。

这类事归它：

- 读取 json / yaml / idl
- 解析 C/C++ 头信息并生成绑定
- 调用外部编译器或代码生成器
- 生成 `.lain` / `.lci` / bridge 文件
- 受控前提下的网络获取

大致长这样：

```lain
let bindings = comptime {
  let spec = fs::read("api.json")?;
  let bridge = cppbind::generate(spec)?;
  emit::source("gen/api.lain", bridge.source);
  emit::interface("gen/api.lci", bridge.interface);
};
```

关键不是 `comptime` 这个词，是语义：跑的是 lain，可以有 effect，effect 必须显式，effect 需要 capability 支持。

## 给 Scheme meta 的 API

收敛为通用底座就够：

- `syntax.*`：构造与访问语法
- `diag.*`：错误、警告、note
- `sym.*`：symbol / gensym
- `artifact.*`：读写接口、记录依赖、声明生成文件
- `ir.*`：构造 lowered IR
- `phase.*`：触发 compile-time phase，传递输入输出

不要走"把语言语义对象直接做成 host API"的路。

## 实现策略

Scheme meta 可以 AOT 或缓存，那只是实现优化，不改语义边界。可接受的方案：标准库 `std/meta/*.scm` 预编译或缓存，项目级 meta 代码解释执行或缓存，compile-time lain 通过独立 phase 运行，capability 由 driver 注入而不是 Scheme VM 自带一套大型宿主标准库。

## 与模块系统的关系

新的 module 设计正适合这套架构：`let M = import("...")` 是 meta 绑定，`module { ... }` 与 `signature { ... }` 是 meta 对象构造，`export { ... }` 形成接口工件，`.lci` 是模块边界 artifact。module 机制不是编译器额外理解的神秘系统，而是 meta 库在 compiler substrate 之上实现出来的语言层结构。

# 1 claude idea

从代码来看，现在的架构是：

```
C main → native_run_pipeline → Scheme (compile-group-to-core) → builder_ffi → L1 IR
```

Scheme 实际上承担了整个前端——词法、解析、类型检查、IR 构建全在里面。C 这边只是个壳。

---

**"阶段 hook"是什么感觉？**

```
C 前端 (lexer + parser → AST)
    ↓
    [hook point] meta pass 1  ← Scheme 介入，可选
    ↓
C AST → L1 lowering
    ↓
    [hook point] meta pass 2  ← Scheme 介入，可选
    ↓
L1 → codegen
```

也就是说，Scheme 是**插件**，不是**主体**。C 才是驱动整个流程的东西，Scheme 只在特定 hook 点被调用来做宏展开、编译期求值、自定义变换等。

---

**现在的问题**

现在的设计反过来了——Scheme 是主体，`compile-group-to-core` 驱动一切，C 的 builder_ffi 反而是 Scheme 的工具库。这导致：

- 调试困难（错误堆栈在 Scheme 里）
- 阶段边界模糊（词法/解析/IR构建混在一起）
- C 这边的 L1 IR 结构和 Scheme 之间靠 FFI 撑着，很脆

---

**要回到"hook"设计，边界应该怎么划**

```
C 负责：lexer → token stream → parser → C AST → L1 lowering
Scheme 负责：宏展开、EVAL 节点求值、meta 变换
```

hook 点就两个：

1. **parse 后、lowering 前**：Scheme 拿到 AST，可以展开宏、插入节点、做编译期计算，返回变换后的 AST
2. **L1 构建后、codegen 前**：Scheme 拿到 L1 IR，可以做 EVAL 节点消除、自定义 pass

这样 `native_run_pipeline` 变成：

```c
int32_t native_run_pipeline(...) {
    Tokens *tok = lain_lex(src, len);
    AST    *ast = lain_parse(tok);
    ast = meta_run_ast_hooks(scheme_ctx, ast);   // hook 1
    L1Module *ir = lain_lower(ast);
    ir  = meta_run_ir_hooks(scheme_ctx, ir);     // hook 2
    // → codegen
}
```

# 2: LAIN-AST

我觉得第一步不是设计 AST 节点，而是设计 AST 的不变量（Invariant）。

因为你已经决定：

```text
struct
effect
function
module
trait
...
```

都不是 Parser 内建。

那么 LAIN-AST 必须满足：

```text
Parser 不理解语言特性
Meta 定义语言特性
```

否则体系会崩掉。

---

我会先定三个原则。

### 原则1：AST 不表达语义

不要出现：

```c
AST_FUNCTION
AST_STRUCT
AST_EFFECT
AST_MODULE
```

因为这些都是 Meta 的概念。

AST 只表达：

```text
名字
字面量
嵌套关系
```

---

### 原则2：AST 保留源代码结构

例如：

```lain
foo(bar, baz)
```

和：

```lain
foo {
    bar
    baz
}
```

应该保留不同结构。

因为 Meta 可能关心这些差异。

---

### 原则3：AST 必须天然适合 Scheme

否则你会一直在做转换。

---

基于这个思路，我觉得 AST 可以压缩到 8 种节点以内。

```c
typedef enum {
    AST_SYMBOL,
    AST_INTEGER,
    AST_FLOAT,
    AST_STRING,

    AST_GROUP,   // (...)
    AST_BLOCK,   // {...}
    AST_LIST,    // [...]

    AST_FORM,
} LainAstKind;
```

---

### Symbol

```lain
foo
```

↓

```scheme
(symbol foo)
```

---

### Integer

```lain
123
```

↓

```scheme
(int 123)
```

---

### String

```lain
"hello"
```

↓

```scheme
(string "hello")
```

---

### Group

表示圆括号。

```lain
(a b c)
```

↓

```scheme
(group
  a
  b
  c)
```

---

### Block

表示花括号。

```lain
{
   foo
   bar
}
```

↓

```scheme
(block
  foo
  bar)
```

---

### List

表示方括号。

```lain
[a b c]
```

↓

```scheme
(list
  a
  b
  c)
```

---

然后最关键：

### Form

```c
struct AstForm {
    AstNode* head;
    AstNode** args;
};
```

表示：

```text
head + children
```

例如：

```lain
foo(bar, baz)
```

↓

```scheme
(form
  foo
  bar
  baz)
```

---

### 为什么需要 Form？

因为否则：

```lain
foo(bar)
```

会变成：

```scheme
(group
  foo
  bar)
```

失去了：

```text
foo 是头部
bar 是参数
```

这个信息。

---

所以我会让：

```lain
foo(bar,baz)
```

Parser 直接生成：

```scheme
(form
  foo
  bar
  baz)
```

而不是：

```scheme
(group
  foo
  bar
  baz)
```

---

同样：

```lain
foo {
    ...
}
```

也可以表示成：

```scheme
(form
  foo
  (block ...))
```

---

于是：

```lain
struct Person {
    age: i32
}
```

变成：

```scheme
(form
  struct
  Person
  (block
      ...))
```

---

而：

```lain
effect IO {
    ...
}
```

变成：

```scheme
(form
  effect
  IO
  (block
      ...))
```

---

注意：

```text
struct
effect
```

都只是 Symbol。

AST 完全不知道它们是什么。

---

我甚至会再进一步。

不要做：

```lain
a + b
```

↓

```scheme
(binary +
   a
   b)
```

---

而是：

```scheme
(form
   +
   a
   b)
```

---

同样：

```lain
a:b
```

↓

```scheme
(form
   :
   a
   b)
```

---

这样：

```text
运算符
函数
关键字
```

全部统一。

---

最终 AST 其实长这样：

```c
typedef enum {
    AST_SYMBOL,
    AST_INTEGER,
    AST_FLOAT,
    AST_STRING,

    AST_GROUP,
    AST_BLOCK,
    AST_LIST,

    AST_FORM,
} LainAstKind;

struct LainAstNode {
    LainAstKind kind;

    SourceSpan span;

    union {
        ...
    };
};
```

然后 Meta 看到的几乎就是：

```scheme
(form
   struct
   Person
   ...)
```

这样的树。

---

这样有一个很大的好处。

以后你可以把：

```lain
struct
effect
module
fn
trait
enum
actor
service
protocol
```

全部删光。

Parser 一行代码都不用改。

因为对于 Parser：

```text
它们从来都只是 Symbol。
```

而不是语言关键字。

# IR API

编译器的中间表示。Scheme 侧通过 `std/meta/ir-api.scm` 构造 IR，C 侧通过 `core.*` FFI 函数处理。

当前实现是 Scheme 薄包装 → C FFI，未来可替换为纯 Scheme 实现。

## 类型

```scheme
(ir.type.bits width)      → 整数 (i8/i16/i32/i64)
(ir.type.addr)            → 地址 (64-bit)
(ir.type.floats width)    → 浮点 (f32/f64) — 预留
(ir.type.simd width lanes) → SIMD — 预留
(ir.type.unit)            → void
(ir.type.never)           → 不可到达

(ir.type.unit? ty)        → #t / #f
(ir.type.size ty)         → 字节数
(ir.type.equal? a b)      → #t / #f
(ir.type.lookup name)     → 按名查找已注册类型
```

## 表达式

表达式是纯值，作为指令的操作数存在。

```scheme
;; 常量与引用
(ir.expr.const block type val)        → 整数常量
(ir.expr.string block str)            → 字符串常量
(ir.expr.var block name type)         → 命名变量引用
(ir.expr.arg block index type)        → 参数引用 (第 index 个参数)

;; 内存
(ir.expr.load block type ptr)         → 从地址加载
(ir.expr.lea block base idx scale offset) → 地址计算

;; 运算
(ir.expr.add block left right)        → 加法
(ir.expr.sub block left right)        → 减法
(ir.expr.primitive block opcode operands result-ty) → 通用运算

;; 调用
(ir.expr.call block fn-name args ret-ty)           → 直接调用
(ir.expr.call-indirect block fn-ptr ret-ty params args) → 间接调用

;; 分配
(ir.expr.alloca block element-ty byte-size) → 栈分配
(ir.expr.field block base struct-ty field-index field-ty) → 字段访问
```

`ir.expr.primitive` 支持的 opcode：`mul sdiv udiv srem urem and or xor shl lshr ashr eq ne slt sle ult ule fadd fsub fmul fdiv`。

## 指令

指令有副作用，组成基本块体。

```scheme
(ir.inst.set block name value)       → 命名变量赋值
(ir.inst.store block dest value)     → 内存存储
(ir.inst.call block expr)            → 调用语句
(ir.inst.return block value)         → 块内返回
(ir.inst.if block cond then-body else-body) → 结构化 if
```

`ir.inst.return` 和 `ir.term.return` 的区别：前者是块体末尾的 INST_RETURN 指令，后者是终止符 TERM_RETURN。通常两者一起用。

没有 LOOP/BREAK 指令。循环在 lowering 阶段展开为 branch 图。

## 终止符

每个基本块必须有一个终止符，决定执行流如何离开当前块。设定终止符后不能再追加指令。

```scheme
(ir.term.return block value)                 → 返回值
(ir.term.return-none block)                  → 返回 void
(ir.term.branch block target)                → 无条件跳转
(ir.term.cond-branch block cond true-b false-b) → 条件跳转
```

## 子例程与块

```scheme
(ir.sub.define name params ret)      → 创建子例程 (空块列表)
(ir.sub.extern name params ret linkage) → 外部声明 (linkage: c|lain)
(ir.sub.set-link-name! sub name)     → 设置链接名 (C 符号重整)

(ir.sub.name sub)       → 名字
(ir.sub.link-name sub)  → 链接名
(ir.sub.params sub)     → 参数类型列表
(ir.sub.ret-type sub)   → 返回类型

(ir.sub.block sub)      → 追加空基本块
(ir.block.parent block) → 块所属子例程
```

## 类型安全谓词

全部是桩，当前返回 `#t`。运行时替换后带真实类型标签。

```scheme
(ir.type? obj)   (ir.expr? obj)   (ir.inst? obj)
(ir.term? obj)   (ir.sub? obj)    (ir.block? obj)
```

## 命名约定

- 纯函数不加 `!`，副作用函数加 `!`（如 `ir.sub.set-link-name!`）
- 谓词加 `?`（如 `ir.type.unit?`）
- 前缀对应 L1 IR 层：`ir.type.*` `ir.expr.*` `ir.inst.*` `ir.term.*` `ir.sub.*` `ir.block.*`

## lowering 示例

### 整数加法

```scheme
(let* ((i32   (ir.type.bits 32))
       (sub   (ir.sub.define 'add (list i32 i32) i32))
       (block (ir.sub.block sub))
       (a     (ir.expr.arg block 0 i32))
       (b     (ir.expr.arg block 1 i32))
       (sum   (ir.expr.primitive block "add" (list a b) i32)))
  (ir.inst.return block sum)
  (ir.term.return block sum))
```

### while 循环展开

```scheme
;; (while (< i 10) (set! i (+ i 1))) 展开为:
(let* ((header-b (ir.sub.block sub))
       (body-b   (ir.sub.block sub))
       (exit-b   (ir.sub.block sub)))
  ;; header: 比较 → 条件跳转
  (let ((cond (ir.expr.primitive header-b "slt"
                 (list (ir.expr.var header-b 'i i32)
                       (ir.expr.const header-b i32 10))
                 (ir.type.bits 1))))
    (ir.term.cond-branch header-b cond body-b exit-b))
  ;; body: 递增 → 跳回 header
  (let ((inc (ir.expr.add body-b
               (ir.expr.var body-b 'i i32)
               (ir.expr.const body-b i32 1))))
    (ir.inst.set body-b 'i inc)
    (ir.term.branch body-b header-b))
  ;; exit: 继续...
  )
```

## 函数清单

`ir-api.scm` 当前 41 个函数：

| 分类     | 数量 | 函数                                                                                                                                                                                                              |
| -------- | ---: | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 类型     |    6 | `ir.type.bits` `ir.type.addr` `ir.type.floats` `ir.type.simd` `ir.type.unit` `ir.type.never`                                                                                                                      |
| 类型查询 |    4 | `ir.type.unit?` `ir.type.size` `ir.type.equal?` `ir.type.lookup`                                                                                                                                                  |
| 表达式   |   13 | `ir.expr.const` `ir.expr.string` `ir.expr.var` `ir.expr.arg` `ir.expr.load` `ir.expr.lea` `ir.expr.add` `ir.expr.sub` `ir.expr.primitive` `ir.expr.call` `ir.expr.call-indirect` `ir.expr.alloca` `ir.expr.field` |
| 指令     |    4 | `ir.inst.set` `ir.inst.store` `ir.inst.if` `ir.inst.call`                                                                                                                                                         |
| 终止符   |    4 | `ir.term.return` `ir.term.return-none` `ir.term.branch` `ir.term.cond-branch`                                                                                                                                     |
| 子例程   |    7 | `ir.sub.define` `ir.sub.set-link-name!` `ir.sub.extern` `ir.sub.name` `ir.sub.link-name` `ir.sub.params` `ir.sub.ret-type`                                                                                        |
| 块       |    1 | `ir.sub.block`                                                                                                                                                                                                    |
| 安全     |    6 | `ir.type?` `ir.expr?` `ir.inst?` `ir.term?` `ir.sub?` `ir.block?`                                                                                                                                                 |

# Meta System Specification

本文档定义 Lain 的 Meta 系统。

本文档定义 Meta 层如何消费 `AstTree`、生成 `Middle AST`、执行宏展开、完成语义检查，并最终 lower 到 `LAIN-IR`。

## 1. Position in Pipeline

Meta 层位于 `AstTree` 之后，`LAIN-IR` 之前。

```text
Source Text
  -> RawAst
  -> AstTree
  -> Middle AST
  -> Typed / Elaborated Middle AST
  -> LAIN-IR
```

其中：

```text
RawAst
  C parser 产出的无语义拓扑树。

AstTree
  RawAst 的 Scheme / Meta 层值表示，仍然无语义。

Middle AST
  Meta domain parser 产出的语言语义树。

Typed / Elaborated Middle AST
  完成名称解析、类型检查、effect 检查、comptime 特化和 layout 后的语义树。

LAIN-IR
  物理执行层中间表示。
```

Meta 层负责：

```text
AstTree -> Middle AST -> Typed / Elaborated Middle AST -> LAIN-IR
```

Parser 不理解 Lain 语言语义。

Backend 不重新理解 Lain 语言语义。

语言语义归 Meta 层所有。

## 2. Core Principle

Meta 系统的核心原则是：

```text
Syntax topology is not language semantics.
```

也就是说：

```text
AstTree 只描述源码 token 如何组合。
Meta 层才决定这些组合是什么意思。
```

例如：

```lain
foo(x)
```

在 AstTree 中只是：

```text
Postfix(Atom("foo"), Group(paren, ...))
```

它可能被解释为：

```text
value call
type application
effect application
macro invocation
attribute argument
DSL-specific form
```

具体含义由 domain parser 和上下文决定。

因此，Meta 层不能假定某种拓扑形状天然等于某种语言语义。

## 3. Responsibilities

Meta 层可以做：

```text
domain parsing
macro expansion
attribute expansion
name resolution
type checking
effect checking
generic / comptime specialization
layout calculation
closure conversion
control form lowering
ABI lowering
lowering to LAIN-IR
diagnostic generation
```

Meta 层不应该做：

```text
直接修改 C parser 的 RawAst 内存
绕过 SyntaxContext 构造非卫生 AST
把 domain-specific parser 放入 AstTree helper
让 LAIN-IR 重新承载高层语义
直接执行不受约束的宿主 I/O
```

Meta 层是语言定义的主要位置。

高层 Lain 的这些概念都属于 Meta：

```text
fn
let
struct
module
import
effect
interface
impl
comptime
macro
attribute
type
generic
```

它们不是 Parser 内建概念，也不是 LAIN-IR 内建概念。

## 4. Host Boundary

Meta 层运行在宿主编译器提供的环境中。

初期实现中，Meta 层可以使用 Scheme。

后续自举阶段，Meta 层可以逐步迁移到 Lain 自身。

当前自举实现已经包含第一个由 Lain 所有的端到端切片：

```text
target source
  -> RawAst opaque handles
  -> syntax.lain (zero-copy Middle-AST views)
  -> elaborator.lain (scope and i32 semantics)
  -> lower.lain (recursive lowering)
  -> compiler.lain (driver)
  -> packages/lain/compiler/l1_text_builder.lain
  -> canonical structured LAIN-IR
  -> host verifier / interpreter
```

M2 切片支持多个具名 `i32` 过程、`i32` 参数、顺序局部 `let`、参数与局部
作用域、规范十进制字面量、递归 `+`、直接函数调用、`==` 和最终位置的结构化
`if/else`。模块函数表负责重复声明、callee 和 arity 检查。目标程序的这些
语义不经过 Scheme domain lowerer；Scheme 只承担编译并启动这段 Lain Meta
实现的 stage-0 职责。

当前作用域环境使用函数体 syntax interval `[body-start, current-form)` 表示。
查找名字时，Lain Meta 遍历此前的 local forms。这使源码顺序、重复绑定和
先声明后使用的规则保持显式，并避免把作用域策略下放给 C host。

当前 Middle AST 采用 zero-copy semantic view：仍以 opaque RawAst node id 作为
存储身份，但只通过 `syntax` 暴露的 `expr-tag`、`form-tag` 和 program view
访问。`elaborator` 与 `lower` 不直接调用 C AST capability。这样阶段
边界已经存在，同时不要求 C host 为高层语义节点提供分配器。

M2 使用明确的 `i32` / `bool` 类型对象和单次 parse 的 CompileResult 查询协议，
可取得成功状态、诊断、procedure count、procedure lookup 和完整 L1 unit 文本。
它仍不是完整 Meta 系统：尚未覆盖用户定义类型、泛型、effect、完整控制流和
跨源模块摘要。后续 domain 必须沿相同所有权边界扩展，而不是在 C parser 或
LAIN-IR 中加入这些高层语义。

### M3 reusable compiler artifact

M3 不再为每次目标编译递归 source-link 整个 Lain Meta 实现。Stage-0 按依赖
顺序分别生成 interface 与 L1，再把实际 procedure 合并、去除已由 artifact
提供的 extern，并保留真正的 host extern，形成可重复装载的：

```text
build/core-self-hosting/meta_compiler.l1
```

运行 compiler artifact 与运行普通生成程序具有不同 capability policy：

```text
generated target L1
  capability-free by default

M3 compiler artifact
  binds an #extern only when it is also in the host's fixed compiler allowlist
```

M3 的固定 allowlist 仅包含 RawAst reader（`ast.parse!`、node topology/text
queries）以及纯字符串/整数文本 helpers。文件、进程、环境、网络和普通生成目标
执行能力不在名单中；artifact 声明未授权 extern 会在执行前得到 diagnostic
3006。构建脚本同时断言最终 artifact 的 extern 集恰好等于该名单，防止依赖静默
漂移。当前 RawAst bridge 是 single-active-session：下一次 `ast.parse!` 替换上一
次 arena，最后一个 session 随 VM 生命周期释放。

artifact 的首个真实输入是 `packages/lain/compiler/types.lain`，随后覆盖
返回 `addr` 和字符串诊断的 `diagnostics.lain`。这要求 Lain Meta 支持带
`@export` 的 canonical callable binding、显式 `return`、语句式 `if`、
`addr`/字符串类型，以及带参数调用。

### M4 Meta-owned frontend core

M4 把 zero-copy syntax view 之上的顶层解释正式放入
`packages/lain/compiler/surface_forms.lain`。它区分 attribute、import、procedure
declaration 和 procedure definition；callable table 同时包含声明和定义，而
lowering 只生成被选择的定义。

局部作用域不再用单一的 `[body-start, current-form)` 平面区间近似。Lain Meta
从函数 block 根开始查找目标 form，进入嵌套 `if` 时继承此前支配该分支的绑定，
同时拒绝分支之后才出现的声明。由此支持 parent lexical scope、branch-local
shadowing 和顺序可见性。

M4 artifact 验收把以下源文件合成一个语义闭包：

```text
syntax + surface + types + modules
  + elaborator + l1_text_builder + lower
  + diagnostics + compile_result + compiler_frontend
```

完整编译由 Lain-owned elaborator 验证整个闭包。增量入口
`compiler_frontend_compile_named` 只验证所选 procedure 的函数体及其引用的 callable
签名，随后生成该定义并把其余 callable 写成 extern contract；测试执行
自编译生成的 `compiler_frontend_schema_version() -> 4`。这是增量自举接口，不是长期
module linker。

CompileResult 查询协议包含成功状态、diagnostic code/message、失败 procedure、
procedure summary 和 L1 unit。失败编译不会生成部分 L1。M4/M5 使用的整单元文本
lowering 现在只保留为 bootstrap 参考实现。

### M5 Unified bindings and Meta modules

M5 将顶层声明收束为同一个 Meta binding：

```lain
let NAME: EXPECTED_SHAPE = INITIALIZER;
```

`std::func`、`std::module` 与 `import("path")` 是构造 MetaValue 的普通 Meta
constructor，不是 RawAst 节点种类。`surface_forms.lain` 提取 binding name、expected shape、
initializer 和可选 body；`modules.lain` 在这些 view 上形成 zero-copy
ModuleSummary，并维护 exports 与 dependency graph；`workspace.lain` 统一完成
结构校验、跨模块 elaboration、诊断与 linking。

验收 workspace 中，`app` 通过
`let math = import("math")` 获得 ModuleRef，`math.add(40, 2)` 经过 export
检查后 lower 为 `#call math__add(...)`。Module/import/export 在 L1 中全部消失，
最终只有 `math__add`、`app__main` 等物理 procedure。自编译 schema 同步升级为
`compiler_frontend_schema_version() -> 5`。

### M6 Structured L1Unit

M6 将默认 Meta artifact 的 lowering 路径从：

```text
Lain Meta -> L1 text -> C text parser -> physical L1 nodes
```

替换为：

```text
Lain Meta -> opaque physical builder capabilities -> L1Unit handle
```

`l1_unit_builder.lain` 定义 Lain 侧的结构化 builder API，
`lower.lain` 负责名称、类型、调用、局部变量、控制流与 procedure 顺序。
C host 只分配和保存 `L1Subroutine`、`L1Instruction`、`L1Expr` 等物理节点，并提供
verify、execute、debug-text 与 destroy；它不解释 `let`、`fn`、`module` 或 source
alias。

成功编译返回非零 opaque unit handle。任一验证或构造步骤失败时，Meta 销毁正在
构造的 unit 并返回 0，因此调用者永远拿不到部分 L1Unit。文本 emitter 只用于
观察已经构造完成的 unit，不再是编译通道。

默认 artifact 不再链接 `l1_text_builder.lain`、`lower.lain` 或
`core.string-append-linear!`。M6 验收覆盖普通编译、named self-compile、跨模块
`math.add(40, 2)` linking、失败原子性、debug text，以及直接执行
`app__main == 42`。schema 同步升级为 6。

### M7 Lain-owned L1 Interpreter

M7 在 structured L1Unit 上增加只读的物理查询 ABI。C 可以回答 procedure、
instruction、expression 的 kind、child、name、argument 等结构问题，并提供 opaque
frame/result 存储；C 不决定表达式如何求值。

执行语义位于 `packages/lain/compiler/l1_interpreter.lain`：

```text
procedure lookup -> argument frame -> instruction walk -> expression eval
                 -> call/if/return -> structured result
```

当前自举子集包括 i32 常量、argument、local variable、add、eq、ne、direct call、
let、structured if 和 return。找不到 procedure、arity 不符、未知节点及 extern call
均产生稳定的 interpreter status。尤其是 extern 不会自动映射到 host capability。

M7 使用同一个跨模块 unit 验收：C reference interpreter 与 Lain interpreter 都执行
`app__main == 42`。C reference 仍用于差分测试，但不是新 interpreter 的语义实现。

### M8 Structured Comptime Evaluation

M8 不创建第二套 evaluator。Meta 先把待求值程序 lower 为 temporary structured
L1Unit，再调用 M7 的 Lain interpreter：

```text
source -> RawAst -> Lain Meta -> temporary L1Unit
                               -> Lain interpreter -> status/value
                               -> continued structured lowering
```

`compiler_frontend_comptime`/`compiler_frontend_comptime_main` 返回 opaque result，其中 status 0
表示成功；source elaboration 失败保留原 diagnostic code，物理 lowering 失败返回
8002，extern capability call 返回 7002。temporary unit 在求值后销毁。

`compiler_frontend_comptime_materialize_main` 将求出的 i32 重新写入一个新的 structured
L1Unit，证明结果回到了 Meta/lowering 流程，而不是停在测试宿主。验收程序以递归
`sum_range(1, 11, 0)` 计算 55，运行两次结果相同；C runtime reference、Lain
comptime 与 materialized unit 三条路径均得到 55。schema 同步升级为 8。

当前 M8 是编译器 Meta API/执行协议；把 `comptime` surface form 接入完整用户语法
仍属于后续 surface elaboration 工作。其执行语义已经固定为 LAIN-IR interpreter，
不属于 Scheme Meta evaluator。

### M9 Lain-owned Compiler State

M9 把编译过程的状态与结果从宿主约定迁入 Lain：

```text
M9SyntaxRef(node, start, end)
M9CompilerState(syntax, phase, module_count, optional_unit, diagnostics)
  -> M9CompileResult(outcome_tag, syntax, module_count,
                     optional_unit, diagnostics)
```

`compiler_frontend_compile_structured_root` 是真实内部入口。验证失败时，它把稳定 diagnostic
code/message/span 写入 Lain-owned `M9DiagnosticBag`；lowering 成功时才写入 opaque
L1Unit。`m9_compiler_state_finish` 强制执行失败原子性：只要存在诊断或 unit 缺失，
结果就是 failure 且不暴露 unit。`compiler_frontend_compile -> i32` 仅是旧 CLI/FFI 的兼容投影，
不再定义内部错误模型。

当前 bootstrap 没有自有 growable allocator，因此 DiagnosticBag 使用拥有四个 inline
slot 的有界 slice，避免返回借用 callee stack 的链表。结果 outcome 使用 tagged-struct
字段；完整 enum/match 仍由独立 bootstrap-core 测试保护，在 match-as-value lowering
完备后可以替换物理表示，而不改变 CompileResult policy。

结构体按地址通过当前 L1 ABI。interpreter 的 aggregate arena 因而具有整次 run 的
生命周期，`#field[offset](base):type` 在文本 round-trip 中保留字段物理类型，64 位
addr store 必须写入完整 pointer width。M9 验收执行 begin/with-unit/finish/result 四个
探针，并覆盖成功结果、失败诊断、source extent、无 partial L1Unit，以及包含
`compiler_state.lain` 的增量自编译闭包。schema 升级为 9。

无论宿主语言是什么，Meta 层都不直接操作 C 内存指针。

Meta 通过 Host API 操作 AST、IR、diagnostic 和编译期执行环境。

```text
Meta code
  -> Host API
  -> RawAst arena / IR builder / evaluator / diagnostics
```

Host API 必须保持：

```text
opaque
safe
minimal
hygienic
diagnostic-aware
```

## 5. RawAst and AstTree Access

Meta 层通过 opaque handle 查询 RawAst。

典型 Host API：

```scheme
;; node kind
(lain_ast_kind ctx node-id)

;; atom
(lain_ast_atom_text ctx node-id)

;; group
(lain_ast_group_delim ctx node-id)
(lain_ast_group_children ctx node-id)

;; prefix
(lain_ast_prefix_op ctx node-id)
(lain_ast_prefix_operand ctx node-id)

;; postfix
(lain_ast_postfix_operand ctx node-id)
(lain_ast_postfix_op ctx node-id)

;; infix
(lain_ast_infix_op ctx node-id)
(lain_ast_infix_left ctx node-id)
(lain_ast_infix_right ctx node-id)

;; juxt
(lain_ast_juxt_left ctx node-id)
(lain_ast_juxt_right ctx node-id)

;; sep
(lain_ast_sep_text ctx node-id)

;; metadata
(lain_ast_span ctx node-id)
(lain_ast_syntax_context ctx node-id)
```

`RawAst -> AstTree` 的转换可以由 `canonicalize.scm` 完成。

`AstTree` 是 Scheme 值表示，便于 pattern matching 和 domain parser 操作。

推荐表示：

```scheme
(atom text span ctx)
(group delimiter children span ctx)
(prefix op operand span ctx)
(postfix operand op span ctx)
(infix op left right span ctx)
(juxt left right span ctx)
(sep text span ctx)
```

早期实现可以省略 `span` 和 `ctx`，但 API 和数据结构设计必须保留位置。

## 6. AstTree Helper Boundary

AstTree helper 只能提供通用树操作。

允许的 helper：

```text
tree.kind
tree.span
tree.syntax-context
tree.children
tree.atom-text
tree.group-delim
tree.prefix-op
tree.postfix-op
tree.infix-op
tree.left
tree.right
tree.flatten-juxt
tree.split-by-sep
tree.strip-paren
tree.expect-atom
tree.match-infix
tree.match-prefix
tree.match-postfix-group
```

禁止放入 AstTree helper 的函数：

```text
tree-parse-type
tree-parse-block
tree-parse-params
tree-parse-effect-set
tree-lower-expr
tree-parse-fn
tree-parse-struct
tree-parse-call
tree-parse-module
```

这些函数依赖 Lain 语言语义，必须放入对应 domain parser。

AstTree 层只能知道：

```text
这是 atom。
这是 group。
这是 infix。
这是 postfix。
这是 sep。
```

它不能知道：

```text
这是函数。
这是类型。
这是调用。
这是字段。
这是 block。
这是 effect set。
```

## 7. Domain Parsers

Meta 层通过 domain parser 把 AstTree 转换为 Middle AST。

推荐划分：

```text
fn/parse.scm          AstTree -> middle.fn / middle.foreign-fn
struct/parse.scm      AstTree -> middle.struct
types/parse.scm       AstTree -> type.*
expr/parse.scm        AstTree -> expr.*
control/parse.scm     AstTree -> control.*
effects/form.scm      AstTree -> effect.*
interface/parse.scm   AstTree -> interface.*
module/parse.scm      AstTree -> module.*
import/parse.scm      AstTree -> import.*
attrs/parse.scm       AstTree -> attr.*
pattern/parse.scm     AstTree -> pattern.*
macro/parse.scm       AstTree -> macro.*
```

Domain parser 可以共享通用 helper，但不能把自己的语义规则塞回 AstTree 层。

例如：

```text
types/parse.scm 可以解释 Vec(i32) 是 type application。
expr/parse.scm 可以解释 Vec(i32) 是 value call。
effects/form.scm 可以解释 Throws(i32) 是 effect application。
```

但 `tree.scm` 不应该提供 `tree-parse-call` 或 `tree-parse-type-app`。

## 8. Middle AST

Middle AST 是 Meta domain parser 产出的语义树。

它已经脱离纯拓扑层。

Middle AST 可以包含：

```text
middle.fn
middle.struct
middle.module
middle.import
middle.interface
middle.impl

expr.literal
expr.ref
expr.call
expr.field
expr.block
expr.if
expr.match
expr.loop
expr.return

type.ref
type.app
type.fn
type.ptr
type.array
type.comptime

effect.ref
effect.app
effect.set

pattern.binding
pattern.tuple
pattern.struct
pattern.literal
```

Middle AST 仍然可以包含未解析的语义信息，例如：

```text
unresolved name
unresolved type reference
unresolved overload
unexpanded macro
unresolved trait / interface constraint
unchecked effect annotation
```

这些信息必须在 elaboration 阶段消解，不能进入 LAIN-IR。

## 9. Elaboration

Elaboration 是从普通 Middle AST 到 Typed / Elaborated Middle AST 的过程。

```text
Middle AST -> Typed / Elaborated Middle AST
```

它负责：

```text
name resolution
module resolution
import resolution
attribute expansion
macro expansion
type inference
type checking
effect checking
overload resolution
interface / trait resolution
comptime evaluation
generic specialization
layout calculation
ABI decision
closure conversion
control-flow desugaring
```

Elaboration 之后，程序中不应该继续存在：

```text
unresolved identifier
unresolved type name
unresolved overload set
unexpanded macro
unchecked generic
unchecked effect
source-only attribute
```

但它仍然可以保留高层语义信息，直到 lowering 阶段。

例如：

```text
typed expression
resolved symbol
resolved type
resolved function
resolved effect
layout metadata
ABI metadata
```

## 10. Core AST Naming

当前阶段不强制引入独立的 `Core AST`。

推荐使用：

```text
Typed / Elaborated Middle AST
```

如果后续引入 `Core AST`，必须严格定义为：

```text
Core AST = 已完成名称解析、类型检查、effect 检查、comptime 特化和必要 desugaring 的语义 AST。
```

也就是说：

```text
Middle AST -> Core AST -> LAIN-IR
```

其中：

```text
Middle AST
  已解析的语言语义。

Core AST
  已解析、已检查、已特化的语言语义。

LAIN-IR
  物理执行层。
```

Core AST 仍然可以包含语义结构。

LAIN-IR 不能包含高层语义结构。

因此：

```text
Core AST != LAIN-IR
```

## 11. Function Constructor

高层 callable 由 Meta 层的 `std::func` 构造器产生，并通过统一的
`let NAME [: EXPECTED] = INITIALIZER` 语法绑定。

Parser 不知道 `std::func` 会构造函数。

LAIN-IR 也不知道 source-level callable。

源码：

```lain
let add = std::func(x: i32, y: i32) -> i32 {
    x + y
}
```

AstTree 只表达：

```text
Atom("add")
Atom("=")
Atom("std")
Atom("::")
Atom("func")
Group(paren, ...)
Infix("->", ...)
Group(brace, ...)
```

绑定解析器先取得 initializer，`std::func` 构造器再把它解释为：

```text
middle.fn
  name: add
  params: ...
  return: ...
  body: ...
```

后续经过 elaboration 后，一个 `middle.fn` 可能 lower 为：

```text
一个 #proc
多个 specialized #proc
一个 closure object + invoke #proc
一个 wrapper / trampoline
一个 comptime-only function
一个 inline 后不存在的代码片段
```

因此：

```text
std::func callable != #proc
```

## 12. Struct Constructor

高层结构类型由 Meta 层的 `std::struct` 构造器产生，并绑定到名称。

源码：

```lain
let Pair: type = std::struct {
    a: i32,
    b: i32,
}
```

AstTree 不知道这是结构体。

统一绑定解析器与 `std::struct` 构造器将其解释为：

```text
middle.struct
  name: Pair
  fields:
    a: type.ref(i32)
    b: type.ref(i32)
```

layout pass 之后得到：

```text
size
align
field offset
field ABI
```

最终 lower 到 LAIN-IR 时，不应该产生高层 `#struct`。

字段访问应该 lower 成：

```text
#addr + #offset + #load / #store
```

例如：

```lain-ir
#set %b_ptr = #offset %pair_addr 4
#set %b = #load #bits<32> %b_ptr none
```

## 13. Type System in Meta

高层类型系统属于 Meta / Middle AST 层。

LAIN-IR 只保留物理类型：

```text
#bits<N>
#float<N>
#vec<N, T>
#addr
#unit
#never
```

高层类型可以包括：

```text
i32
u32
usize
bool
Ptr(T)
Array(T, N)
Slice(T)
Fn(...)
Struct type
Enum type
Interface type
Comptime type
Effectful function type
```

这些类型必须在 elaboration / lowering 阶段映射到物理表示。

例如：

```text
i32      -> #bits<32>
bool     -> #bits<1>
Ptr(T)   -> #addr
Struct   -> layout metadata + address/offset operations
```

类型表达式由 `types/parse.scm` 负责解析。

类型检查由 elaboration pass 负责。

## 14. Generic and Comptime Model

Lain 的泛型是 comptime value parameter 的一种用法。

泛型不属于 Parser。

泛型不应该要求 Parser 理解 `<...>`。

推荐形式：

```lain
let identity = std::func(comptime T: type, x: T) -> T {
    x
}

let y = identity(i32, 10);
```

类型是一等 comptime value。

因此，下面这些形式在 AstTree 层可以共享同一种拓扑：

```lain
Vec(i32)
Result(i32, Error)
Throws(i32)
foo(x)
```

它们都是：

```text
Postfix(Name, Group(paren, ...))
```

不同 domain parser 根据上下文解释：

```text
types/parse.scm       Vec(i32)       -> type.app
effects/form.scm      Throws(i32)    -> effect.app
expr/parse.scm        foo(x)         -> expr.call
```

`<...>` 泛型形式不推荐作为核心语法：

```lain
Vec<i32>
Result<i32, Error>
foo::<i32>(x)
```

因为它会强迫 Parser 区分比较表达式和类型参数，从而污染 LAIN-AST 边界。

## 15. Effect System

Effect 是 Meta 层语义。

Parser 不知道 effect。

LAIN-IR 不直接保留高层 effect set。

例如：

```lain
let read = std::func() -> String ! {IO, Throws(Error)} {
    ...
}
```

AstTree 只描述 `!`、brace group、name、postfix group 等拓扑。

`effects/form.scm` 负责解析：

```text
effect.set
  IO
  Throws(Error)
```

Elaboration 负责：

```text
effect name resolution
effect checking
effect polymorphism
effect lowering decision
```

Lowering 之后，effect 可能变成：

```text
普通调用约束
错误返回值
continuation passing
handler table
runtime token
state machine
trap / unwind path
```

LAIN-IR 只承载 lower 后的物理控制流和调用。

## 16. Attribute System

Attribute 是 Meta 层 form。

例如：

```lain
@foreign(link_name = "puts")
let puts = std::func(s: CStr) -> i32;
```

AstTree 只描述：

```text
Prefix("@", Group(bracket, ...))
```

`attrs/parse.scm` 负责把它解释为 attribute。

Attribute 可以作用于：

```text
module
import
fn
struct
field
param
type
expr
block
```

具体允许位置由对应 domain parser 决定。

Attribute expansion 可以：

```text
修改 Middle AST metadata
生成新的 Middle AST
触发 macro expansion
影响 ABI lowering
影响 linkage
影响 diagnostics
```

Attribute 不应该让 Parser 识别语义。

## 17. Macro System

Macro 是 Meta 层的语义变换机制。

Macro 可以分为：

```text
Ast macro
  AstTree -> AstTree

Middle macro
  Middle AST -> Middle AST

Attribute macro
  Attribute + target -> transformed target
```

推荐边界：

```text
Ast macro
  适合处理语法糖和 surface-level rewriting。

Middle macro
  适合处理已经有明确语义的 form。

Lowering pass
  负责从 semantic form 生成 LAIN-IR。
```

Macro 不应该直接生成未验证的 LAIN-IR，除非它本身就是明确的 IR-level macro，并经过 verifier。

默认规则：

```text
宏生成 AstTree 或 Middle AST。
LAIN-IR 由 lowering pass 生成。
```

## 18. Hygiene

所有 AST 节点都携带 `SyntaxContext`。

源码节点由 Parser 赋予初始 syntax context。

宏展开产生的新节点必须获得新的 hygiene context。

Meta 层构造 AST 时不能直接拼接裸 identifier。

应该通过 Host API 创建带上下文的 identifier：

```scheme
(lain_ast_make_ident ctx "x" syntax-context)
(lain_ast_fresh_ident ctx "tmp" parent-context)
```

Hygiene 的目标是避免：

```text
宏内部临时变量污染用户作用域。
用户变量意外捕获宏生成的 identifier。
宏生成的引用绑定到错误定义。
```

示例：

```lain
macro twice(expr) {
    #{
        {
            let tmp = ,expr;
            tmp + tmp
        }
    }
}
```

这里的 `tmp` 必须是 fresh identifier，而不是普通用户可捕获的 `tmp`。

## 19. Quasiquote

`#{ ... }` 是 Meta 层构造 AstTree 的语法糖。

它内部书写 Lain surface syntax。

但它的结果仍然是 AstTree，不是 Middle AST，也不是 LAIN-IR。

例如：

```scheme
(define (make-assert cond-node msg-node)
  #{
    if (!,(cond-node)) {
      panic(,(msg-node));
    }
  })
```

`#{ ... }` 的结果只表示拓扑。

之后仍然需要 domain parser 把它解释为：

```text
control.if
expr.call
expr.block
```

### 19.1 Escape

Quasiquote 中使用 `,(expr)` 逃逸到 Meta 层。

```scheme
#{
  println(,(message-node));
}
```

`expr` 的结果必须是可插入的 AST value。

### 19.2 Fresh Identifier

Quasiquote 应支持 fresh identifier 插入。

示例形式：

```scheme
(let ((tmp (fresh-ident ctx "tmp")))
  #{
    {
      let ,tmp = compute();
      ,tmp
    }
  })
```

具体语法可以后续确定，但语义必须支持 hygiene。

### 19.3 No Direct Semantics

下面的 quasiquote：

```scheme
#{
  Vec(i32)
}
```

不会直接生成 `type.app`。

它只生成：

```text
Postfix(Atom("Vec"), Group(paren, ...))
```

在类型上下文中，`types/parse.scm` 可以把它解释为 type application。

在表达式上下文中，`expr/parse.scm` 可以把它解释为 value call。

## 20. Name Resolution

Name resolution 属于 elaboration 阶段。

Parser 不解析名字。

AstTree 不绑定名字。

Middle AST 初期可以保留 unresolved name：

```text
expr.ref("x")
type.ref("Vec")
effect.ref("IO")
```

Name resolution 将其转换为 resolved symbol：

```text
expr.ref(symbol-id)
type.ref(type-id)
effect.ref(effect-id)
```

Name resolution 必须考虑：

```text
module scope
import
local binding
parameter binding
type namespace
value namespace
effect namespace
macro namespace
hygiene context
```

是否采用多 namespace 或统一 namespace 是语言设计问题，但必须在 Meta 层解决，不能下放给 Parser 或 LAIN-IR。

## 21. Type Checking

Type checking 属于 elaboration 阶段。

它输入 Middle AST，输出 typed Middle AST。

示例：

```text
expr.binary("+", expr.ref(x), expr.ref(y))
```

经过 type checking 后变成：

```text
typed.expr.binary
  op: integer.add
  type: i32
  lhs: typed.expr.ref(x: i32)
  rhs: typed.expr.ref(y: i32)
```

Lowering 到 LAIN-IR 时，才生成：

```lain-ir
#set %z = #add #bits<32> %x %y
```

因此：

```text
+ 的重载选择属于 type checking。
#add 是 lower 后的物理整数加法。
```

## 22. Layout Calculation

Layout calculation 属于 elaboration / lowering 边界。

它负责确定：

```text
size
align
field offset
enum tag representation
calling convention representation
capture layout
closure layout
```

例如：

```lain
let Pair: type = std::struct {
    a: i32,
    b: i32,
}
```

layout pass 产生：

```text
Pair.size = 8
Pair.align = 4
Pair.a.offset = 0
Pair.b.offset = 4
```

后续 field access lower 成：

```lain-ir
#set %b_ptr = #offset %pair_addr 4
#set %b = #load #bits<32> %b_ptr none
```

LAIN-IR 不应该直接知道 `Pair.b`。

## 23. Lowering to LAIN-IR

Lowering 负责把 Typed / Elaborated Middle AST 转换为 LAIN-IR。

```text
Typed / Elaborated Middle AST -> LAIN-IR
```

Lowering 必须消除：

```text
std::func callable binding
source struct
source module
source effect
generic
interface
method syntax
field syntax
pattern matching
high-level control syntax
overload
```

Lowering 输出：

```text
#proc
#extern_proc
#global
#bits
#float
#addr
#alloca
#offset
#load
#store
#add
#call
#block
#loop
#if
#ret
```

Lowering 不应该重新做语法解析。

Lowering 的输入应已经完成语义解析和检查。

## 24. Compile-Time Execution Boundary

Meta 本身不应该直接执行任意宿主 I/O。

需要编译期副作用时，必须 lower 成受约束的 LAIN-IR，并交给 Host evaluator 执行。

```text
Middle AST
  -> LAIN-IR comptime proc
  -> Host evaluator
  -> comptime value / generated AstTree
```

编译期执行可以返回：

```text
primitive value
string
bytes
type value
AstTree value
diagnostic
```

编译期副作用必须受 effect system 或 host capability 限制。

例如，读取文件生成 binding：

```lain
let generate_bindings = std::func() -> AstTree ! {HostRead, HostWrite} {
    ...
}
```

其 callable phase 由 Meta 语义决定；当它被判定为 comptime callable
时，应先被 Meta 检查，再 lower 成 comptime LAIN-IR，由 Host evaluator
执行。这里不规定尚未冻结的 phase 表层语法。

Meta 不能直接绕过 effect system 调用宿主文件 API。

## 25. Host Capabilities

Host capability 用于限制编译期副作用。

可能的 capability：

```text
ReadFile
WriteFile
ReadEnv
RunCommand
Network
PackageFetch
EmitDiagnostic
GenerateAst
```

默认 Meta expansion 不拥有危险 capability。

需要副作用的 comptime code 必须显式声明，并由编译器策略允许。

例如：

```text
普通 macro expansion
  不允许文件 I/O。

build script / package fetch
  可以在明确 capability 下访问文件系统或网络。

diagnostic emission
  可以通过受控 API 产生 warning / error。
```

Capability 是 Host boundary 的一部分，不是 Parser 语义，也不是 LAIN-IR 高层语义。

## 26. Diagnostics

Meta 层负责产生高质量 diagnostics。

所有 AstTree、Middle AST 和 Typed Middle AST 节点都应该保留 source span。

Diagnostic 至少包含：

```text
severity
message
primary span
secondary spans
notes
help
origin
```

示例：

```text
error: expected type expression
  --> main.lain:3:12
   |
 3 | let x: = 1;
   |        ^ expected type here
```

Meta diagnostic 应尽量基于 domain parser 语境，而不是只输出通用 parse error。

例如：

```text
fn/parse.scm
  可以输出 “expected function parameter list”。

types/parse.scm
  可以输出 “expected type argument”。

effects/form.scm
  可以输出 “expected effect set after !”。
```

Parser 只负责低层拓扑错误，例如括号不匹配、非法 token。

## 27. Expansion Order

Meta pipeline 的推荐顺序：

```text
1. RawAst -> AstTree canonicalization
2. canonical binding parsing and std::module/import initializer dispatch
3. attribute collection
4. top-level domain parsing
5. macro registration
6. macro expansion
7. name resolution
8. type checking
9. effect checking
10. comptime evaluation / specialization
11. layout calculation
12. ABI lowering
13. control / closure lowering
14. LAIN-IR generation
15. LAIN-IR verification
```

具体实现可以分阶段演进，但边界不能反向污染。

尤其禁止：

```text
Parser 为了方便 type checking 而识别 type application。
AstTree helper 为了方便 lowering 而提供 expr parser。
Backend 为了方便 codegen 而重新解析 source-level struct。
```

## 28. Module and Import

Module 是 Meta 层语义。

Parser 不知道 module。

源码：

```lain
let io = import("std::io");

let math: Module = std::module {
    @export
    let add = std::func(x: i32, y: i32) -> i32 {
        x + y
    };
};
```

AstTree 只看到 atom、juxt、sep、group 等拓扑。

统一绑定解析器取得 initializer；`std::module` 与 `import("path")`
构造器分别负责模块构造和依赖加载。

Module elaboration 负责：

```text
module name resolution
import path resolution
visibility
symbol table construction
linkage name generation
cycle checking
incremental compilation dependency
```

LAIN-IR 中不应保留高层 module form。

Module 信息最终体现为：

```text
symbol namespace
mangled name
linkage metadata
global / proc visibility
```

## 29. Interface and Impl

Interface / trait-like 机制属于 Meta / Middle AST 层。

Parser 不知道 interface。

Middle AST 可以表达：

```text
middle.interface
middle.impl
constraint
associated type
method requirement
```

Elaboration 负责：

```text
constraint solving
method resolution
dictionary generation
monomorphization
witness table generation
```

Lowering 后可以变成：

```text
static call
dictionary parameter
witness table
function pointer table
specialized #proc
```

LAIN-IR 不应该直接承载高层 interface 语义。

## 30. Pattern Matching

Pattern 是 Meta / Middle AST 层语义。

Parser 不知道 pattern。

例如：

```lain
match x {
    Some(v) => v,
    None => 0,
}
```

AstTree 只表达 token 拓扑。

`pattern/parse.scm` 和 `control/parse.scm` 负责解释 pattern 和 match form。

Elaboration 负责：

```text
pattern type checking
exhaustiveness checking
binding introduction
desugaring
```

Lowering 后生成：

```text
tag load
comparison
branch
field offset
load
```

LAIN-IR 不应该直接拥有 high-level `match`。

## 31. Current Implementation Notes

当前实现中，部分文件可能还没有完全符合本合同。

已知迁移方向：

```text
std/meta/canonicalize.scm
  应只负责 RawAst -> AstTree，不生成语义节点。

std/meta/surface/tree.scm
  应只保留通用 tree helper。
  其中 type、expr、block、effect 解析应逐步迁出。

types/parse.scm
  应负责 type expression 和 type application 解析。

expr/parse.scm
  应负责 value expression 和 call 解析。

effects/form.scm
  应负责 effect set 和 effect application 解析。

control/parse.scm
  应负责 block、if、loop、match、return 等控制 form。

fn/parse.scm
  应负责 std::func initializer。

struct/parse.scm
  应负责 std::struct initializer。
```

特别注意：

```text
canonicalize.scm 不应该生成 (call callee args)。
```

括号后缀应保持为：

```text
(postfix callee (group paren args ...))
```

是否是 call，由 `expr/parse.scm` 决定。

是否是 type application，由 `types/parse.scm` 决定。

是否是 effect application，由 `effects/form.scm` 决定。

## 32. Migration Plan

建议按以下顺序迁移：

```text
1. 确认 LAIN-AST 合同：Atom / Group / Prefix / Postfix / Infix / Juxt / Sep。
2. 收紧 canonicalize.scm：只输出拓扑节点。
3. 从 surface/tree.scm 移除语义 parser。
4. 新增或完善 types/parse.scm。
5. 新增或完善 expr/parse.scm。
6. 新增或完善 effects/form.scm。
7. 新增或完善 control/parse.scm。
8. 新增或完善 fn/parse.scm。
9. 新增或完善 struct/parse.scm。
10. 建立 Middle AST 数据结构。
11. 建立 elaboration pass。
12. 建立 layout pass。
13. 建立 LAIN-IR lowering pass。
14. 建立 IR verifier。
15. 保证旧式独立 fn、struct、module、import 声明在 domain phase 被拒绝。
16. `<...>` 不进入核心语法；类型和值应用统一使用圆括号。
```

迁移时不需要一次性重写整个编译器。

优先级最高的是：

```text
不要让 LAIN-AST 层继续产生语义节点。
不要让 AstTree helper 继续包含 domain parser。
不要让 LAIN-IR 承载 source-level 高层语义。
```

## 33. Boundary Summary

Meta 系统边界总结：

```text
Parser
  只产出无语义拓扑。

AstTree
  只保存无语义拓扑的 Meta 层表示。

Domain Parser
  把拓扑解释为语言 form。

Middle AST
  保存 Lain 高层语言语义。

Elaboration
  完成名称解析、类型检查、effect 检查、comptime 特化、layout、ABI 等。

Lowering
  把已检查语义转换成物理 LAIN-IR。

LAIN-IR
  只表达物理执行。

Backend
  只做目标代码生成。
```

核心规则：

```text
LAIN-AST owns topology.
Meta owns language semantics.
LAIN-IR owns physical execution.
Backend owns target emission.
```

## 34. M10-M13 自举状态

当前实现已经越过“只能编译一个选定函数”的增量切片。稳定编译器源码闭包由
`tests/core/self_hosting/compiler_source.py` 中的有序模块列表定义，数据流为：

```text
compiler_source.lain
  -> RawAst
  -> Lain-owned Parsed/Middle/Elaborated programs
  -> Lain-owned structured lowering policy
  -> opaque physical L1Unit
  -> reloadable L1 text
```

M10 使 Meta nominal identity 与物理 ABI shape 分离；M11 使动态集合、符号表、
scope 和 diagnostics 由 Lain `CompilerContext` 所有；M12 固定显式阶段与失败原子性；
M13 则用这条管线编译完整的 Lain compiler source closure。

`@foreign(c, link_name = "...")` 在 RawAst 中仍只是 attribute topology。Lain
Middle/Lower 层解释它、把源码调用名重定位到物理 capability name，并按 link name
去重跨模块重复声明。C 不认识 `foreign` 的语言语义。

当前固定点不是只比较行为分数：

```text
stage1 compile(source) -> stage2.l1
stage2 compile(source) -> stage3.l1
stage2.l1 == stage3.l1  // byte-for-byte
```

stage2/stage3 都重新经过 L1 parser 与 verifier，compiler API schema 都为 1。
跨代门槛还要求普通函数和两模块 workspace 生成完全相同的 LAIN-IR，未知
procedure 生成完全相同的 2301 诊断；生成结果分别执行为 42。

这意味着日常 Meta compiler 的策略主体已经可以由 Lain 表达并达到固定点；
Scheme 仍用于制造 stage1 和承载尚未迁移的旧语言域，因此尚未从仓库删除。

## 35. M20.3 统一 Lowering

单文件和 workspace 保留两个编译入口，但不再各自维护表达式、语句和函数
lowerer。两者都构造 `LowerContext` 并进入同一条递归 lowering：

```text
plain root --------> LowerContext(mode = plain) -----\
                                                     -> expr/body/function -> L1Unit
workspace module --> LowerContext(mode = workspace) -/
```

模式差异只允许存在于两个查询中：

```text
source callable / foreign capability / module symbol -> physical link name
local or cross-module expression                     -> physical type tag
```

`consteval`、specialization、aggregate、enum/match、structured if/loop、
assignment 和普通调用均由共享 engine 处理。旧的 workspace-only
`lower_expr`、`lower_body`、`lower_function` 已删除；boundary lint
`M20_SINGLE_LOWERING_ENGINE` 防止平行实现重新出现。两个入口仍分别负责
workspace 的链接验证和 plain source 的入口选择，这属于编排差异，不属于
语言 lowering 策略。

## 36. M21 统一 Elaboration

单文件与 workspace 不再各自维护类型推断、表达式验证、语句体验证和函数验证。
两个入口都构造 `ElaborationContext`，再进入同一套语义引擎：

```text
plain root --------> ElaborationContext(mode = plain) -----\
                                                           -> type/expr/body/function
workspace module --> ElaborationContext(mode = workspace) -/
```

上下文只封装查找环境：

```text
ElaborationContext
  index          当前源码的 declaration/function index
  workspace      跨模块 summary 与导出查询
  module/root    当前语义作用域
  workspace_mode 是否允许跨模块 member resolution
```

因此 `if` 条件、调用参数、局部 binding、赋值、返回值、`consteval`、
aggregate、enum/match 与 specialization 的语义检查只有一个实现。workspace
模式增加的是成员解析能力，不是另一套语言规则。

兼容入口 `elaborator_module_expr_type_indexed` 仍保留给现有调用方，但它只创建
context 并转发到共享查询。旧的 `workspace_expr_type` 和
`workspace_validate_*` 递归实现已删除；boundary lint
`M21_SINGLE_ELABORATION_ENGINE` 防止它们重新出现。

parity 测试同时覆盖：

```text
跨模块成员解析成功并执行为 42
workspace 内局部调用和 consteval 执行结果为 42
单文件与 workspace 对错误 if 条件都报告 2401
```

这一步与 M20.3 合并后的前端主路径是：

```text
RawAst
  -> surface forms
  -> ElaborationContext (one semantic engine)
  -> LowerContext (one physical lowering engine)
  -> L1Unit
```
