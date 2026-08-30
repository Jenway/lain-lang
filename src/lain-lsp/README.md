# Lain LSP

This directory contains a language server implemented in Lain itself. The
native `lainir-lsp` host only supplies byte-oriented stdin/stdout and memory
allocation; JSON-RPC framing, request dispatch, document decoding, and the
syntax scan live in `lsp.lain`.

## Build and verify

```powershell
.\build\lainc.exe -o build\lain-lsp.l1 src\lain-lsp\lsp.lain
.\seed\zig-out\bin\lainir-print.exe build\lain-lsp.l1 main > $null
```

## Run

```powershell
.\seed\zig-out\bin\lainir-lsp.exe build\lain-lsp.l1 main
```

The server currently implements `initialize`, `shutdown`, `exit`,
`textDocument/didOpen`, `textDocument/didChange`,
`textDocument/formatting`, and `textDocument/semanticTokens/full`. Document
notifications run a delimiter/string syntax scan and publish diagnostics. The
current response serializer publishes an empty diagnostic array while escaped
string lowering in the Lain backend is being completed.

Run the protocol smoke test with:

```powershell
python tests\lainir_tools\lain_lsp_e2e.py
```

## Compiler gaps found by this implementation

- Division and remainder assignments are not preserved by the current Lain
  backend.
- Address expressions with multiplication are emitted as invalid L1.
- Escaped Lain string literals are emitted into L1 without safe encoding.
- Assigning to a function parameter can generate a mismatched L1 local name.

The implementation uses subtraction-based decimal formatting, explicit byte
slots, explicit protocol quote bytes, and local loop cursors so that its
generated artifact stays within the verified bootstrap subset.
