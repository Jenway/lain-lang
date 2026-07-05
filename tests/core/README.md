# Core Tests

Core tests protect compiler contracts. They are intentionally small.

Run:

```bash
python tests/core_runner.py
```

Current active suites:

- `ast`: RawAst topology golden tests
- `boundaries`: static layer-boundary lint

Planned suites:

- `meta`
- `ir`
- `comptime`
- `effects`
- `imports`
- `runtime`
