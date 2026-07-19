# Test Strategy

Lain tests are split into small compiler contracts and Bootstrap Core gates.

Passing many weak tests is not a quality signal. A core test must protect a compiler contract.

## 1. Test Tiers

```text
tests/core/
  ast/
  declarations/
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

```

`tests/core` is the quality gate. It should stay small.

`tests/bootstrap-core` is the `0.1.0` self-hosting gate. It proves that the
bootstrap compiler can compile the Lain subset needed to write compiler code.

The former broad `tests/fixtures` tree and its runner were removed. Historical
syntax is not retained as a compatibility suite: old declaration spellings are
represented only by focused rejection contracts.

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
let identity = std::func(comptime T: type, x: T) -> T { x }
@foreign(link_name = "puts")
let puts = std::func(s: CStr);
let f = std::func() -> i32 ! {Throws(i32), Suspend} { 0 }
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

### Declarations

Path:

```text
tests/core/declarations/
```

Purpose:

```text
let NAME[: TYPE] = RHS is the only declaration form
```

The normal self-hosted CLI must accept `std::func`, `std::struct`,
`std::module`, and `import` as binding initializers. It must reject historical
`fn NAME`, `struct NAME`, standalone `import`, and any binding whose type
cannot be inferred. Pending contracts remain visible until the implementation
provides the specified stable diagnostics.

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

Each domain should have one minimal constructor/binding contract test:

```text
binding parser owns let NAME[: TYPE] = RHS
std::func Meta constructor owns function formation
std::struct Meta constructor owns type/layout formation
types parser owns type application
effects parser owns effect application
attrs parser owns attributes
import Meta constructor owns dependency formation
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
let add = std::func(a: i32, b: i32) -> i32 { a + b }
let local = std::func() -> i32 { let x = 1; x }
let Point: type = std::struct { x: i32, y: i32 }
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
python tests/bootstrap-core/run_bootstrap_core.py
```

Core reports should be short:

```text
core/ast         pass
core/boundaries  pass, known debt tracked
core/meta        pending
...
```

## 5. Syntax Migration Rule

There is no source compatibility phase before the language is complete.
Examples and positive tests use only canonical bindings. A removed spelling is
kept only as one focused negative test with an exact diagnostic contract.

## 6. Quality Metric

Use this shape:

```text
Core contracts: N/N passing
Bootstrap Core: N/N passing or pending with explicit contracts
Known boundary debt: M
Pending canonical declaration contracts: P
```

Do not report only a single total pass count.

## 7. M7/M8 Differential Execution Gate

`tests/core/self_hosting/lain_interpreter_comptime.lain` protects the
interpreter/comptime ownership boundary. It must prove:

```text
same M6 cross-module L1Unit: C interpreter == Lain interpreter == 42
recursive temporary L1Unit: runtime == comptime == materialized result == 55
two comptime runs: identical result
invalid source: nonzero source diagnostic status
named-unit extern call: rejected with status 7002
```

The test must execute structured units directly. Emitting and reparsing L1 text
does not satisfy this gate.

## 8. M9 Structured Compiler State Gate

`tests/core/self_hosting/run_self_hosting.py` and
`compile_result_diagnostics.lain` must prove:

```text
begin state: nested SyntaxRef remains readable across a call
with-unit state: OptionalUnit and module_count remain readable
finish state: success returns the complete unit and outcome
valid source: CompileResult.ok == 1
invalid source: CompileResult.ok == 0 and unit == None
invalid source: exactly one stable code/message/span diagnostic
failure: no partial L1Unit is returned
self-compile closure: compiler_state.lain is present and schema == 9
```

The test must invoke the procedures in the reusable artifact. Constructing a
parallel host-side result or checking only a source-level score does not satisfy
the gate.

## 9. M10-M14 Fixed-point And CLI Gate

`tests/core/self_hosting/run_self_hosting.py` is the authoritative M10-M14
gate. It must prove all of the following in one run:

```text
M10: lci-v2 preserves semantic TypeIdentity and ABI shape without mirrors
M11: Lain-owned CompilerContext collections/scopes/diagnostics execute
M12: parse -> Middle -> elaborate -> structured L1 phases succeed atomically
M13: stage1 emits verifier-clean stage2
M13: stage2 emits verifier-clean stage3
M13: stage2 and stage3 are byte-identical
M13: stage2 and stage3 report schema 12
M14: normal lainc --emit-l1 loads the stage2 artifact and executes main == 42
M14: --emit-workspace-l1 links two Module files and executes app__main == 42
M14: invalid input reports diagnostic 2301 and leaves no partial output
```

`artifact_generation_behavior.lain` additionally runs stage1, stage2, and
stage3 against the same ordinary program, invalid program, module workspace,
and comptime program. Text equality without these behavior checks is not a
sufficient fixed-point proof.

Stage1 and stage2 artifacts are cached by a SHA-256 fingerprint of their full
source/tool inputs and rechecked with `l1check` on a cache hit.  An existing
stage3 is reused only when its bytes already equal the current verified
stage2.  Full compiler-closure compilation belongs to the two generation
gates; smaller runtime fixtures must not compile that closure again.

Run it through the Zig build graph:

```text
zig build test-self-host
```
