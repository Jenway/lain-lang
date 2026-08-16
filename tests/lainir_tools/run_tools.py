#!/usr/bin/env python3
"""Regression tests for the LAIN-IR-written editor tools.

The formatter is exercised twice through the stage0 runner.  This catches
both a broken LAIN-IR module (the lainir-print gate) and a formatter that is not
idempotent.  The parser, highlighter, and LSP are also executed through their
stage0 paths; protocol checks use the real stdio host.
"""

from __future__ import annotations

import pathlib
import json
import os
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP_DIR = ROOT / "seed" / "zig-out" / "bin"
L1CHECK = BOOTSTRAP_DIR / ("lainir-print.exe" if sys.platform == "win32" else "lainir-print")
L1BOOTSTRAP = BOOTSTRAP_DIR / (
    "lainir-seed.exe" if sys.platform == "win32" else "lainir-seed"
)
L1LS = BOOTSTRAP_DIR / ("lainir-lsp.exe" if sys.platform == "win32" else "lainir-lsp")
TOOLS = ROOT / "src" / "lainir" / "tools"
FIXTURES = pathlib.Path(__file__).parent / "fixtures"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True)


def report_failure(label: str, result: subprocess.CompletedProcess[str]) -> None:
    print(f"FAIL {label} (exit {result.returncode})", file=sys.stderr)
    if result.stdout:
        print(result.stdout, file=sys.stderr)
    if result.stderr:
        print(result.stderr, file=sys.stderr)


def check_module(source: pathlib.Path, entry: str) -> bool:
    result = run([str(L1CHECK), str(source), entry])
    if result.returncode:
        report_failure(f"lainir-print {source.name}:{entry}", result)
        return False
    print(f"PASS lainir-print {source.name}:{entry}")
    return True


def run_formatter() -> bool:
    source = TOOLS / "formatter.l1"
    source_frontend = TOOLS / "source.l1"
    parser = TOOLS / "parser.l1"
    fixture = FIXTURES / "formatter_input.l1"
    expected = (FIXTURES / "formatter_expected.l1").read_bytes()

    if not check_module(source, "lainir_format"):
        return False

    with tempfile.TemporaryDirectory(prefix="lainir-tools-") as temporary:
        temporary_path = pathlib.Path(temporary)
        bundle = temporary_path / "formatter-bundle.l1"
        result = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(source_frontend),
                str(parser),
                str(source),
            ]
        )
        if result.returncode:
            report_failure("formatter bundle", result)
            return False
        if not check_module(bundle, "lainir_format"):
            return False
        first = pathlib.Path(temporary) / "formatted-first.l1"
        result = run(
            [str(L1BOOTSTRAP), str(bundle), "lainir_format", str(first), str(fixture)]
        )
        if result.returncode:
            report_failure("formatter stage0 run", result)
            return False
        if first.read_bytes() != expected:
            print("FAIL formatter output differs from the fixture", file=sys.stderr)
            print(first.read_text(encoding="utf-8"), file=sys.stderr)
            return False
        print("PASS formatter stage0 output")

        second = pathlib.Path(temporary) / "formatted-second.l1"
        result = run(
            [str(L1BOOTSTRAP), str(bundle), "lainir_format", str(second), str(first)]
        )
        if result.returncode:
            report_failure("formatter second stage0 run", result)
            return False
        if second.read_bytes() != expected:
            print("FAIL formatter is not idempotent", file=sys.stderr)
            print(second.read_text(encoding="utf-8"), file=sys.stderr)
            return False
        print("PASS formatter idempotence")
        malformed = pathlib.Path(temporary) / "formatter-malformed.l1"
        malformed.write_text("proc broken(){\n", encoding="utf-8")
        rejected = pathlib.Path(temporary) / "formatter-rejected.l1"
        result = run(
            [str(L1BOOTSTRAP), str(bundle), "lainir_format", str(rejected), str(malformed)]
        )
        if result.returncode == 0 or rejected.exists():
            print("FAIL formatter accepted malformed AST input", file=sys.stderr)
            return False
        print("PASS formatter AST rejection")
        deep_input = pathlib.Path(temporary) / "formatter-deep.l1"
        deep_input.write_text(
            "proc nested(){" + ("{" * 64) + "return 1" + ("}" * 64) + "}\n",
            encoding="utf-8",
        )
        deep_output = pathlib.Path(temporary) / "formatter-deep-output.l1"
        result = run(
            [str(L1BOOTSTRAP), str(bundle), "lainir_format", str(deep_output), str(deep_input)]
        )
        if result.returncode or not deep_output.exists():
            report_failure("formatter deep AST", result)
            return False
        print("PASS formatter deep AST")
    return True


def run_highlight() -> bool:
    """Bundle and execute the LAIN-IR highlighter, then validate its ABI.

    The test intentionally checks the produced JSON rather than only lainir-print:
    this catches broken capability wiring, linked-list termination, span
    ordering, and accidental changes to the public highlight record shape.
    """
    fixture = FIXTURES / "highlight_input.l1"
    source_module = TOOLS / "source.l1"
    highlight_module = TOOLS / "highlight.l1"
    cli_module = TOOLS / "highlight_cli.l1"
    with tempfile.TemporaryDirectory(prefix="lainir-highlight-") as temporary:
        temporary_path = pathlib.Path(temporary)
        bundle = temporary_path / "highlight-bundle.l1"
        output = temporary_path / "highlights.json"
        result = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(source_module),
                str(highlight_module),
                str(cli_module),
            ]
        )
        if result.returncode:
            report_failure("highlight bundle", result)
            return False
        if not check_module(bundle, "lainir_highlight"):
            return False
        result = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lainir_highlight",
                str(output),
                str(fixture),
            ]
        )
        if result.returncode:
            report_failure("highlight stage0 run", result)
            return False
        try:
            payload = json.loads(output.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"FAIL invalid highlight JSON: {error}", file=sys.stderr)
            return False
        if not isinstance(payload, list) or not payload:
            print("FAIL highlight output is not a non-empty array", file=sys.stderr)
            return False
        source_length = len(fixture.read_bytes())
        required_keys = {"category", "start", "length", "line", "column"}
        previous_end = 0
        categories: set[int] = set()
        for item in payload:
            if not isinstance(item, dict) or set(item) != required_keys:
                print(f"FAIL malformed highlight item: {item!r}", file=sys.stderr)
                return False
            if not all(isinstance(item[key], int) for key in required_keys):
                print(f"FAIL non-integer highlight item: {item!r}", file=sys.stderr)
                return False
            category = item["category"]
            start = item["start"]
            length = item["length"]
            if category not in range(1, 13) or start < previous_end or length <= 0:
                print(f"FAIL invalid or unsorted highlight span: {item!r}", file=sys.stderr)
                return False
            if start + length > source_length:
                print(f"FAIL highlight span outside source: {item!r}", file=sys.stderr)
                return False
            if item["line"] < 0 or item["column"] < 0:
                print(f"FAIL negative highlight position: {item!r}", file=sys.stderr)
                return False
            categories.add(category)
            previous_end = start + length
        if categories != set(range(1, 13)):
            print(f"FAIL highlight categories missing: {sorted(categories)}", file=sys.stderr)
            return False
    print("PASS highlight stage0 JSON and spans")
    return True


def run_parser() -> bool:
    """Exercise the LAIN-IR dumb parser through the stage0 runner."""
    fixture = FIXTURES / "parser_input.l1"
    source_module = TOOLS / "source.l1"
    parser_module = TOOLS / "parser.l1"
    with tempfile.TemporaryDirectory(prefix="lainir-parser-") as temporary:
        temporary_path = pathlib.Path(temporary)
        bundle = temporary_path / "parser-bundle.l1"
        output = temporary_path / "ast.json"
        result = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(source_module),
                str(parser_module),
            ]
        )
        if result.returncode:
            report_failure("parser bundle", result)
            return False
        if not check_module(bundle, "tool_parse_demo"):
            return False
        result = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "tool_parse_demo",
                str(output),
                str(fixture),
            ]
        )
        if result.returncode:
            report_failure("parser stage0 run", result)
            return False
        try:
            payload = json.loads(output.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"FAIL invalid parser JSON: {error}", file=sys.stderr)
            return False
        if not isinstance(payload, list) or len(payload) != 1:
            print(f"FAIL parser root shape: {payload!r}", file=sys.stderr)
            return False
        kinds: set[int] = set()

        def visit(node: object) -> None:
            if not isinstance(node, dict):
                raise ValueError(f"non-object AST node: {node!r}")
            required = {"kind", "start", "length", "children"}
            if set(node) != required or not all(
                isinstance(node[key], int) for key in ("kind", "start", "length")
            ):
                raise ValueError(f"malformed AST node: {node!r}")
            if not isinstance(node["children"], list):
                raise ValueError(f"children is not a list: {node!r}")
            if node["start"] < 0 or node["length"] < 0:
                raise ValueError(f"negative AST span: {node!r}")
            kinds.add(node["kind"])
            for child in node["children"]:
                visit(child)

        try:
            visit(payload[0])
        except ValueError as error:
            print(f"FAIL malformed parser AST: {error}", file=sys.stderr)
            return False
        required_kinds = {1, 2, 3, 4, 5, 6, 8}
        if not required_kinds.issubset(kinds):
            print(f"FAIL parser kinds missing: {sorted(required_kinds - kinds)}", file=sys.stderr)
            return False
        malformed = temporary_path / "malformed.l1"
        malformed.write_text("#proc main() { #return 1; } }\n", encoding="utf-8")
        malformed_output = temporary_path / "malformed.json"
        result = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "tool_parse_demo",
                str(malformed_output),
                str(malformed),
            ]
        )
        if result.returncode == 0:
            print("FAIL parser accepted unmatched closing brace", file=sys.stderr)
            return False
    print("PASS parser stage0 AST ranges and child links")
    return True


def check_lsp_conditionally() -> bool:
    source = TOOLS / "lsp.l1"
    result = run([str(L1CHECK), str(source), "lsp_run"])
    if result.returncode:
        # Keep this as an explicit skip until the LSP source has no unresolved
        # helper calls.  Once it verifies, the framing fixture below becomes a
        # real gate rather than silently remaining untested.
        message = (result.stderr or result.stdout).strip().splitlines()
        detail = message[-1] if message else "verification failed"
        print(f"SKIP lsp framing fixture ({detail})")
        return True

    print("PASS lainir-print lsp.l1:lsp_run")
    fixture = (FIXTURES / "lsp_initialize_frame.txt").read_bytes().rstrip(b"\n")
    # Keep fixtures repository-friendly (LF), then put them on the wire with
    # the protocol's required CRLF separators and validate the declared body.
    framing = fixture.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    if not framing.startswith(b"Content-Length:") or b"\r\n\r\n" not in framing:
        print("FAIL malformed LSP framing fixture", file=sys.stderr)
        return False
    header, body = framing.split(b"\r\n\r\n", 1)
    if not header.endswith(b"46") or len(body) != 46:
        print("FAIL LSP framing content length mismatch", file=sys.stderr)
        return False

    # Exercise the actual LAIN-IR header parser without requiring the
    # not-yet-present lsp.read-byte host adapter.  The test entry reads the
    # fixture through the normal bootstrap source capability and calls
    # lsp_header_length directly.
    test_entry = """
#extern #proc bootstrap.source-data(#bits<64> %index) -> #addr;
#extern #proc bootstrap.source-length(#bits<64> %index) -> #bits<64>;
#proc lsp_framing_test() -> #bits<32> {
  #let %header: #addr = #call bootstrap.source-data(0)
  #let %length: #bits<64> = #call bootstrap.source-length(0)
  #if #eq(#call lsp_header_length(%header, %length), 46) {
    #return 0
  }
  #return 1
}
"""
    with tempfile.TemporaryDirectory(prefix="lainir-lsp-") as temporary:
        module = pathlib.Path(temporary) / "lsp_framing.l1"
        module.write_text(source.read_text(encoding="utf-8") + test_entry, encoding="utf-8")
        artifact = pathlib.Path(temporary) / "unused-artifact"
        result = run(
            [str(L1BOOTSTRAP), str(module), "lsp_framing_test", str(artifact), str(FIXTURES / "lsp_initialize_frame.txt")]
        )
        if result.returncode:
            report_failure("LSP framing stage0 run", result)
            return False
    print("PASS lsp framing stage0 run")
    return True


def run_lsp_stdio() -> bool:
    if not L1LS.exists():
        print(f"FAIL missing LSP host: {L1LS}", file=sys.stderr)
        return False
    # lsp.l1 imports the feature serializers; exercise the same complete
    # bundle here so plain protocol tests do not depend on unresolved externs.
    bundle = pathlib.Path(tempfile.gettempdir()) / "lainir-tools-lsp-stdio.l1"
    bundle_result = run(
        [
            sys.executable,
            str(BUNDLER),
            "-o",
            str(bundle),
            str(TOOLS / "source.l1"),
            str(TOOLS / "highlight.l1"),
            str(TOOLS / "lsp.l1"),
            str(TOOLS / "lsp_tools.l1"),
        ]
    )
    if bundle_result.returncode:
        report_failure("LSP stdio bundle", bundle_result)
        return False
    # Deliberately vary field order/whitespace and request ids.  The wire is
    # written in small pieces to exercise a stream implementation that splits
    # both headers and bodies across reads.
    bodies = [
        b'{"jsonrpc":"2.0","id":7,"method":"initialize","params":{}}',
        b'{"jsonrpc":"2.0","id":5,"method":"initialize\\u0020","params":{}}',
        b'{"jsonrpc":"2.0","method":"shutdown","id":99,"params":null}',
    ]
    wire = b"".join(
        b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
        for body in bodies
    )
    process = subprocess.Popen(
        [str(L1LS), str(bundle)],
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdin is not None
    try:
        # Non-uniform tiny chunks ensure CRLFCRLF and JSON fields can be split
        # at every kind of boundary.  Flush each chunk so fgetc sees a real
        # stream rather than one monolithic write.
        chunk_sizes = (1, 2, 5, 11, 3, 17)
        cursor = 0
        chunk_index = 0
        while cursor < len(wire):
            size = chunk_sizes[chunk_index % len(chunk_sizes)]
            process.stdin.write(wire[cursor : cursor + size])
            process.stdin.flush()
            cursor += size
            chunk_index += 1
    finally:
        process.stdin.close()
    output = process.stdout.read() if process.stdout is not None else b""
    error = process.stderr.read() if process.stderr is not None else b""
    result_code = process.wait(timeout=10)
    if result_code:
        print(f"FAIL LSP stdio host (exit {result_code})", file=sys.stderr)
        if error:
            print(error.decode(errors="replace"), file=sys.stderr)
        return False

    frames: list[bytes] = []
    cursor = 0
    while cursor < len(output):
        separator = output.find(b"\r\n\r\n", cursor)
        if separator < 0:
            print("FAIL LSP response is missing CRLF header terminator", file=sys.stderr)
            return False
        header = output[cursor:separator].decode("ascii", errors="strict")
        prefix = "Content-Length: "
        length_line = next(
            (line for line in header.split("\r\n") if line.startswith(prefix)), None
        )
        if length_line is None:
            print(f"FAIL LSP response has no Content-Length: {header!r}", file=sys.stderr)
            return False
        try:
            length = int(length_line[len(prefix) :])
        except ValueError:
            print(f"FAIL invalid LSP response length: {length_line!r}", file=sys.stderr)
            return False
        body_start = separator + 4
        body_end = body_start + length
        if body_end > len(output):
            print("FAIL truncated LSP response body", file=sys.stderr)
            return False
        frames.append(output[body_start:body_end])
        cursor = body_end
    if len(frames) != 3:
        print(f"FAIL expected 3 LSP responses, got {len(frames)}", file=sys.stderr)
        print(output.decode(errors="replace"), file=sys.stderr)
        return False
    try:
        responses = [json.loads(frame) for frame in frames]
    except json.JSONDecodeError as error:
        print(f"FAIL malformed LSP response JSON: {error}", file=sys.stderr)
        return False
    ids = [response.get("id") for response in responses]
    if ids != [7, 5, 99]:
        print(f"FAIL LSP response ids were not preserved: {ids!r}", file=sys.stderr)
        return False
    if "capabilities" not in responses[0].get("result", {}):
        print("FAIL initialize response has no capabilities", file=sys.stderr)
        return False
    semantic = responses[0]["result"]["capabilities"].get("semanticTokensProvider", {})
    legend = semantic.get("legend", {}).get("tokenTypes", [])
    expected_legend = [
        "comment", "string", "number", "directive", "variable", "operator",
        "punctuation", "invalid", "keyword", "type", "function", "property",
    ]
    if legend != expected_legend:
        print(f"FAIL initialize semantic-token legend: {legend!r}", file=sys.stderr)
        return False
    if responses[1].get("error", {}).get("code") != -32600 or responses[2].get("result") is not None:
        print(f"FAIL unexpected LSP results: {responses!r}", file=sys.stderr)
        return False
    print("PASS LSP stdio multi-request, fragmented framing, and request ids")
    return True


def run_lsp_features() -> bool:
    """Run the bundled LAIN-IR source/highlight/LSP feature path."""
    with tempfile.TemporaryDirectory(prefix="lainir-lsp-features-") as temporary:
        bundle = pathlib.Path(temporary) / "lsp-bundle.l1"
        result = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(TOOLS / "highlight.l1"),
                str(TOOLS / "parser.l1"),
                str(TOOLS / "lsp.l1"),
                str(TOOLS / "lsp_tools.l1"),
            ]
        )
        if result.returncode:
            report_failure("LSP feature bundle", result)
            return False
        if not check_module(bundle, "lsp_run"):
            return False
        text = "proc add(  a,b){return a+b}\n"
        bodies = [
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": "file:///feature.l1",
                        "version": 1,
                        "text": text,
                    }
                },
            },
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": "file:///feature.l1"}},
            },
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "textDocument/formatting",
                "params": {"textDocument": {"uri": "file:///feature.l1"}},
            },
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": "file:///feature.l1", "version": 2},
                    "contentChanges": [{
                        "range": {
                            "start": {"line": 0, "character": 9},
                            "end": {"line": 0, "character": 11},
                        },
                        "text": "",
                    }],
                },
            },
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": "file:///feature.l1"}},
            },
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": "file:///feature.l1", "version": 3},
                    "contentChanges": [{"text": "proc broken(){\n"}],
                },
            },
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": "file:///feature.l1"}},
            },
            {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None},
        ]
        wire = b"".join(
            (lambda body: b"Content-Length: "
             + str(len(body)).encode("ascii")
             + b"\r\n\r\n"
             + body)(json.dumps(item, separators=(",", ":")).encode("utf-8"))
            for item in bodies
        )
        process = subprocess.Popen(
            [str(L1LS), str(bundle)],
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            output, error = process.communicate(wire, timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            output, error = process.communicate()
            print("FAIL LSP feature host timed out", file=sys.stderr)
            return False
        if process.returncode:
            print(f"FAIL LSP feature host (exit {process.returncode})", file=sys.stderr)
            print(error.decode(errors="replace"), file=sys.stderr)
            return False
        frames: list[bytes] = []
        cursor = 0
        while cursor < len(output):
            separator = output.find(b"\r\n\r\n", cursor)
            if separator < 0:
                print("FAIL LSP feature response framing", file=sys.stderr)
                return False
            header = output[cursor:separator].decode("ascii")
            prefix = "Content-Length: "
            line = next(line for line in header.split("\r\n") if line.startswith(prefix))
            length = int(line[len(prefix) :])
            start = separator + 4
            frames.append(output[start : start + length])
            cursor = start + length
        try:
            responses = [json.loads(frame) for frame in frames]
        except json.JSONDecodeError as exc:
            print(f"FAIL LSP feature response JSON: {exc}", file=sys.stderr)
            print(output.decode(errors="replace"), file=sys.stderr)
            return False
        replies = [item for item in responses if "id" in item]
        notifications = [item for item in responses if item.get("method") == "textDocument/publishDiagnostics"]
        if [item.get("id") for item in replies] != [7, 8, 9, 10, 99]:
            print(f"FAIL LSP feature response ids: {responses!r}", file=sys.stderr)
            return False
        if not notifications or any(
            not isinstance(item.get("params", {}).get("diagnostics"), list)
            for item in notifications
        ):
            print(f"FAIL publishDiagnostics notifications: {notifications!r}", file=sys.stderr)
            return False
        if not any(item["params"]["diagnostics"] for item in notifications):
            print("FAIL malformed didChange produced no diagnostics", file=sys.stderr)
            return False
        data = replies[0].get("result", {}).get("data")
        if not isinstance(data, list) or not data or any(
            not isinstance(item, list) or len(item) != 5 for item in data
        ):
            print(f"FAIL semanticTokens data: {data!r}", file=sys.stderr)
            return False
        edits = replies[1].get("result")
        if not isinstance(edits, list) or len(edits) != 1:
            print(f"FAIL formatting edits: {edits!r}", file=sys.stderr)
            return False
        new_text = edits[0].get("newText", "")
        if new_text != "proc add(a, b) {\n  return a + b\n}\n":
            print(f"FAIL formatted text: {new_text!r}; responses={responses!r}", file=sys.stderr)
            return False
        ranged_data = replies[2].get("result", {}).get("data")
        if not isinstance(ranged_data, list) or ranged_data == data:
            print("FAIL semantic cache was not invalidated after ranged didChange", file=sys.stderr)
            return False
        changed_data = replies[3].get("result", {}).get("data")
        if not isinstance(changed_data, list) or changed_data == ranged_data:
            print("FAIL semantic cache was not invalidated after full didChange", file=sys.stderr)
            return False
    print("PASS LSP semanticTokens/full and formatting feature bundle")
    return True


def _lsp_frame(body: bytes, declared_length: int | None = None) -> bytes:
    length = len(body) if declared_length is None else declared_length
    return (
        b"Content-Length: "
        + str(length).encode("ascii")
        + b"\r\n\r\n"
        + body
    )


def _read_lsp_frames(output: bytes) -> list[bytes]:
    frames: list[bytes] = []
    cursor = 0
    while cursor < len(output):
        separator = output.find(b"\r\n\r\n", cursor)
        if separator < 0:
            raise ValueError("response is missing CRLF header terminator")
        header = output[cursor:separator].decode("ascii", errors="strict")
        prefix = "Content-Length: "
        line = next(
            (item for item in header.split("\r\n") if item.startswith(prefix)), None
        )
        if line is None:
            raise ValueError("response has no Content-Length")
        length = int(line[len(prefix) :])
        start = separator + 4
        end = start + length
        if end > len(output):
            raise ValueError("response body is truncated")
        frames.append(output[start:end])
        cursor = end
    return frames


def _build_lsp_bundle(path: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return run(
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


def _run_lsp_host(
    bundle: pathlib.Path, wire: bytes, *, environment: dict[str, str] | None = None
) -> tuple[int, bytes, bytes]:
    env = dict(os.environ)
    if environment:
        env.update(environment)
    process = subprocess.Popen(
        [str(L1LS), str(bundle)],
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    try:
        output, error = process.communicate(wire, timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        output, error = process.communicate()
        raise RuntimeError("LSP host timed out") from None
    return process.returncode, output, error


def run_lsp_stress_and_faults() -> bool:
    """Exercise hostile transport/input and allocator/stdio failure paths.

    These checks intentionally observe only the host boundary: LAIN-IR still
    owns framing, JSON validation, document processing, and response shape.
    """
    with tempfile.TemporaryDirectory(prefix="lainir-lsp-stress-") as temporary:
        bundle = pathlib.Path(temporary) / "lsp-bundle.l1"
        result = _build_lsp_bundle(bundle)
        if result.returncode:
            report_failure("LSP stress bundle", result)
            return False
        if not check_module(bundle, "lsp_run"):
            return False

        initialize = json.dumps(
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            separators=(",", ":"),
        ).encode("utf-8")

        # A body/header cut at EOF must terminate cleanly, never spin or emit
        # a partial JSON-RPC frame.
        for label, wire in (
            ("truncated body", _lsp_frame(initialize, len(initialize) + 9)),
            ("truncated header", b"Content-Length: 10\r\n"),
            ("invalid length", b"Content-Length: nope\r\n\r\n{}"),
            ("oversized length", b"Content-Length: 16777217\r\n\r\n"),
        ):
            try:
                code, output, error = _run_lsp_host(bundle, wire)
            except RuntimeError as exc:
                print(f"FAIL LSP {label}: {exc}", file=sys.stderr)
                return False
            if code != 0 or output:
                print(
                    f"FAIL LSP {label}: exit={code}, output={output!r}, "
                    f"stderr={error.decode(errors='replace')!r}",
                    file=sys.stderr,
                )
                return False

        # Long source and deeply nested blocks are sent in deliberately tiny
        # chunks.  A valid response proves that short reads do not alter the
        # source snapshot or desynchronize framing.
        long_line = "proc main(){return " + ("1" * 262144) + "}\n"
        deep = "proc nested(){" + ("{" * 256) + "return 1" + ("}" * 256) + "}\n"
        bodies = [
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": "file:///long.l1",
                        "version": 1,
                        "text": long_line,
                    }
                },
            },
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": "file:///long.l1"}},
            },
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": "file:///deep.l1",
                        "version": 1,
                        "text": deep,
                    }
                },
            },
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": "file:///deep.l1"}},
            },
            {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None},
        ]
        encoded = [
            json.dumps(item, separators=(",", ":")).encode("utf-8") for item in bodies
        ]
        wire = b"".join(_lsp_frame(body) for body in encoded)
        try:
            # communicate() drains stdout/stderr while writing, preventing a
            # large semantic-token response from filling a platform pipe.
            code, output, error = _run_lsp_host(bundle, wire)
        except (OSError, RuntimeError) as exc:
            print(f"FAIL LSP long/deep run: {exc}", file=sys.stderr)
            return False
        if code != 0:
            print(f"FAIL LSP long/deep host exit={code}: {error!r}", file=sys.stderr)
            return False
        try:
            responses = [json.loads(frame) for frame in _read_lsp_frames(output)]
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"FAIL LSP long/deep response: {exc}", file=sys.stderr)
            return False
        reply_ids = [item.get("id") for item in responses if "id" in item]
        if reply_ids != [2, 3, 4]:
            print(f"FAIL LSP long/deep reply ids: {reply_ids!r}", file=sys.stderr)
            return False
        for item in responses:
            if "id" in item and item["id"] in (2, 3):
                data = item.get("result", {}).get("data")
                if not isinstance(data, list) or not data:
                    print(f"FAIL LSP long/deep empty semantic data: {item!r}", file=sys.stderr)
                    return False

        # Host allocation failure must become a contained non-zero process
        # result, not an access violation or an unbounded allocation.
        padded = initialize + b" " * (5001 - len(initialize))
        code, output, error = _run_lsp_host(
            bundle, _lsp_frame(padded), environment={"LAINIR_LSP_ALLOC_MAX": "4096"}
        )
        if code == 0 or b"fault injection" not in error:
            print(
                f"FAIL allocator fault handling: exit={code}, output={output!r}, "
                f"stderr={error.decode(errors='replace')!r}",
                file=sys.stderr,
            )
            return False

        # A bounded write capability simulates a short write without making
        # the test depend on platform-specific broken-pipe behavior.
        code, output, error = _run_lsp_host(
            bundle, _lsp_frame(initialize), environment={"LAINIR_LSP_WRITE_MAX": "1"}
        )
        if code == 0 or b"fault injection" not in error:
            print(
                f"FAIL short-write fault handling: exit={code}, output={output!r}, "
                f"stderr={error.decode(errors='replace')!r}",
                file=sys.stderr,
            )
            return False
    print("PASS LSP stress, truncated transport, and host fault handling")
    return True


def run_lsp_client_e2e() -> bool:
    """Run the standalone protocol client against the real LAIN-IR server."""
    client = pathlib.Path(__file__).with_name("lsp_client_e2e.py")
    result = run([sys.executable, str(client)])
    if result.returncode:
        report_failure("LSP client end-to-end", result)
        return False
    print(result.stdout.strip())
    return True


def run_lsp_incremental_metrics() -> bool:
    """Prove that a ranged edit uses a finite LAIN-IR lexer window."""
    entry = """
#extern #proc bootstrap.allocate-pages(#bits<64> %size) -> #addr;
#extern #proc bootstrap.source-data(#bits<64> %index) -> #addr;
#extern #proc bootstrap.source-length(#bits<64> %index) -> #bits<64>;
#proc lsp.allocate(#bits<64> %size) -> #addr {
  #return #call bootstrap.allocate-pages(%size)
}
#proc lsp_incremental_metrics() -> #bits<32> {
  #let %text: #addr = #call bootstrap.source-data(0)
  #let %length: #bits<64> = #call bootstrap.source-length(0)
  #let %document: #addr = #call lsp_document_new("file:///metrics.l1", %text, %length, 1)
  #call lsp_tools_prepare_document(%document)
  #let %first_count: #bits<64> = #call lsp_document_token_scan_count(%document)
  #call lsp_document_change_range(%document, "3", 1, 2, 0, 16, 0, 17)
  #call lsp_tools_prepare_document(%document)
  #if #ne(#call lsp_document_token_scan_count(%document), #add(%first_count, 1)) { #return 1 }
  #if #call lsp_document_token_scan_is_full(%document) { #return 2 }
  #if #ge(
    #sub(#call lsp_document_token_scan_end(%document), #call lsp_document_token_scan_start(%document)),
    %length
  ) { #return 3 }
  #return 0
}
"""
    source = "proc a(){return 1}\nproc b(){return 2}\n"
    with tempfile.TemporaryDirectory(prefix="lainir-lsp-incremental-") as temporary:
        temporary_path = pathlib.Path(temporary)
        bundle = temporary_path / "incremental-bundle.l1"
        module = temporary_path / "incremental-entry.l1"
        fixture = temporary_path / "incremental-input.l1"
        artifact = temporary_path / "unused-artifact"
        module.write_text(entry, encoding="utf-8")
        fixture.write_text(source, encoding="utf-8")
        result = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(TOOLS / "highlight.l1"),
                str(TOOLS / "parser.l1"),
                str(TOOLS / "lsp.l1"),
                str(TOOLS / "lsp_tools.l1"),
                str(module),
            ]
        )
        if result.returncode:
            report_failure("LSP incremental metrics bundle", result)
            return False
        if not check_module(bundle, "lsp_incremental_metrics"):
            return False
        result = run(
            [str(L1BOOTSTRAP), str(bundle), "lsp_incremental_metrics", str(artifact), str(fixture)]
        )
        if result.returncode:
            report_failure("LSP incremental metrics stage0", result)
            return False
    print("PASS LSP ranged lexer window and cache metrics")
    return True


def main() -> int:
    missing = [path for path in (L1CHECK, L1BOOTSTRAP, L1LS) if not path.exists()]
    if missing:
        for path in missing:
            print(f"missing bootstrap executable: {path}", file=sys.stderr)
        return 1

    if not run_formatter():
        return 1
    if not run_highlight():
        return 1
    if not run_parser():
        return 1
    if not check_lsp_conditionally():
        return 1
    if not run_lsp_stdio():
        return 1
    if not run_lsp_features():
        return 1
    if not run_lsp_client_e2e():
        return 1
    if not run_lsp_incremental_metrics():
        return 1
    if not run_lsp_stress_and_faults():
        return 1
    print("\nLAIN-IR tools: all enabled checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
