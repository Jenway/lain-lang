# Pure-Lain cutover

This commit deliberately removes the working C host from `main`.

Removed:

- C RawAst parser and arena;
- C compiler launcher and Scheme bridge;
- C compiler storage and generated-syntax storage;
- C LAIN-IR parser, builder, verifier, printer and interpreter;
- C executor and C-oriented Zig/Make build entry;
- standard-library wrappers that made libc or the old interpreter mandatory;
- all C host declarations from `packages/lain/compiler`.

The immediately preceding commit is the last verified state.  It passed the
complete core suite, stage2/stage3 byte equality, and cold bootstrap from
`bootstrap/stage0`.

The current commit is intentionally not buildable.  Calls that previously
resolved to C host facilities are now unresolved.  They are the ordered
rewrite queue:

1. bytes, owned strings, vectors and arenas;
2. tokenizer, reader and syntax nodes;
3. LAIN-IR values, instructions, procedures and units;
4. LAIN-IR text parser and writer;
5. generated syntax storage and hygiene;
6. verifier and interpreter;
7. compiler request/result storage and file entry;
8. new bootstrap interpreter and frozen LAIN-IR compiler.

Completion means that a bootstrap-branch LAIN-IR interpreter can run a frozen
compiler artifact against this branch, after which the produced compiler
rebuilds itself twice with identical stage2 and stage3 bytes.

No C parser, syntax arena, compiler container, Meta helper or LAIN-IR builder
may be added back to `main`.
