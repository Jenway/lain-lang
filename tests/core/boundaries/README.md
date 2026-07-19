# Boundary Tests

These tests protect compiler layer boundaries.

The former broad legacy runner has been removed. This suite starts as a
debt-aware lint: known violations are counted, and new violations fail.

Run:

```bash
python tests/core/boundaries/boundary_lint.py
```

Run strict mode to treat all known debt as failures:

```bash
python tests/core/boundaries/boundary_lint.py --strict
```

When a known violation is fixed, lower or remove its `allowed_count` in `boundary_lint.py`.
