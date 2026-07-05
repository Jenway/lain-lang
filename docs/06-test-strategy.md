# Test Strategy

Lain tests are being split into core contract tests and legacy fixtures.

Passing many weak tests is not a quality signal. A core test must protect a compiler contract.

## 1. Test Tiers

```text
tests/core/
  ast/
  boundaries/
  meta/
  ir/
  comptime/
  effects/
  imports/
  runtime/

tests/bootstrap-core/
  data_structures/
  control/
  meta/
  comptime/
  ir_builder/

tests/legacy/
  fixtures/
```

`tests/core` is the quality gate. It should stay small.

`tests/bootstrap-core` is the `0.1.0` self-hosting gate. It proves that the old-world compiler can compile the Lain subset needed to write compiler code.

`tests/legacy` is a sample and regression archive. It may contain many historical fixtures, but its count is not a proxy for compiler health.

The existing `tests/fixtures` tree remains in place until migration. It should be treated as legacy even before it is physically moved.

## 2. Core Test Rule

Every core test must answer this question:

```text
If this test fails, which compiler contract was broken?
```

If the answer is unclear, the test does not belong in `tests/core`.

Good core tests:

- lock a layer boundary
- exercise a complete critical path
- catch a known architectural failure mode
- use small input and strong assertions

Weak core tests:

- only check that a file compiles
- match one incidental output string
- duplicate another test with different spelling
- exist mainly to increase the pass count

## 3. Core Suites

### AST

Path:

```text
tests/core/ast/
```

Purpose:

```text
Source text -> RawAst/AstTree topology contract
```

These tests ensure that the AST layer stays topology-only.

Required cases:

```lain
foo(i32)
a < b > c
Vec(i32)
fn identity(comptime T: type, x: T) -> T { x }
#[foreign(link_name = "puts")]
fn puts(s: CStr);
fn f() -> i32 ! {Throws(i32), Suspend} { 0 }
```

Forbidden in AST golden output:

```text
call
type-app
effect-name
return
let
middle
raw.node
```

### Boundaries

Path:

```text
tests/core/boundaries/
```

Purpose:

```text
static checks for layer pollution
```

This suite starts as debt-aware. Existing known debt is allowed. New debt fails.

Strict mode treats all debt as failure.

### Meta

Path:

```text
tests/core/meta/
```

Purpose:

```text
AstTree -> Middle AST ownership by domain parser
```

Each domain should have one minimal contract test:

```text
fn parser owns function declarations
struct parser owns struct declarations
types parser owns type application
effects parser owns effect application
attrs parser owns attributes
import parser owns imports
```

These should prove that surface/C did not steal domain semantics.

### IR

Path:

```text
tests/core/ir/
```

Purpose:

```text
Middle AST -> LAIN-IR physical shape
```

Use a few structural golden outputs, not many incidental text matches.

Initial cases:

```lain
fn add(a: i32, b: i32) -> i32 { a + b }
fn local() -> i32 { let x = 1; x }
struct Point { x: i32, y: i32 }
```

### Comptime

Path:

```text
tests/core/comptime/
```

Purpose:

```text
comptime execution goes through LAIN-IR evaluation
```

Initial cases:

```text
pure comptime function
host capability
failure path
```

### Effects

Path:

```text
tests/core/effects/
```

Purpose:

```text
effect ABI and lowering contract
```

Initial cases:

```text
single effect perform
handle effect
multi-effect product/tag layout
```

### Imports

Path:

```text
tests/core/imports/
```

Purpose:

```text
import/interface/build path is owned by meta, not C source scanning
```

Initial cases:

```text
provider -> consumer
re-export or alias
```

### Runtime

Path:

```text
tests/core/runtime/
```

Purpose:

```text
end-to-end backend/runtime sanity
```

Initial cases:

```text
main() -> i32 exit code
main() -> unit exit 0
println stdout
```

### Bootstrap Core

Path:

```text
tests/bootstrap-core/
```

Purpose:

```text
old world can compile the Lain subset needed for self-hosting
```

Initial suites:

```text
data_structures
control
meta
comptime
ir_builder
```

These tests are the release gate for `0.1.0`. They should not become a general language feature suite.

## 4. Runners

Core runner:

```bash
python tests/core_runner.py
```

Bootstrap Core runner:

```bash
python tests/bootstrap_core_runner.py
```

This runner does not exist yet. Until it exists, `tests/bootstrap-core/README.md` is the source of planned cases.

Legacy runner:

```bash
python tests/runner.py
```

The current `tests/runner.py` remains the legacy runner until the tree is migrated.

Core reports should be short:

```text
core/ast         pass
core/boundaries  pass, known debt tracked
core/meta        pending
...
```

Do not use the legacy fixture count as the health metric.

## 5. Migration Plan

1. Create `docs/06-test-strategy.md`.
2. Create `tests/core_runner.py`.
3. Move boundary lint under `tests/core/boundaries`.
4. Add AST golden tests using a small RawAst dump tool.
5. Add pending directories for meta/ir/comptime/effects/imports/runtime.
6. Add core tests from scratch.
7. Move old fixtures under `tests/legacy/fixtures` only after core is useful.
8. Delete weak legacy tests when they do not protect a behavior worth keeping.

## 6. Quality Metric

Use this shape:

```text
Core contracts: N/N passing
Bootstrap Core: N/N passing or pending with explicit contracts
Known boundary debt: M
Legacy fixtures: X/Y passing
```

Do not report only a single total pass count.
