# Scheme Host Boundary

本文档定义 Lain 旧世界 meta compiler 的 Scheme 宿主边界。

目标不是绑定某个 Scheme 实现。目标是让 meta 代码面向一个可移植 Scheme contract，具体解释器只是 backend。

## 1. Contract

Lain meta code may rely on:

```text
define
lambda
if
cond
let / let* / letrec
begin
set!
quote
pairs and lists
symbols
R7RS escaped identifiers, such as |middle.fn|
strings
numbers
booleans
load
basic equality predicates
basic arithmetic
```

The contract intentionally excludes:

```text
define-syntax / syntax-rules
named let
backend-specific reader extensions
backend-specific exception object shapes
```

Meta code may call host capabilities only through registered bindings:

```text
core.*
ast.*
type.*
```

The binding names are part of the Scheme host contract. Their concrete C ABI is not.

Build/package orchestration is not part of this compiler driver contract.

The intended direction is:

```text
build.lain or meta/comptime build library -> compiler core APIs
```

not:

```text
lainc --build -> C-side source scanner / linker driver
```

## 2. Backend Rule

Current Scheme backends:

```text
Chibi  existing Linux backend
Gauche Windows-native backend
```

Both are implementation details.

The compiler core must include only:

```c
#include "compiler/vm_api.h"
```

or the temporary compatibility layer:

```c
#include "compiler/vm_compat.h"
```

Concrete backend headers are allowed only inside backend files:

```text
src/compiler/vm_chibi.c   may include chibi/eval.h
src/compiler/vm_gauche.c  may include gauche.h / gauche/*
```

Backends must not repair or rewrite meta source code. In particular, backend files must not:

```text
preprocess Scheme source text
rewrite named let
emulate define-pass syntax
install reader hacks for meta files
```

If meta code depends on syntax outside the contract, the meta code must be migrated.

## 3. Forbidden Meta Dependencies

Meta code must not depend on:

```text
Chibi modules
Chibi exception object shape
Chibi FFI syntax
Gauche object system extensions
Gauche reader extensions
implementation-specific module systems
```

If a feature is needed by meta code, expose it as a portable helper or a `core.*` host binding.

Current tracked portability debt:

```text
none for R7RS escaped identifiers
```

R7RS escaped identifiers are part of the host contract. A backend that does not
read them must either be rejected or implement this reader feature as part of
host conformance, not as a meta-source rewrite.

## 4. C FFI Boundary

The C side exposes functions through `vm_register_ffi`.

Backend-specific files translate that registration into their local ABI:

```text
vm_chibi.c   -> sexp_define_foreign_aux
vm_gauche.c  -> Gauche subr wrapper + dispatch table
```

`native_runtime.c` and `builder_ffi.c` must not call concrete Scheme APIs directly.

They currently still use `sexp`-shaped names through `vm_compat.h`. This is compatibility debt, not the target design.

Target shape:

```c
vm_value *fn(vm_context *ctx, vm_value *self, int nargs, vm_value **args);
```

or another backend-neutral form defined in `vm_api.h`.

## 5. Windows Rule

Windows native uses Gauche as the first supported backend.

`pixi.toml` can remain Linux-only for now. Windows native build must not require Pixi or Chibi.

Required command shape:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-gauche.ps1
```

or an equivalent build command using the same backend selection:

```bash
make SCHEME_BACKEND=gauche lainc
```

## 6. 0.1.0 Gate

`0.1.0` requires:

```text
Scheme host boundary documented
boundary lint tracks backend leaks
Gauche backend can run the Windows native meta pipeline
meta code does not intentionally add implementation-specific Scheme dependencies
```

It does not require all historical Scheme backend debt to be removed before Bootstrap Core work continues.

## 7. Legacy Build Driver Debt

The current tree still contains a C-side `--build` path:

```text
src/compiler/native_compiler.c
src/compiler/native_runtime.c
tests/runner.py
```

This path scans source/imports and drives object compilation/linking from C.
It conflicts with the Lain design because build orchestration should be a meta or compile-time library concern.

Do not extend this path for new backends. Gauche support only needs the normal single-file compiler/meta pipeline:

```text
lainc --emit-l1 input.lain output.l1
lainc --emit-interface input.lain output.lci
lainc input.lain output.c
```

The migration target is to replace the legacy C driver with `std/build.lain` or a later `build.lain` convention.

## 8. Rejected Alternatives

s7 was removed as a Windows backend candidate.

Reason:

```text
s7 did not satisfy the current Scheme Host Contract directly.
The rejected implementation path required backend-side source rewriting or syntax repair.
That would make meta code depend on a concrete backend instead of the portable host contract.
```

A future backend may be added only if it either satisfies the contract directly or exposes missing host behavior without rewriting meta source files.
