# std/meta Source Layout

`*.scm` files are the canonical meta sources.

`*.lainm` files are generated mirrors used by the bootstrap/mini-eval path.

If you change a `*.scm` file, regenerate the matching `*.lainm` file with:

```sh
python3 scripts/convert_meta.py path/to/file.scm
```

Do not hand-edit both copies.
