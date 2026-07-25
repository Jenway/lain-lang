# lain.compiler

This directory is the Lain implementation of the Lain compiler.

The previous C host implementations of RawAst, compiler storage, generated
syntax storage, structured LAIN-IR storage, and LAIN-IR execution were removed
from `main`.  They remain available only in Git history and the bootstrap
branch.

The compiler is temporarily incomplete.  The old C declarations have been
removed.  Their call sites now deliberately fail to resolve and identify the
Lain implementations that must be written:

```text
ast.*                 -> Lain tokenizer, reader, syntax arena
compiler.storage.*    -> Lain vectors, maps, strings, arenas
meta.syntax.*         -> Lain generated-syntax store
l1.*                  -> Lain LAIN-IR model, builder, verifier, interpreter
core.string.*         -> Lain string and number conversion library
```

Replacement order:

1. owned bytes, strings, vectors and arenas;
2. tokenizer, reader and syntax storage;
3. LAIN-IR data model, builder and text writer;
4. generated syntax and hygiene storage;
5. LAIN-IR verifier and interpreter;
6. compiler entry and module workspace;
7. restore bootstrap-to-stage2-to-stage3 fixed-point tests.

No new C host implementation may be added to `main`.  Platform access belongs
behind the bootstrap LAIN-IR interpreter or a future native runtime interface;
language parsing, compiler storage, Meta policy and LAIN-IR construction belong
in Lain.
