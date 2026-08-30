import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "seed" / "zig-out" / "bin" / "lainir-lsp.exe"
ARTIFACT = ROOT / "build" / "lain-lsp.l1"


def frame(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def parse_frames(data: bytes) -> list[dict]:
    messages = []
    offset = 0
    while offset < len(data):
        header_end = data.find(b"\r\n\r\n", offset)
        if header_end < 0:
            raise AssertionError(f"incomplete header at {offset}: {data[offset:]!r}")
        header = data[offset:header_end].decode()
        length = int(header.split(":", 1)[1].strip())
        body_start = header_end + 4
        body_end = body_start + length
        body = data[body_start:body_end]
        if len(body) != length:
            raise AssertionError(f"short body: wanted {length}, got {len(body)}")
        messages.append(json.loads(body))
        offset = body_end
    return messages


def main() -> int:
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": "file:///smoke.lain",
                    "languageId": "lain",
                    "version": 1,
                    "text": "let main = std::func() -> i32 { return 0; };",
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "textDocument/formatting",
            "params": {"textDocument": {"uri": "file:///smoke.lain"}},
        },
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": "file:///smoke.lain"}},
        },
        {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    proc = subprocess.run(
        [str(HOST), str(ARTIFACT), "main"],
        input=b"".join(frame(item) for item in requests),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"server exited {proc.returncode}\nstderr:\n{proc.stderr.decode(errors='replace')}"
        )
    messages = parse_frames(proc.stdout)
    by_id = {message.get("id"): message for message in messages if "id" in message}
    assert by_id[1]["result"]["serverInfo"]["name"] == "lain-lsp"
    assert by_id[2]["result"] == []
    assert by_id[3]["result"] == {"data": []}
    assert by_id[4]["result"] is None
    diagnostics = [
        message
        for message in messages
        if message.get("method") == "textDocument/publishDiagnostics"
    ]
    assert diagnostics[0]["params"]["uri"] == "file:///smoke.lain"
    print(f"lain-lsp e2e: ok ({len(messages)} messages)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
