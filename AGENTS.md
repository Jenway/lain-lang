# To Agents

READ THE FXXKING DOCS

## Workflow & Delegation
- Read the relevant specs in `docs/` and `PLAN.md` before changing language or compiler semantics, and keep docs synced with behavior.
- For multi-file compiler, stdlib, or test work, act primarily as coordinator, architect, reviewer, and verifier: partition work across disjoint file sets and delegate edits to parallel `pi-agent` workers (use the `pi-agent-orchestration` skill).

## Rolling Bootstrap Discipline (`Step A -> Step B`)
- Any change to `src/` syntax, keywords, or module visibility MUST split into two commits:
  1. **Step A**: Add compiler support while keeping `src/` compilable by the current `seed/bootstrap/lainc_boot.c`, verify fixed point, refresh `lainc_boot.c` via `.\.pi\py\.venv\Scripts\python.exe scripts/verify_all.py --update-bootstrap`, and commit.
  2. **Step B**: Migrate `src/`, `std/`, `examples/`, and `tests/meta/`, remove legacy compatibility, verify fixed point, refresh `lainc_boot.c`, and commit.

## Modern Lain & Compiler Invariants
- **Zero bare `addr`**: Use `Ref(T)`, `Mut(T)`, `Slice(T)`, `MutSlice(T)`, `ptr_cast`, `make_slice`, `null_ref`, `is_null`, `deref`.
- **Unified `let` syntax & no `given`**: Always use `let Name = struct/enum/func ...;` (with `pub let` for exports) and `sig` + `module { pub let Self = ...; }` + `use` / `?{ m: ... }`.
- **`@root` / `extern` cross-module bridges**: Never introduce circular imports in `src/`. When bridging via `@root let lainc_<target> = ...;`, the caller's `extern <local_name>(...) = lainc_<target>;` MUST use a `<local_name>` distinct from `lainc_<target>`, or `lainir-verify` will fail with `duplicate subroutine`.
- **Current frontend parser quirks**:
  - Do NOT chain `.field` directly onto a call result (`foo().bar`); bind `let x = foo();` first.
  - Parenthesize comparisons combined with `and`/`or`: `(a.x == b.x) and (a.y == b.y)`.
  - Avoid `is_null(p) == 0` where `bool` vs integer comparison is strict; prefer `if is_null(p)` or `(p as u64) != 0`.
- **ABI Sync**: Whenever modifying `src/ir_codes.lain`, update `scripts/gen_v2_abi.py` so `gen_v2_abi.py --check` stays green.
