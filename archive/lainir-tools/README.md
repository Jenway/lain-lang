# LAIN-IR tools

`lsp.l1` is the first LAIN-IR implementation slice for the editor protocol.
The implementation owns the protocol framing and the in-memory data models;
the host only supplies a byte stream and page allocation:

```text
lsp.read-byte()  -> #bits<16>   // 0..255 byte, 256 = EOF
lsp.write-byte(#bits<8>)
lsp.flush()
lsp.allocate(#bits<64>) -> #addr
```

The module provides:

- `lsp_read_message`: `Content-Length`/CRLF framing and body buffering;
- document store operations for open/change/close state;
- linked diagnostic, formatting-edit, and semantic-token records;
- method classification and a minimal `initialize`/`shutdown`/formatting/
  semantic-token dispatch loop.

Input frames are bounded at 16 MiB. Truncated input, malformed headers, and
oversized lengths terminate the stream without emitting a partial response.

The result writer computes `Content-Length` in LAIN-IR. After bundling
`source.l1`, `parser.l1`, `highlight.l1`, and `lsp_tools.l1`, formatting returns
a canonical full-document `TextEdit`, semantic tokens return LSP delta tuples,
and parser errors are emitted as `publishDiagnostics`. Numeric request ids,
method, URI, text, and version fields are decoded from the JSON body and copied
into the document store; field order and ordinary whitespace do not matter.
Notifications (which have no numeric `id`) do not produce a response. The C
host remains limited to stream IO and allocation.

`didChange` accepts both full-sync changes and the usual ranged change object.
The document record retains old/new dirty byte ranges, LSP start/end positions,
an edit generation, and token-scan metadata. Ranged edits expand to token/trivia
boundaries, rescan that window, and splice the resulting token list into the
cached prefix/suffix. Full-document lexing remains the safe fallback when there
is no reusable token cache; the AST is rebuilt from the resulting snapshot.

The semantic-token legend advertised by `initialize` is fixed: comment,
string, number, directive, variable, operator, punctuation, invalid, keyword,
type, function, and property. Unknown directives remain distinct from known
keywords so editors can render syntax mistakes separately.

Validation:

```text
seed/zig-out/bin/lainir-print.exe src/lainir/tools/lsp.l1 lsp_run
```

End-to-end framing, fragmented input, multiple request ids, and response
lengths, feature responses, cache invalidation, and diagnostics are covered by:

```text
python tests/lainir_tools/run_tools.py
```

The standalone protocol client also runs the complete editor sequence (including
CRLF headers, fragmented writes, reordered JSON fields, ranged and full
`didChange`, formatting, semantic tokens, diagnostics, and shutdown):

```text
python tests/lainir_tools/lsp_client_e2e.py
```

## Lossless source and dumb AST

`source.l1` owns the shared lexer.  It emits token/trivia linked records and
keeps a one-byte zero sentinel for nullable links; callers must use
`tool_is_nil`, never compare addresses with `#eq`.

`parser.l1` consumes that stream and builds a deliberately dumb AST.  Nodes
carry `kind`, `start`, `length`, and linked `children`/siblings.  The current
kind set is module, proc, block, let, record, return, and statement; expression
and type syntax remains in statement ranges until a later pass.  The executable
entry `tool_parse_demo` serializes source[0] as nested JSON, which makes the
layout testable without a C parser.

`parser_ast_boundary` is the shared structural query used by the formatter. It
walks the AST and reports the opening byte or closing boundary of a `block`
node, so formatter indentation follows parser ranges instead of independently
guessing which braces are structural.

```text
python scripts/bundle_lainir.py -o parser_bundle.l1 \
  src/lainir/tools/source.l1 src/lainir/tools/parser.l1
seed/zig-out/bin/lainir-seed.exe parser_bundle.l1 \
  tool_parse_demo ast.json input.l1
```
