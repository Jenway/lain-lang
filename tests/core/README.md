# Core Tests

Core tests protect compiler contracts. They are intentionally small.

Run:

```bash
python tests/core_runner.py
```

Current active suites:

- `ast`: RawAst topology golden tests
- `declarations`: canonical `let NAME[: TYPE] = RHS` contracts and removal
  of historical declaration spellings
- `examples`: every supported user-facing example compiles to LAIN-IR
- `boundaries`: static layer-boundary lint
- `bootstrap-execution`: execute a Lain-written compiler component
- `lainir-contract`: parse, verify, and canonicalize LAIN-IR
- `self-hosting`: build and execute the Lain-written Meta artifact
- Scheme-host compatibility tests live on `bootstrap/stage0`; `main` tests the
  Lain-owned compiler and consumes bootstrap tools from the sibling worktree.
- `bootstrap-core`: minimal Lain capabilities needed by compiler code
