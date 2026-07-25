# Bootstrap boundary

`main` contains only the Lain-owned compiler sources and libraries.  The last
working C/Scheme recovery implementation is preserved on `bootstrap/stage0`
and in Git history; it is no longer copied into or built by `main`.

```text
D:\codeFiles\lain-lang       main
D:\codeFiles\lain-bootstrap  bootstrap/stage0
```

The replacement bootstrap must contain only:

```text
a LAIN-IR parser/interpreter
a frozen LAIN-IR compiler artifact
the minimum file, memory and byte-oriented platform operations
```

It must execute the frozen compiler against a selected `main` commit, produce
stage2 and stage3, and require their bytes to match.  It must not implement
Lain parsing, Meta, modules, types, syntax storage or LAIN-IR construction as
C host services.

The historical worktree can still be inspected with:

```text
git worktree add ..\lain-bootstrap bootstrap/stage0
```

It is evidence and a migration source, not a permitted dependency of `main`.
