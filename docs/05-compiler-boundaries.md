# Compiler Boundaries

本文档定义 Lain 编译器的硬边界。

这些规则不是风格建议。它们用于判断某个实现是否把语言语义放错了层。

## 1. Layer Contract

```text
Source Text
  -> RawAst          C arena tree, topology only
  -> AstTree         Scheme value tree, topology only
  -> Middle AST      meta domain semantics
  -> LAIN-IR         physical execution model
  -> Backend         C / native output
```

每一层只能依赖前一层公开的结构，不能偷偷解释下一层语义。

## 2. AST Rules

### A1. RawAst Is Topology Only

RawAst may contain:

```text
Atom
Group
Prefix
Postfix
Infix
Juxt
Sep
```

RawAst must not contain:

```text
call
type-app
block
return
let
effect-name
attr
fn
struct
import
interface
```

The C parser may tokenize punctuation and build precedence structure. It must not recognize Lain source-level keywords.

### A2. AstTree Is Topology Only

`canonicalize.scm` converts RawAst handles into Scheme values. It may rename physical shapes, but it must not create Middle AST concepts.

Allowed output shapes:

```scheme
(atom ...)
(group ...)
(prefix ...)
(postfix ...)
(infix ...)
(juxt ...)
(sep ...)
```

Forbidden output shapes:

```scheme
(call ...)
(type-app ...)
(stmt.return ...)
(expr.call ...)
(effect.name ...)
```

`foo(...)` must stay a postfix/group topology. It is not a call until a domain parser says so.

## 3. Surface Rules

### S1. Surface Helpers Do Tree Work Only

Surface helpers may provide:

```scheme
tree.kind
tree.children
tree.left
tree.right
tree.flatten-juxt
tree.split-by-sep
form.keyword
form.tree
```

Surface helpers must not parse language domains.

Forbidden in the surface layer:

```scheme
tree-parse-type
tree-parse-block
tree-parse-effect-set
tree-parse-attrs
tree-parse-params
tree-lower-expr
raw.node!
middle.node!
```

Those belong in `types/`, `control/`, `effects/`, `attrs/`, `expr/`, or another domain directory.

### S2. Surface Does Not Lower

The surface layer must not lower directly to Middle AST or LAIN-IR. It can expose topology. Domain passes assign meaning.

## 4. Meta Domain Rules

### M1. Domain Parsers Own Semantics

Only domain parsers may turn AstTree into semantic nodes.

Examples:

```text
fn/parse.scm          owns function declarations
types/parse.scm       owns type expressions
expr/parse.scm        owns value expressions
effects/form.scm      owns effect declarations and effect sets
attrs/parse.scm       owns attributes
import/parse.scm      owns import declarations
```

Shared helpers are allowed, but shared helpers must not become a hidden second parser.

### M2. Generic Syntax Uses Ordinary Topology

Types are first-class values. Generic application should use ordinary paren topology:

```lain
Vec(i32)
Result(i32, Error)
Throws(i32)
identity(i32, 10)
```

`<T>` syntax is not the preferred generic form. It forces parser or AstTree code to guess type context.

## 5. C Host Rules

### C1. C May Host, Not Define Lain Semantics

C may implement:

```text
RawAst arena and parser
LAIN-IR data structures
LAIN-IR parser and interpreter
C emitter
file/process/runtime capabilities
Scheme FFI bridge
```

C must not implement:

```text
source-level import scanning
fn/struct/effect/interface parsing
generic syntax policy
stdlib type aliases
implicit source-level declarations
```

### C2. C Must Not Synthesize Source-Level Declarations

C must not create missing Lain functions, externs, modules, or imports by guessing. Missing declarations should be errors unless a meta pass explicitly declared them.

### C3. C May Expose Capabilities, Policy Lives In Meta

Host file IO, process execution, environment access, and runtime calls may be exposed as capabilities. The decision to use them belongs in meta/std code.

### C4. Module Policy Lives In Meta

Module/import/export/package rules are source-level policy.

C may expose file IO and path utilities. It must not decide module graph policy, export visibility, or package resolution.

`.lci` is a legacy bootstrap artifact. It should not become the long-term module system.

See `docs/10-module-meta-boundary.md`.

### C5. Structured L1 Builder Is A Physical ABI

C may allocate physical L1 nodes, keep them behind opaque handles, verify and
execute a completed unit, emit a debug text view, and destroy the unit.

C must not interpret source bindings, modules, type aliases, visibility, or
choose linked procedure names. Those policies belong to Lain Meta. The `l1.*`
builder capability set is therefore a physical storage ABI, not a second
source-language frontend.

### C6. Interpreter Storage Is Not Interpreter Policy

During bootstrap, C may expose read-only physical L1 node queries and opaque
frame/result storage because the current Lain subset cannot yet allocate its
own dynamic maps. C must not evaluate expressions, walk control flow, resolve
calls, or dispatch target externs on behalf of the Lain interpreter.

Those decisions live in `packages/lain/compiler/l1_interpreter.lain`. The C
interpreter may remain as a reference implementation and runtime fallback, but
M8 comptime evaluation must use the Lain-owned interpreter path.

### C7. Compiler State And Result Policy Belong To Lain

Syntax/L1 handles may remain opaque physical values during bootstrap, but
phase transitions, diagnostic collection, success/failure, optional-unit
policy and failure atomicity belong to `compiler_state.lain`.

C or Scheme must not manufacture a successful result, translate a diagnostic
code into success, or expose a partially built L1Unit. They may retain
aggregate storage for one interpreter run and copy a diagnostic string at the
FFI boundary. Those are lifetime/representation operations, not compiler
policy.

## 6. Pipeline Rules

### P1. Missing Passes Should Not Be Silent

Missing required passes should produce diagnostics. A silent no-op is allowed only for explicitly optional extension points.

### P2. Unknown Top-Level Forms Should Not Become Fake Programs

Unknown forms should not silently become `main` or another declaration. If implicit-main syntax exists, it must preserve the input body and be specified as a language feature.

## 7. Current Known Boundary Debt

These are known violations in the current implementation. They are tracked by `tests/core/boundaries/boundary_lint.py`.

### Debt S

`std/meta/surface/tree.scm` contains domain parsers and lowerers:

```text
tree-parse-type
tree-parse-params
tree-parse-attrs
tree-parse-effect-set
tree-parse-block
tree-lower-expr
raw.node!
middle.node!
```

These should move into domain files.

### Debt C

`src/compiler/native_runtime.c` scans source text for `import`.

Build graph discovery should go through the meta import/build pipeline or a specified AST/Middle interface.

### Debt F

`src/compiler/builder_ffi.c` hardcodes some source/std-level type names and auto-creates default extern stubs.

The C side should expose physical constructors. Meta/std should own names and declarations.

### Debt M

Module/interface debt remains in old runtime and LAIN-IR support code:

```text
native_runtime.c emits/reads .lci
src/lainir owns source-level export/module lists
builder_ffi.c exposes declare-module and mark-export bridges
```

These are bootstrap bridge mechanisms. Long-term module policy belongs in meta.

## 8. Resolved Boundary Debt

### Resolved A

`std/meta/canonicalize.scm` used to emit `(call callee args)` for postfix paren topology.

The fixed shape is:

```scheme
(postfix callee (paren args ...))
```

This should stay fixed. Boundary lint now allows zero semantic AstTree nodes from `canonicalize.scm`.

## 9. Boundary Tests

Boundary tests live under:

```text
tests/core/boundaries/
```

The first test is debt-aware. It allows the known debt above, but fails when new violations are added.

Run it with:

```bash
python tests/core/boundaries/boundary_lint.py
```

Use strict mode to see all current debt as failures:

```bash
python tests/core/boundaries/boundary_lint.py --strict
```
