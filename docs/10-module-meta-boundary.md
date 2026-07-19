# Module Meta Boundary

本文档定义 Lain 的长期 module 机制。

结论先写清楚：

```text
module/import/export/package 是 META 概念。
LAIN-IR 不拥有 source-level module system。
.lci 是旧世界 bootstrap artifact，不是长期 IR 接口。
```

## 1. Design Goal

Module 机制要解决的是编译期名字空间和代码组织：

```text
package root
module path
import binding
export visibility
source-level alias
cross-module name resolution
```

这些都不属于运行时物理执行模型。它们应该在 meta 阶段被解释掉。

LAIN-IR 只接收已经解析后的物理事实：

```text
procedure
global
type/layout
extern declaration
link name
visibility/linkage primitive
call target
```

## 2. Layer Contract

长期 pipeline 应该是：

```text
Source Text
  -> LAIN-AST topology
  -> Meta module resolver
  -> Meta domain passes
  -> LAIN-IR unit
  -> backend/linker
```

Module resolver 的输入是 LAIN-AST 或 meta-normalized bindings。

Module resolver 的输出不是 LAIN-IR module node，而是 meta artifact：

```text
ModuleSummary
ResolvedImports
ExportTable
LinkagePlan
```

LAIN-IR unit 可以保存一组 procedure/global/type declarations，但这里的 unit 只是物理编译单元，不是 source-level module。

## 3. Terms

### Package

Package 是 source tree 的根和 dependency 边界。

它回答：

```text
这个 module path 从哪个 root 开始解析？
这个 package 依赖哪些其他 package？
哪些 module 是 public entry？
```

Package 是 meta/build 概念。LAIN-IR 不知道 package。

### Module

Module 是 source-level namespace。

例子：

```lain
let ast_sexpr = import("packages::lain::compiler::ast_sexpr");
```

这条路径应该由 meta resolver 解释为：

```text
ModuleId(package=packages/lain, path=compiler/ast_sexpr)
```

Module 可以对应一个文件，也可以对应 meta 生成的 virtual module。这个选择属于 resolver。

### Import

Import 是 compile-time name binding。

它不生成 IR instruction。

它只改变 meta environment：

```text
local name -> exported declaration
qualified path -> exported declaration
```

### Export

Export 是 source-level visibility。

它不等同于 backend symbol visibility。

Meta 可以把 public source declaration 降成：

```text
internal name
resolved canonical name
optional backend link name
optional external visibility
```

只有最后两项才进入 LAIN-IR linkage primitive。

### Linkage

Linkage 是物理符号层。

LAIN-IR 可以表达：

```text
internal procedure
extern procedure
exported/link-visible procedure
link_name string
```

它不表达：

```text
source dependency binding
export { name }
source module namespace
package dependency
```

## 4. Correct Module Flow

### Step 1: Parse Topology

RawAst / AstTree 只保留拓扑。

It must not recognize:

```text
import
export
module
pub
package
```

这些 token 可以是普通 atom，但不能在 AST 层获得语义。

### Step 2: Meta Interprets Unified Bindings

M5 不为 module/import/export 增加独立 declaration grammar。Meta 统一解释：

```lain
let math: Module = std::module {
    @export
    let add = std::func(a: i32, b: i32) -> i32 { a + b };
};

let app: Module = std::module {
    let math = import("math");
    @export
    let main = std::func() -> i32 { math.add(40, 2) };
};
```

统一形式是：

```text
let NAME : EXPECTED_SHAPE = INITIALIZER
```

`std::module`、`import("path")`、`std::func` 都是 Meta constructor。`Module` 是期望的
MetaValue shape，`@export` 是后继 binding 的 Meta metadata。RawAst 只保留
这些 token 的拓扑，不理解 module 语义。

### Step 3: Resolve Module Graph

Module resolver 负责：

```text
path canonicalization
source file discovery
cycle detection
visibility checks
alias expansion
re-export resolution
diagnostics
```

C 可以提供 file IO 和 path normalization capability，但不能扫描 Lain source 来推断 imports。

### Step 4: Build Module Environment

每个 module 得到一个 meta environment：

```text
private declarations
public export table
import bindings
canonical symbol map
```

Source-level names 在这里被解析。

### Step 5: Lower To LAIN-IR

Meta lowerer 把 resolved declarations 降成 LAIN-IR：

```text
let foo = std::func(...) -> ...
  -> #proc canonical_or_internal_name(...)

extern "c" link_name = "puts"
  -> #extern local_name link_name(...)
```

LAIN-IR 看到的是 resolved physical names。

它不需要再知道这些名字来自哪个 import。

## 5. Module Artifact

长期需要一个 meta-owned module artifact。

先称为：

```text
ModuleSummary
```

它可以被序列化，但序列化格式不是 LAIN-IR。

最小内容：

```text
module_id
source_hash
exports:
  name
  kind
  type_signature
  canonical_symbol
  link_name when needed
dependencies:
  module_id list
diagnostics metadata
```

这个 artifact 的用途：

```text
incremental compile
cross-module checking
bootstrap import bridge
editor tooling
```

## 6. Status Of .lci

当前 `.lci` 是旧世界 artifact。

它现在混合了：

```text
module summary
export table
function signature
link name
old import bridge input
```

这可以作为 bootstrap 脚手架继续保留，但不能作为长期 module 机制。

长期规则：

```text
.lci belongs to legacy bridge.
ModuleSummary belongs to meta.
LAIN-IR does not read or write .lci.
```

迁移时可以先让 `.lci` 的内容越来越接近 `ModuleSummary`，但 ownership 必须从 C runtime 移到 meta/Lain library。

## 7. LAIN-IR Primitive Boundary

LAIN-IR 可以保留这些 primitive：

```text
proc declaration
extern declaration
global declaration
type/layout declaration
link_name
calling convention
visibility/linkage flag
```

LAIN-IR 不应保留这些 source-level 状态：

```text
export name list
declared module names
declared signature names
import binding registry
source module prefix policy
.lci emission policy
```

当前 `src/lainir/` 里还有 export/module state。这是 legacy debt。

## 8. Bootstrap Migration Plan

### Phase 0: Mark Boundaries

Keep current `.lci` working.

Add boundary lint so new source-level module semantics do not leak into LAIN-IR.

### Phase 1: Move Interface Emission Out Of C Runtime

Current:

```text
native_runtime.c emits .lci
```

Target:

```text
meta module library emits ModuleSummary
legacy bridge can serialize ModuleSummary as .lci while old import path needs it
```

### Phase 2: Remove Source Export State From LAIN-IR

Current debt:

```text
g_export_names_head
g_declared_module_names_head
native_mark_export
native_declare_module
```

Target:

```text
meta resolves exports
IR receives direct linkage flags or link names
```

### Phase 3: Implement Resolver In Lain

M5 已实现的 bootstrap slice：

```text
packages/lain/compiler/surface_forms.lain
packages/lain/compiler/modules.lain
packages/lain/compiler/workspace.lain
```

Responsibilities:

```text
ModuleId
ImportDecl
ExportTable
ModuleSummary
path resolution helpers
visibility checking
cycle diagnostics
```

当前实现以一个 workspace RawAst root 表示多个显式 Module binding，支持 export
summary、import dependency、重名/缺失/环诊断、跨模块可见性检查，并在 lowering
时把 `math.add` 解析成物理符号 `math__add`。这仍是 zero-copy bootstrap
ModuleSummary view；持久化 artifact、package path/source discovery 和增量 cache
留给后续阶段。

### Phase 4: Retire .lci As Public Concept

Old commands may remain temporarily:

```text
lainc --emit-interface
```

But the implementation should become:

```text
compile module through meta resolver
emit serialized ModuleSummary for legacy consumers
```

After bootstrap moves far enough, rename or remove the command.

## 9. Tests

Boundary tests should enforce:

```text
src/lainir does not gain new source-level module/export state
.lci usage remains confined to legacy bridge paths
C runtime does not scan source text for imports
LAIN-AST parser does not recognize module/import/export keywords
```

Bootstrap tests should verify:

```text
ModuleSummary-shaped data can be represented in Lain
import/export resolution helpers compile in packages/lain/compiler
legacy .lci bridge still works until removed
```

当前自举验收是：

```text
math + app workspace
  validates 2 modules / 1 dependency
  exposes only @export bindings
  rejects missing dependency, private access, duplicate module and cycle
  lowers app.main -> app__main and math.add -> math__add
  executes app__main == 42
```

## 10. Stop Rule

Do not make `.lci` smarter to solve language design.

Use `.lci` only to keep the old compiler running while meta-owned module artifacts are built.

If a module feature requires policy, it belongs in meta or `packages/lain/compiler`, not in LAIN-IR or C runtime.
