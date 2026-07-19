# Canonical Declaration Contracts

Lain has one declaration shape:

```lain
let NAME: OPTIONAL_TYPE = RHS
```

`std::func`, `std::struct`, `std::module`, and `import` are Meta
constructors used as ordinary binding initializers. They are not parser
declaration kinds.

The contract runner invokes the installed self-hosted compiler through its
normal `--emit-l1` or `--emit-workspace-l1` path. Cases remain explicitly
pending until the compiler implements the complete contract; they are never
reported as passing merely because a fixture exists.

Required success contracts:

- an unannotated local value is inferred from its initializer;
- a function is bound through `let NAME = std::func(...)`;
- a type is bound through `let NAME: type = std::struct {...}`;
- a dependency is bound through `let NAME = import(...)`;
- a module is bound through `let NAME: Module = std::module {...}`.

Required diagnostics:

- `fn NAME(...)` is rejected with
  `error 12001: non-canonical top-level form`;
- `struct NAME {...}` is rejected with
  `error 12001: non-canonical top-level form`;
- standalone `import PATH` is rejected with
  `error 12001: non-canonical top-level form`;
- an unconstrained empty literal is rejected with
  `error 12007: unable to infer top-level binding type`.

Run:

```text
python tests/core/declarations/run_declaration_contract.py
python tests/core/declarations/run_declaration_contract.py --strict-pending
```
