# Lain AST operations

`src/lainir/lain/raw_ast.l1` now carries the first semantic views and
structural transforms over the topology-only RawAst tree, together with a
working macro pipeline.  Everything below runs through the seed bundle
(`source.l1` + `raw_ast.l1`) and is exercised by
`tests/lainir_lain/run_ast_ops.py`.

## Model

RawAst is deliberately topology-only: atoms (kind 1) and delimiter groups
(kind 2), with span + token kind, linked as parent/first/last/next.
Semantic interpretation is layered on top as predicates and transforms;
the parser never learns `let`, `record`, `type` or any other meaning.

```
text --raw_parse--> RawAst --views--> semantics
                        ^                |
                        |                v
                        +-- ast_write --+ transforms (replace/remove/copy)
```

## Views (predicates)

| proc | purpose |
|------|---------|
| `ast_span_equal(source, node, text, len)` | byte-compare a node's span with a literal |
| `ast_is_atom(source, node, text, len)` | node is an atom whose text equals `text` |
| `ast_is_postfix_group(node)` | `Name(...)` topology: atom followed by a paren group |
| `ast_is_call(node)` | the call view of the shared postfix shape |
| `ast_postfix_name(node)` / `ast_postfix_args(node)` | callee atom / arg group |

`Vec(i32)`, `foo(x)` and `Throws(i32)` all share the postfix topology
(see `docs/02-meta-system.md` §14); the caller decides the view.

## Traversal and transforms

| proc | purpose |
|------|---------|
| `ast_node_count(root)` | subtree node count, iterative explicit stack (no recursion) |
| `ast_find_atom(parent, text, len)` | first matching atom child |
| `ast_find_atom_deep(node, text, len)` | depth-first placeholder search |
| `ast_replace_child(parent, old, new)` | replace a child; the new node takes over the old's next link |
| `ast_remove_child(parent, child)` | splice a child out of the chain |
| `ast_copy(node)` | deep copy of a subtree |
| `ast_relocate(node, delta)` | shift every span by delta (rebase to parent source) |
| `ast_from_text(source, start, len)` | parse a source span into a fresh, independent AST |
| `ast_write(source, node)` | AST -> source text (atoms emit bytes, groups emit delimiters + children) |
| `ast_substitute(source, node, ph, len, arg)` | replace every placeholder atom in a subtree with a copy of `arg` |

`ast_from_text` + `ast_write` close the text-AST round trip; together
with `ast_copy`/`ast_replace_child`/`ast_substitute` they provide the
read-transform-write cycle macro expansion needs.

## Macro pipeline

Declarations have the shape `let NAME = macro(P) { TEMPLATE };`, parsed
as top-level siblings `let`, `NAME`, `=`, `macro`, `(P)`, `{TEMPLATE}`,
`;` (note: `macro(x)` is an atom `macro` plus a paren group, not a
single postfix node).

| proc | purpose |
|------|---------|
| `ast_find_macro_call(source, root, text, len)` | next top-level call site with the given name |
| `ast_find_macro_call_deep(source, node, text, len)` | depth-first call-site search |
| `ast_find_parent(root, node)` | parent group containing `node` |
| `ast_expand_macro_call(source, root, name, group, template, param)` | expand one call site in place |
| `ast_substitute_params(source, body, param_group, arg_group)` | pair each parameter with its argument (multi-param macros) |
| `ast_arg_expr_end(source, node)` | end offset of an argument expression up to the next comma |

Probes (each is a seed entry point):

- `lain_macro_expand_probe` — instantiate a template with an argument
  (`tmpl:(x+x) args:2 first_x:1 inst:(21+21)`)
- `lain_macro_decl_probe` — recognise `let NAME = macro(P) {T};` and
  expand one call (`name:twice param:x tmpl:(x+x) arg:21 inst:(21+21)`)
- `lain_macro_program_probe` — remove the declaration and rewrite the
  program with one call expanded (`prog:lety=(21+21);`)
- `lain_macro_multi_probe` — expand every call site
  (`prog:leta=(21+21);letb=(5+5);letc=(1+1); expanded:3`)
- `lain_macro_nested_probe` — expand macros whose templates contain other
  macro calls, until none remain
  (`prog:lety=((21+1)+(21+1)); expanded:3 residual:0`)
- `lain_ast_ops_probe` — the view/transform unit checks
- `lain_macro_multi_param_probe` — multi-parameter macros
  (`let add = macro(x, y) { (x + y) }; let z = add(3, 4);` →
  `prog:letz=(3+4);`; arguments may be whole expressions, e.g.
  `add(2 * 3, 4)` → `(2*3+4)`)
- `lain_macro_compile_probe` — expansion to compilable Lain: a macro
  with a bare-expression template (`macro(x) { x }`) expands
  `val(42)` to `lety=42;` (ast_write emits no whitespace); the test
  harness re-spaces it to `let y =42;`, appends a `main`, compiles the
  result with the reference lainc and runs it to 42 — macro output is
  real, compilable code, not just parseable text
- `lain_macro_recursion_probe` — self-referential macros are caught by
  a step bound: `let loop = macro(x) { (loop(x)) };` reports
  `expanded:8 recursive:1 residual:1` instead of looping forever
- `lain_macro_let_probe` — templates may declare local bindings:
  `let inc2 = macro(x) { let t = (x + 1); (t + t) };` expands
  `inc2(10)` to `{let t = (10+1); (t+t)}` — the placeholder is replaced
  inside the binding and the template's own `t` is kept (deep copy makes
  each instance independent)
- `lain_macro_capture_probe` — hygiene step 1: capture-collision detection.
  When the caller also binds the template's local name, expansion leaves
  both sets of atoms in the program:
  `let t = 99; let inc2 = macro(x) { let t = (x + 1); (t + t) }; let y = inc2(10);`
  reports `t_occurrences:4 capture:1` (the caller's `t`, the template's
  bound `t`, and its two uses).  `ast_count_atom` counts atoms by text
  span; `capture` is 1 when the name occurs more than once after
  expansion.  The follow-up is fresh-name generation (`ast_atom_from_text`
  buffers), which needs in-memory text buffers not yet built.

## Verification

`tests/lainir_lain/run_ast_ops.py` asserts every probe's exact output,
including a round trip: the expanded program text is re-parsed by
`lain_raw_ast_dump` and must parse cleanly (macro output is ordinary,
parseable Lain source).  Registered in `run_all.py`.

## LAIN-IR notes found while building this

- `#loop` bodies fall through and exit the loop unless the last statement
  is `#continue` — an expansion loop without `#continue` ran exactly once.
- Variable names `%arg<N>` are parsed as `#arg` parameter references
  (parser.c `parse_percent_ref`); `%arg1` in a parameterless proc is
  verify[2003].  Avoid `arg`+digits as local names.
- Address arithmetic uses `#lea` (`#add` on `#addr` is rejected as
  "expected bits for add").
