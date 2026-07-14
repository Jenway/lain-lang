# Core Tests

Core tests protect compiler contracts. They are intentionally small.

Run:

```bash
python tests/core_runner.py
```

Current active suites:

- `ast`: RawAst topology golden tests
- `examples`: every supported user-facing example compiles to LAIN-IR
- `boundaries`: static layer-boundary lint
- `bootstrap-execution`: execute a Lain-written compiler component
- `lainir-contract`: parse, verify, and canonicalize LAIN-IR
- `self-hosting`: build and execute the Lain-written Meta artifact
- `scheme-host`: host/runtime compatibility contract
- `bootstrap-core`: minimal Lain capabilities needed by compiler code
