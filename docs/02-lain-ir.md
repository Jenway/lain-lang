# LAIN-IR language specification

Status: executable bootstrap subset.

This document specifies the LAIN-IR text accepted by the bootstrap parser,
verified by `l1check`, and executed by `l1i`.  Forms that are only ideas for a
future IR do not belong in this document.

## 1. Boundary

LAIN-IR is a physical, typed, structured and executable language.

It contains:

- physical scalar types;
- procedures and direct calls;
- immutable local bindings and mutable local storage names;
- address calculation, load, store and stack allocation;
- integer operations;
- structured conditionals and loops.

It does not contain:

- source-language modules or namespaces;
- source-language `let`, functions, structures or types;
- macros or Meta objects;
- generics, interfaces, effects or patterns;
- globals;
- CFG basic-block labels.

Those concepts may be implemented by an executable LAIN-IR program, but they
are not LAIN-IR forms.

## 2. Lexical structure

Whitespace separates tokens and is otherwise insignificant.

Line comments start with `//` and continue to the end of the line.

IR forms use `#`-prefixed names.  Local values use `%`-prefixed names.

## 3. Physical types

The canonical scalar types are:

```lain-ir
#bits<N>
#float<N>
#addr
#unit
#never
```

`N` is a non-zero decimal bit width.

`#bits<N>` has no signedness.  Signed or unsigned interpretation belongs to
the operation consuming the bits.

`#addr` is an untyped physical address.  It carries no pointee type.

The spellings `i32`, `i64`, `addr`, `f32` and similar source-language aliases
are not valid LAIN-IR types.

The in-memory IR model contains a SIMD type tag, but the executable text
language does not yet define a SIMD type grammar.  SIMD is therefore not part
of this specification.

## 4. Procedures

A procedure definition is:

```lain-ir
#proc add(
  #bits<32> %left,
  #bits<32> %right
) -> #bits<32> {
  #return #add(%left, %right)
}
```

An external procedure declaration is:

```lain-ir
#extern #proc source_data(
  #bits<64> %index
) -> #addr;
```

External procedures are resolved by the interpreter capability table.  They
do not introduce a source-language foreign-function system.

## 5. Local bindings

An immutable local binding is:

```lain-ir
#let %sum: #bits<32> = #add(%left, %right)
```

A mutable local storage name is updated with:

```lain-ir
%index: #bits<64> = #add(%index, 1)
```

The mutable form exists for the current structured-loop implementation.  It
is not source-language assignment.

## 6. Integer expressions

The executable subset contains:

```lain-ir
#add(left, right)
#sub(left, right)
#mul(left, right)
#sdiv(left, right)
#udiv(left, right)

#eq(left, right)
#ne(left, right)
#slt(left, right)
#sle(left, right)
#sgt(left, right)
#sge(left, right)
#ult(left, right)
#ule(left, right)
#ugt(left, right)
#uge(left, right)
```

Comparison expressions produce `#bits<1>`.

`#add`, `#sub` and `#mul` operate on fixed-width bit patterns and wrap modulo
`2^N`.  Division and ordering explicitly select signed or unsigned
interpretation.  The ambiguous spellings `#div`, `#lt`, `#le`, `#gt` and
`#ge` are invalid.

## 7. Calls

A direct call expression is:

```lain-ir
#call add(40, 2)
```

A call used only for its effect is an instruction:

```lain-ir
#call write_byte(%byte)
```

A procedure address is an untyped physical address:

```lain-ir
#let %target: #addr = #proc_addr(add)
```

An indirect call carries its complete physical signature at the call site:

```lain-ir
#let %sum: #bits<32> =
  #call_indirect[
    (#bits<32>, #bits<32>) -> #bits<32>
  ](%target, 40, 2)
```

The verifier checks the target is `#addr`, the arguments match the declared
signature, and the result type is explicit.  When the target expression is
itself `#proc_addr(name)`, the verifier additionally checks the declared
signature against that procedure.

`#proc_addr` and `#call_indirect` expose physical procedure references.  They
do not add source-language callable, Meta or Module semantics to LAIN-IR.

## 8. Memory

Stack/run allocation:

```lain-ir
#let %memory: #addr = #alloca(64)
```

Address calculation:

```lain-ir
#let %element: #addr = #lea(
  base=%memory,
  idx=%index,
  scale=16,
  offset=8
)
```

Typed load:

```lain-ir
#let %value: #bits<64> = #load[#bits<64>](%element)
#let %pointer: #addr = #load[#addr](%element)
```

Store:

```lain-ir
#store[#bits<64>] %value, %element
#store[#addr] %pointer, %pointer_slot
```

`#lea` always produces `#addr`.

The store type is mandatory.  Truncation or extension must be written as an
explicit conversion and never depends on interpreter value metadata.

Integer-width conversion is explicit:

```lain-ir
#zext[#bits<64>](%byte)
#sext[#bits<64>](%signed_byte)
#trunc[#bits<8>](%word)
```

`#zext` and `#sext` require a wider target. `#trunc` requires a narrower
target. Source and target must both be `#bits<N>`.

## 9. Structured control flow

Conditional:

```lain-ir
#if #eq(%value, 0) {
  #return 1
} else {
  #return 2
}
```

Loop:

```lain-ir
#let %index: #bits<64> = 0
#loop {
  #if #ge(%index, %length) {
    #break
  }

  %index: #bits<64> = #add(%index, 1)
  #continue
}
```

`#continue` begins the next loop iteration.  Falling through the loop body
ends the current loop in the bootstrap interpreter; code that intends an
iterating loop must therefore use `#continue`.

LAIN-IR does not expose CFG block labels.  A backend may construct private
basic blocks when lowering structured control flow.

## 10. Return

Value return:

```lain-ir
#return %value
```

Unit return:

```lain-ir
#return
```

The returned value must match the procedure result type.

## 11. Canonical formatting

`l1check` performs:

```text
parse
→ verify
→ print canonical LAIN-IR to stdout
```

Canonical output always uses:

```text
#bits<N>
#float<N>
#addr
#unit
#never
```

A dedicated `l1fmt` command may wrap the same parser and printer, but the
printer is already the single formatting authority.

## 12. Current executable example

`bootstrap/lainir/module_let.l1` is the first non-trivial executable example.
It represents Syntax nodes and Module bindings in ordinary memory and executes
a `meta_let` LAIN-IR procedure.  No Module or Meta instruction is added to
LAIN-IR.

The expected interpreter result is:

```text
0
```

## 13. Known missing physical capabilities

The following work remains before LAIN-IR can host the complete Lain
compiler:

- executable indirect calls and procedure-address values;
- explicit signed and unsigned integer operations;
- stable immutable data segments for frozen compiler data;
- float and bitcast conversion operations;
- precise allocation lifetime and address validity rules;
- deterministic traps with source locations.

These are physical execution capabilities.  They must not be replaced with
AST, Module, type-system or Meta-specific host callbacks.
