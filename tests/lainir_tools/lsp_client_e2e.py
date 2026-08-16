#!/usr/bin/env python3
"""Protocol-level LSP client regression for the LAIN-IR implementation.

This deliberately does not use an editor or an LSP client library.  It is a
small client at the JSON-RPC/stdio boundary, so the test exercises the exact
wire format that an editor uses: CRLF headers, fragmented writes, arbitrary
JSON field order, notifications, and both kinds of text document edits.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP_BIN = ROOT / "seed" / "zig-out" / "bin"
L1CHECK = BOOTSTRAP_BIN / ("lainir-print.exe" if sys.platform == "win32" else "lainir-print")
L1LS = BOOTSTRAP_BIN / ("lainir-lsp.exe" if sys.platform == "win32" else "lainir-lsp")
TOOLS = ROOT / "src" / "lainir" / "tools"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"


def _run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True)


def _frame(body: bytes) -> bytes:
    # Include a second, irrelevant header to ensure the LAIN-IR reader searches
    # for Content-Length rather than requiring a particular header order.
    return (
        b"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
        + b"Content-Length: "
        + str(len(body)).encode("ascii")
        + b"\r\n\r\n"
        + body
    )


def _json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def _read_frames(output: bytes) -> list[dict]:
    """Parse response frames and preserve wire-level framing invariants."""
    frames: list[dict] = []
    cursor = 0
    while cursor < len(output):
        separator = output.find(b"\r\n\r\n", cursor)
        if separator < 0:
            raise AssertionError("response is missing the CRLFCRLF separator")
        header = output[cursor:separator]
        if b"\n" in header.replace(b"\r\n", b""):
            raise AssertionError("response header used a bare LF")
        content_length = None
        for line in header.split(b"\r\n"):
            if line.startswith(b"Content-Length:"):
                value = line[len(b"Content-Length:") :].strip()
                content_length = int(value)
                break
        if content_length is None:
            raise AssertionError("response has no Content-Length header")
        start = separator + 4
        end = start + content_length
        if end > len(output):
            raise AssertionError("response body is truncated")
        frames.append(json.loads(output[start:end]))
        cursor = end
    return frames


def _bundle(path: pathlib.Path) -> None:
    result = _run(
        [
            sys.executable,
            str(BUNDLER),
            "-o",
            str(path),
            str(TOOLS / "source.l1"),
            str(TOOLS / "highlight.l1"),
            str(TOOLS / "parser.l1"),
            str(TOOLS / "lsp.l1"),
            str(TOOLS / "lsp_tools.l1"),
        ]
    )
    if result.returncode:
        raise AssertionError(f"LSP bundle failed:\n{result.stdout}\n{result.stderr}")
    checked = _run([str(L1CHECK), str(path), "lsp_run"])
    if checked.returncode:
        raise AssertionError(f"LSP bundle does not verify:\n{checked.stdout}\n{checked.stderr}")


def _assert_diagnostics(message: dict, uri: str, *, nonempty: bool) -> None:
    if message.get("jsonrpc") != "2.0" or message.get("method") != "textDocument/publishDiagnostics":
        raise AssertionError(f"not a diagnostics notification: {message!r}")
    if "id" in message:
        raise AssertionError("diagnostics notification unexpectedly has an id")
    params = message.get("params")
    if not isinstance(params, dict) or params.get("uri") != uri:
        raise AssertionError(f"diagnostics URI mismatch: {message!r}")
    diagnostics = params.get("diagnostics")
    if not isinstance(diagnostics, list) or (nonempty and not diagnostics):
        raise AssertionError(f"diagnostics shape mismatch: {message!r}")
    for diagnostic in diagnostics:
        if not isinstance(diagnostic, dict):
            raise AssertionError(f"malformed diagnostic: {diagnostic!r}")
        if not isinstance(diagnostic.get("severity"), int):
            raise AssertionError(f"diagnostic severity is not numeric: {diagnostic!r}")
        if not isinstance(diagnostic.get("message"), str):
            raise AssertionError(f"diagnostic message is not text: {diagnostic!r}")
        span = diagnostic.get("range")
        if not isinstance(span, dict):
            raise AssertionError(f"diagnostic has no range: {diagnostic!r}")
        for side in ("start", "end"):
            position = span.get(side)
            if not isinstance(position, dict) or not isinstance(position.get("line"), int) or not isinstance(position.get("character"), int):
                raise AssertionError(f"diagnostic range is malformed: {diagnostic!r}")


def _assert_semantic(message: dict, request_id: int) -> list[list[int]]:
    if message.get("jsonrpc") != "2.0" or message.get("id") != request_id:
        raise AssertionError(f"semantic response id mismatch: {message!r}")
    result = message.get("result")
    data = result.get("data") if isinstance(result, dict) else None
    if not isinstance(data, list) or any(not isinstance(token, list) or len(token) != 5 for token in data):
        raise AssertionError(f"semantic token result has invalid shape: {message!r}")
    for token in data:
        if any(not isinstance(value, int) or value < 0 for value in token):
            raise AssertionError(f"semantic token contains invalid value: {token!r}")
    return data


def _assert_formatting(message: dict, request_id: int, expected_text: str) -> None:
    if message.get("jsonrpc") != "2.0" or message.get("id") != request_id:
        raise AssertionError(f"formatting response id mismatch: {message!r}")
    edits = message.get("result")
    if not isinstance(edits, list) or len(edits) != 1:
        raise AssertionError(f"formatting result is not one TextEdit: {message!r}")
    edit = edits[0]
    if edit.get("newText") != expected_text:
        raise AssertionError(f"unexpected formatted text: {edit!r}")
    span = edit.get("range")
    if not isinstance(span, dict):
        raise AssertionError(f"formatting edit has no range: {edit!r}")
    start, end = span.get("start"), span.get("end")
    if start != {"line": 0, "character": 0} or not isinstance(end, dict):
        raise AssertionError(f"formatting range start is malformed: {edit!r}")
    if not isinstance(end.get("line"), int) or not isinstance(end.get("character"), int):
        raise AssertionError(f"formatting range end is malformed: {edit!r}")


def _run_boundary_case(
    bundle: pathlib.Path,
    *,
    uri: str,
    initial: str,
    changed: str,
    replacement: str,
    line: int,
    start: int,
    end: int,
    label: str,
) -> None:
    """Compare a bounded ranged scan with a full rebuild at a lexer boundary."""
    bodies = [
        _json({"jsonrpc": "2.0", "id": 101, "method": "initialize", "params": {}}),
        _json({
            "jsonrpc": "2.0", "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": uri, "version": 1, "text": initial}},
        }),
        _json({
            "jsonrpc": "2.0", "id": 102,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        }),
        _json({
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{
                    "range": {
                        "start": {"line": line, "character": start},
                        "end": {"line": line, "character": end},
                    },
                    "text": replacement,
                }],
            },
        }),
    ]
    bodies.extend([
        _json({
            "jsonrpc": "2.0", "id": 103,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        }),
        _json({
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 3},
                "contentChanges": [{"text": changed}],
            },
        }),
        _json({
            "jsonrpc": "2.0", "id": 104,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        }),
        _json({"jsonrpc": "2.0", "id": 105, "method": "shutdown", "params": None}),
    ])
    wire = b"".join(_frame(body) for body in bodies)
    process = subprocess.Popen(
        [str(L1LS), str(bundle)], cwd=ROOT, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        output, error = process.communicate(wire, timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise AssertionError(f"{label}: host timed out") from None
    if process.returncode:
        raise AssertionError(f"{label}: host failed: {error.decode(errors='replace')}")
    messages = _read_frames(output)
    replies = {message["id"]: message for message in messages if "id" in message}
    ranged = _assert_semantic(replies[103], 103)
    full = _assert_semantic(replies[104], 104)
    if ranged != full:
        raise AssertionError(f"{label}: bounded scan differs from full rebuild")


def main() -> int:
    if not L1CHECK.exists() or not L1LS.exists():
        print("missing bootstrap LSP executables", file=sys.stderr)
        return 1
    uri = "file:///client-e2e.l1"
    initial = "proc add(a, b) {\n  return a + b\n}\n"
    ranged_snapshot = "proc add(sum, b) {\n  return a + b\n}\n"
    changed = "proc broken(){\n"

    # The initialize body intentionally has CRLF whitespace and an unusual
    # field order.  The remaining requests similarly vary nested field order.
    initialize = (
        b"{\r\n  \"params\" : {},\r\n  \"id\" : 41,\r\n"
        b"  \"method\" : \"initialize\",\r\n  \"jsonrpc\" : \"2.0\"\r\n}"
    )
    bodies = [
        initialize,
        _json(
            {
                "params": {
                    "textDocument": {"text": initial, "version": 1, "uri": uri}
                },
                "method": "textDocument/didOpen",
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "id": 7,
                "params": {"textDocument": {"uri": uri}},
                "method": "textDocument/semanticTokens/full",
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/formatting",
                "params": {"textDocument": {"uri": uri}},
                "id": 8,
            }
        ),
        _json(
            {
                "params": {
                    "contentChanges": [
                        {
                            "text": "sum",
                            "range": {
                                "end": {"character": 10, "line": 0},
                                "start": {"character": 9, "line": 0},
                            },
                        }
                    ],
                    "textDocument": {"version": 2, "uri": uri},
                },
                "method": "textDocument/didChange",
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "id": 9,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 22},
                    "contentChanges": [{"text": ranged_snapshot}],
                },
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "id": 11,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": changed}],
                },
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "params": {"textDocument": {"uri": uri}},
                "id": 10,
                "method": "textDocument/semanticTokens/full",
                "jsonrpc": "2.0",
            }
        ),
        _json(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": uri}},
            }
        ),
        _json({"jsonrpc": "2.0", "params": None, "id": 99, "method": "shutdown"}),
    ]
    wire = b"".join(_frame(body) for body in bodies)

    with tempfile.TemporaryDirectory(prefix="lainir-lsp-client-") as temporary:
        bundle = pathlib.Path(temporary) / "lsp-client-bundle.l1"
        try:
            _bundle(bundle)
            process = subprocess.Popen(
                [str(L1LS), str(bundle)],
                cwd=ROOT,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            assert process.stdin is not None
            # Fragment both headers and bodies at uneven boundaries.  A real
            # editor often writes through a buffered stream in this fashion.
            sizes = (1, 2, 7, 3, 19, 5, 31)
            cursor = 0
            size_index = 0
            while cursor < len(wire):
                size = sizes[size_index % len(sizes)]
                process.stdin.write(wire[cursor : cursor + size])
                process.stdin.flush()
                cursor += size
                size_index += 1
            process.stdin.close()
            output = process.stdout.read() if process.stdout is not None else b""
            error = process.stderr.read() if process.stderr is not None else b""
            code = process.wait(timeout=20)
            if code:
                raise AssertionError(f"LSP process failed ({code}): {error.decode(errors='replace')}")
            messages = _read_frames(output)
            # Diagnostics are notifications and are emitted before feature
            # replies, so their count depends on which requests trigger a
            # cached-document preparation. Index replies by id instead of
            # assuming a fixed notification/reply alternation.
            replies = {message["id"]: message for message in messages if "id" in message}
            diagnostics = [
                message for message in messages
                if message.get("method") == "textDocument/publishDiagnostics"
            ]
            if sorted(replies) != [7, 8, 9, 10, 11, 41, 99] or not diagnostics:
                raise AssertionError(f"unexpected response/notification count: {messages!r}")
            positions = {
                request_id: next(
                    index for index, message in enumerate(messages)
                    if message.get("id") == request_id
                )
                for request_id in replies
            }
            if not (
                positions[41]
                < positions[7]
                < positions[8]
                < positions[9]
                < positions[11]
                < positions[10]
                < positions[99]
            ):
                raise AssertionError(f"responses arrived out of client order: {messages!r}")
            initialize_reply = replies[41]
            if initialize_reply.get("id") != 41:
                raise AssertionError(f"initialize id was not preserved: {initialize_reply!r}")
            capabilities = initialize_reply.get("result", {}).get("capabilities", {})
            if capabilities.get("documentFormattingProvider") is not True:
                raise AssertionError(f"formatting capability missing: {initialize_reply!r}")
            semantic_provider = capabilities.get("semanticTokensProvider", {})
            if semantic_provider.get("full") is not True:
                raise AssertionError(f"semantic capability missing: {initialize_reply!r}")
            legend = semantic_provider.get("legend", {}).get("tokenTypes")
            if legend != [
                "comment", "string", "number", "directive", "variable", "operator",
                "punctuation", "invalid", "keyword", "type", "function", "property",
            ]:
                raise AssertionError(f"semantic legend changed unexpectedly: {legend!r}")
            for diagnostic in diagnostics:
                _assert_diagnostics(diagnostic, uri, nonempty=False)
            if not any(message["params"]["diagnostics"] for message in diagnostics):
                raise AssertionError("broken didChange produced no diagnostics")
            initial_tokens = _assert_semantic(replies[7], 7)
            _assert_formatting(replies[8], 8, initial)
            ranged_tokens = _assert_semantic(replies[9], 9)
            full_tokens = _assert_semantic(replies[11], 11)
            if ranged_tokens != full_tokens:
                raise AssertionError("ranged token scan differs from equivalent full rebuild")
            if ranged_tokens == initial_tokens:
                raise AssertionError("ranged didChange did not invalidate semantic tokens")
            broken_tokens = _assert_semantic(replies[10], 10)
            if broken_tokens == ranged_tokens:
                raise AssertionError("full didChange did not invalidate semantic tokens")
            if replies[99].get("result") is not None:
                raise AssertionError(f"shutdown response shape changed: {replies[99]!r}")
            _run_boundary_case(
                bundle,
                uri="file:///string-boundary.l1",
                initial='proc main() {\n  return "abc" + 1\n}\n',
                changed='proc main() {\n  return "xyz" + 1\n}\n',
                replacement="xyz",
                line=1,
                start=10,
                end=13,
                label="string boundary",
            )
            _run_boundary_case(
                bundle,
                uri="file:///comment-boundary.l1",
                initial="proc main() {\n  /* old */\n  return 1\n}\n",
                changed="proc main() {\n  /* new */\n  return 1\n}\n",
                replacement="new",
                line=1,
                start=5,
                end=8,
                label="comment boundary",
            )
        except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
            print(f"FAIL LSP client end-to-end: {error}", file=sys.stderr)
            return 1
    print("PASS LSP client protocol end-to-end (initialize/open/tokens/format/change/diagnostics/shutdown)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
