# AST Core Tests

These tests lock the topology-only RawAst contract.

The runner compiles a tiny dump tool against `src/lainast` and compares `.lain` fixtures with `.ast` golden files.

Run:

```bash
python tests/core/ast/run_ast_golden.py
```

This suite tests RawAst directly. `canonicalize.scm` debt is tracked by the boundary lint until a Scheme AstTree dump hook exists.
