# Bootstrap

`bootstrap/` contains hand-maintained LAINIR source needed before the
Lain-written compiler can compile itself. It is a startup tool, not the source
of the long-term compiler architecture.

```text
seed/lainir-seed
  + bootstrap/compiler/*.l1
  + bootstrap/std/*.l1
  -> build/bootstrap/lainc.l1
  -> src/lainc/*.lain
```

Generated bundles and manifests belong under `build/bootstrap/` and are
ignored by Git. `scripts/freeze_lainc_bootstrap.py` builds `lainc.l1` and writes
the corresponding source and artifact hashes to `lainc.l1.snapshot.json`.

`compiler/` holds the remaining hand-written LAINIR bootstrap compiler
modules. `std/` holds its temporary LAINIR standard-library implementation.
They provide the bootstrap subset of RawAst, Meta, compiler ABI and lowering.

C0 removed the old evaluation result protocol and the bootstrap Meta evaluator
that depended on it. This source set is therefore intentionally incomplete
until LAINVM execution is reconnected.

The formal compiler lives in `src/lainc/`. Formal LAINIR definitions and
verification live in `src/lainir/`; LAINVM execution lives in `src/lainvm/`.
