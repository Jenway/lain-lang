# M20.4 Code Health Audit

Snapshot: 2026-07-25, after M20.3 unified lowering.

## Result

The compiler is a self-hosting, testable prototype with a stable physical
boundary. It is not yet a small or easily replaceable compiler. The next
engineering goal is to reduce duplicated elaboration and the width of the host
ABI, not to add another language feature.

## Measured size

| Area | Files | Lines |
| --- | ---: | ---: |
| Lain compiler (`packages/lain/compiler`) | 26 | 12,910 |
| C compiler host (`src/compiler`) | 15 | 7,341 |
| C RawAst (`src/lainast`) | 3 | 901 |
| C LAIN-IR (`src/lainir`) | 11 | 4,112 |
| Python test/build orchestration (`tests`) | 19 | 2,671 |

The four largest Lain compiler modules are:

| Module | Lines | Function/struct declarations |
| --- | ---: | ---: |
| `elaborator.lain` | 2,400 | 118 |
| `lower.lain` | 1,706 | 70 |
| `syntax.lain` | 1,343 | 211 |
| `modules.lain` | 1,012 | 97 |

The source contains 35 explicit `TODO`/`FIXME`/compatibility/legacy/debt
markers. The active Lain compiler source declares 163 `@foreign` bindings.
The generated stage2 compiler retains 139 physical extern capabilities:

| Capability family | Count |
| --- | ---: |
| structured L1 construction/query/evaluation storage | 62 |
| RawAst and generated syntax storage | 44 |
| Meta syntax builder compatibility facade | 16 |
| string/debug/unit helpers | 10 |
| compiler-owned growable storage host | 7 |

## What is healthy

- `stage2 == stage3` is byte-for-byte, not merely behaviorally equivalent.
- The 13 core suites pass, including comptime, user Meta, enum/match,
  workspace, structured control flow, boundary lint and bootstrap-core.
- RawAst does not own source-language keywords.
- LAIN-IR does not own `enum`, `match`, generic, Meta or structured source
  semantics.
- M20.3 removed the parallel workspace expression/body/function lowerers.
  `M20_SINGLE_LOWERING_ENGINE` prevents them from returning.
- Failure is atomic at the compiler result boundary: unsuccessful compilation
  does not expose a partial unit.

## Concrete risks

### 1. Elaboration is still duplicated

Lowering is unified, but `elaborator.lain` still contains both
`elaborator_validate_expr` and `workspace_validate_expr`, plus parallel body
and statement validation. This is now the largest semantic divergence risk.

Required correction: introduce an `ElaborationContext` analogous to
`LowerContext`. Keep module lookup as a context query and use one validation
and type-inference recursion.

### 2. Four modules are too large

`elaborator.lain`, `lower.lain`, `syntax.lain` and `modules.lain` collectively
hold most compiler policy. Their current names describe layers, but each file
contains several independently testable domains.

Required correction after unified elaboration:

```text
elaborator/
  context, expressions, statements, declarations, specialization
lower/
  context, expressions, statements, aggregates, specialization
syntax/
  topology, forms, queries, source identity
modules/
  values, summaries, dependency graph, resolution
```

This is a source-organization change only. It must not add semantic layers.

### 3. The host ABI is broad

139 externs make the self-hosted artifact sensitive to low-level storage
details. Most are mechanically simple, but together they form a large
compatibility surface.

Required correction: replace families of scalar getters/setters with three
versioned opaque protocols:

```text
SyntaxStore API
L1Unit API
CompilerStorage API
```

The first goal is fewer exported capability names and explicit schema
versions, not moving memory management into Lain prematurely.

### 4. Self-compilation is slow

One observed uncached stage1-to-stage2 generation after M20.3 took roughly
400 seconds on this machine. This is an observation, not a controlled
benchmark. The build currently gives no per-phase timing or procedure count.

Required correction: add phase timing for parse, expand, elaborate, lower,
verify and emit; record generated procedure/expression counts. Optimize only
after the measurements identify the dominant phase.

### 5. The bootstrap seed is reproducible but slow

The canonical constructor parser and its rejection fixtures are committed as
`bootstrap/stage0@a11f26affc9ea6c53d4a5551129ccf5164f19b79`.
The cold-bootstrap gate exports that exact commit, ignores the bootstrap
worktree, bypasses all artifact caches and successfully reaches:

```text
stage0 -> stage1 -> stage2 -> stage3
stage2 == stage3
```

This proves recovery from committed source. It also makes the uncached
bootstrap cost visible: each full compiler generation takes several minutes
on the current machine, so phase timing remains required.

## Ordered repair plan

1. Unify single/workspace elaboration through `ElaborationContext`.
2. Add compiler phase timings and size counters.
3. Split the four oversized Lain modules without changing public ABI.
4. Version and compress the three host protocols.

Feature work should resume only after items 1 and 2.
