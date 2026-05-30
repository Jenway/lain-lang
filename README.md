# Lain

Lain is a small native language experiment built on LLVM. The compiler keeps a
small core and pushes higher-level language features into a Scheme-backed
compile-time meta system.

Current highlights:

- LLVM IR, object, and executable output through the LLVM API backend
- same-directory imports, visibility, structs, enums, generics, impl methods,
  references, slices, raw pointers, arrays, `defer`, and `match`
- static effects, effect operations, v0 handlers, `Throws<E>`, `Suspend`, and
  `Spawn`
- standard-library macros for `@foreign`, `@derive(Clone)`, `@interface`,
  `@effect`, `format!`, `println!`, and the postfix `?` path
- a small standard library covering `Result`, `Option`, `DynArray`, `String`,
  IO, TCP, and replaceable executors

Useful commands:

```powershell
pixi run cargo run -- examples/tour.lain -o target/tour.ll
pixi run cargo run -- examples/tour.lain --emit exe -o target/tour.exe
pixi run cargo fmt --check
pixi run test
```

The default test suite intentionally leaves the TCP listener runtime smoke test
ignored, because opening a local listener can trigger Windows firewall/UAC
prompts. The compile-only TCP echo coverage remains part of the default suite.

See [docs/README.md](docs/README.md) for the current implementation notes.
