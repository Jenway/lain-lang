# Lain-on-LAINIR demo

`lain_meta_subset.l1` is a deliberately small Lain compiler written in
LAINIR.  Its parser only builds generic token, delimiter-group, and form
nodes.  The Meta layer implements `let`, `bits`, and `record`: it creates type
descriptors, binds them in a compile-time environment, and changes the
surface forms into Core nodes before the C emitter runs.

Build the bootstrap tools first, then from the repository root run:

```text
bootstrap/zig-out/bin/l1bootstrap \
  demo/lain_meta_subset.l1 demo_compile demo/hello_record.c \
  demo/hello_record.lain
```

The generated C contains a `Point` struct and a `main` returning `42`.  The
demo is intentionally small: its purpose is to prove the parser → Meta
expansion → lowering boundary, not to define the final Lain syntax.  The
important part is that the parser never branches on `record`; `meta_record`
reads its generic `{ ... }` group and constructs the type descriptor.
