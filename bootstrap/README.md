# Bootstrap

`bootstrap/` contains the LAINIR source and frozen artifact needed before the
Lain-written compiler can compile itself. It is a startup tool, not the source
of the long-term compiler architecture.

```text
seed/lainir-seed
  -> bootstrap/lainc.l1
  -> src/lainc/*.lain
```

`lainc.l1` and `lainc.l1.snapshot.json` are checked-in frozen artifacts. The
snapshot records the artifact hash and the formal Lain source closure that
produced it. `scripts/check_lainc_bootstrap_snapshot.py bootstrap/lainc.l1`
verifies both.

`compiler/` holds the remaining hand-written LAINIR bootstrap compiler
modules. `std/` holds its temporary LAINIR standard-library implementation.
They provide the bootstrap subset of RawAst, Meta, compiler ABI and lowering.

C0 removed the old evaluation result protocol and the bootstrap Meta evaluator
that depended on it. This source set is therefore intentionally incomplete
until LAINVM execution is reconnected. The frozen artifact remains checked in
as the temporary startup artifact during that migration.

The formal compiler lives in `src/lainc/`. Formal LAINIR definitions and
verification live in `src/lainir/`; LAINVM execution will move to
`src/lainvm/` during C3.
