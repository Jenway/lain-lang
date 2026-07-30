# Pure-Lain policy modules and effects

The pure-Lain standard library uses three distinct concepts:

- a Module is a compile-time namespace and policy value;
- an effect states which operation a function may perform;
- a handler supplies the implementation and transforms the remaining effects.

There are no allocator, platform, source-reader or console callback tables.

## Allocation and bounds

`std::allocation` defines the parameterized effect `Alloc(Policy)` and the
Module shape implemented by an allocation policy.  `std::core::arena` exports
one such policy and a handler over caller-supplied memory.  The handler may
replace `Alloc(Arena)` with `Throws(OutOfMemory)`; it never obtains pages.

`std::bounds` defines `Check(Policy)`.  Checked, unchecked and trapping
policies install different handlers.  Containers do not return a fixed
`BoundsError`.

## Types with namespaces

`Vec(T, Allocation, Bounds)` is specialized by one type and two Module values.
The result is a type with an associated namespace:

```lain
let IntVec: type = vector.Vec(
    i32,
    arena.Policy,
    bounds.Checked,
);

let values: IntVec = IntVec.new();
IntVec.push(&mut values, 42);
let first: &i32 = IntVec.get(&values, 0);
```

String follows the same policies.  `MemoryModel` bundles the chosen policies
and their derived Bytes and String types so a compiler specialization carries
one coherent memory model.

## Platform and compiler

Platform APIs are effect operations.  Windows, POSIX and deterministic memory
platforms provide handlers.  Only the compiler driver performs platform
effects.

Compiler core is `Compiler(Memory) -> Module`.  Its context, source buffers,
diagnostics, strings and vectors are derived from the supplied MemoryModel.
No allocator value is passed through compiler functions.

LAIN-IR gains no heap, syscall, effect or capability instruction.  Effects and
Module specialization are eliminated during elaboration before physical
lowering.

## Compiler-owned type and L1 storage

`packages/lain/compiler/types.lain` exports one `types` Module.  A `TypeValue`
has a stable `TypeId`, nominal owner, type arguments and an associated
namespace.  A `ModuleValue` is also a compile-time value, so allocation and
bounds policy Modules can participate in deterministic specialization keys.

`packages/lain/compiler/l1_ir.lain` contains the dynamic L1 model:

```text
Unit
  types: Vec(Type)
  expressions: Vec(Expr)
  regions: Vec(Region)
  procedures: Vec(Procedure)

Region
  instructions: Vec(Instruction)

Procedure
  parameter_types: Vec(TypeId)
  body: RegionId
```

Each call owns its argument vector, each procedure owns its parameter vector,
and each region owns its instruction vector.  Correctness therefore does not
depend on a hidden "append these objects in exactly this order" rule.

Control flow remains structured: `if` and loops refer to nested regions.
LAIN-IR does not expose backend block labels or require a CFG.  LLVM or a
future native backend may construct a CFG after reading this structure.

`l1_unit_builder.lain` writes these values directly.  `l1_interpreter.lain`
reads the same values directly.  Neither file contains native handles or
`host_*` calls.  External procedures are ordinary calls resolved through an
explicit module supplied to the interpreter.

## Execution status

These files intentionally describe the desired language before the bootstrap
compiler can compile them.  ModuleValue specialization, associated type
namespaces, parameterized effects and handler row transformation must be
implemented by the self-hosted compiler next.

R1 through R4 now exist as pure-Lain source contracts:

```text
tokenizer
  -> lossless SyntaxStore
  -> generated syntax / Meta expansion
  -> ModuleValue workspace
  -> TypeValue and parameterized effect elaboration
  -> one lowerer writing l1_ir.Unit
  -> verifier / printer / interpreter
```

The first post-R4 convergence pass also fixes concrete representation
boundaries that a real `lainc` would have to type-check:

- source contents are borrowed `ByteSpan` values; only paths are cloned;
- compiler collections store `ModuleId`, `NamespaceId`, `TypeId` and
  `ProcedureId`, never compile-time `Module` values;
- parameterized effect arguments live once in an `Effects.Store`;
- builtin surface types are interned and mapped explicitly to physical L1
  types;
- normal compilation invokes Meta expansion before elaboration;
- surface calls lower to L1 calls whose targets are stable `ProcedureId`
  values, and the interpreter executes those procedures recursively;
- platform memory handlers clone owned paths and artifacts instead of
  shallow-copying borrowed containers.

The old `ast_*`, `generated_host_*`, `meta_host_*`, `M9*`, `M11*` and
handle-shaped L1 builder APIs are absent from active compiler source.  The
fixed-size L1 score mocks and the parallel `surface_forms`, `enums` and
`compiler_state` implementations were deleted.

This is not yet an executable compiler.  The current bootstrap compiler cannot
compile Module values, associated type namespaces, parameterized effects or
the final container APIs used here.  The next milestone is therefore not
another architectural rewrite: it is producing the first frozen L1 compiler
artifact capable of executing this source model, then recovering the
stage2/stage3 fixed point.
