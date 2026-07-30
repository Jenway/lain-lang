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
