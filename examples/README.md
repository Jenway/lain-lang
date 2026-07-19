# Examples

Every source file in this directory is a supported, compile-checked example.
`zig build test` runs the example smoke suite, so experimental or future syntax
must not be placed here.

- `tour.lain`: minimal executable returning `42`
- `structs.lain`: `std::struct` binding, construction, field access, and calls

All declarations use one surface form:

```lain
let name: optional_type = meta_constructor(...)
```

Functions use `std::func`, types use `std::struct`, and dependencies use a
binding initialized by `import(...)`. Historical `fn name`, `struct Name`, and
standalone `import path` declarations are intentionally not demonstrated or
accepted as alternate spellings.
