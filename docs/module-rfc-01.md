# Module RFC 01

## Summary

Lain modules are compile-time objects in L2. They are not runtime values by default and do not exist in L1/LainIR except through the lowered symbols, globals, and link metadata needed by compiled code.

## Core Terms

- `module`: a meta-layer namespace object with exported bindings
- `signature`: the interface type for modules
- `interface`: a type-level dynamic dispatch protocol backed by `Dyn` and vtables
- `import(path)`: a meta constructor returning a module object
- `export`: module interface declaration

## Semantic Decisions

1. `module` is L2/meta-only and erased before L1 lowering.
2. `signature` constrains modules and separate compilation boundaries.
3. `interface` is not a synonym for `signature`; it stays reserved for type-level dynamic dispatch.
4. `let` is the unified binding syntax.
5. `import(path)` replaces special import-only semantics over time.
6. `export` is not a runtime effect; it forms the module interface.

## Relationship To Current System

The current implementation treats imports as a manifest-driven special form that registers extern functions directly. This RFC turns imports into module-producing meta operations and makes typed compiled interfaces the long-term boundary format.

## First Iteration Scope

- typed interface artifacts (`.lci`)
- module/signature object model in the meta layer
- migration path from `import foo::bar;`
- no runtime first-class modules
- no `open`
- no full functors yet

## Lowering Boundary

L1/LainIR should not model module semantics directly. The only required physical support remains:

- symbol linkage and visibility
- explicit `link_name`
- extern declarations
- global symbol definitions and address references

Everything else stays in the meta layer and in compiled interface artifacts.
