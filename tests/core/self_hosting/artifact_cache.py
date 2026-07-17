"""Small, deterministic cache contract shared by bootstrap artifact builders."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Iterable


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def input_fingerprint(
    schema: str, inputs: Iterable[tuple[str, bytes]]
) -> str:
    """Hash a named, ordered build-input contract.

    Names are part of the digest so accidentally swapping two inputs cannot
    produce a cache hit.  Callers sort open-ended input sets before passing
    them; fixed ABI inputs remain in their declared order.
    """
    hasher = hashlib.sha256()
    hasher.update(schema.encode("utf-8"))
    hasher.update(b"\0")
    for name, content in inputs:
        hasher.update(name.encode("utf-8"))
        hasher.update(b"\0")
        hasher.update(content)
        hasher.update(b"\0")
    return hasher.hexdigest()


def cache_matches(
    output: Path, stamp_path: Path, schema: str, inputs: str
) -> bool:
    if not output.is_file() or not stamp_path.is_file():
        return False
    try:
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
        output_digest = sha256_file(output)
    except (OSError, ValueError, TypeError):
        return False
    return (
        stamp.get("schema") == schema
        and stamp.get("inputs") == inputs
        and stamp.get("output") == output_digest
    )


def write_stamp(
    output: Path, stamp_path: Path, schema: str, inputs: str
) -> None:
    stamp_path.write_text(
        json.dumps(
            {
                "schema": schema,
                "inputs": inputs,
                "output": sha256_file(output),
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
