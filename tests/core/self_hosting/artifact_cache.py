"""Small, deterministic cache contract shared by bootstrap artifact builders."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Iterable, Mapping


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


def specialization_key(schema: str, components: Mapping[str, object]) -> str:
    """Return a stable, serializable specialization key.

    Canonical JSON makes mapping order irrelevant while preserving the
    component names and scalar values that affect physical specialization.
    """
    payload = json.dumps(
        {"schema": schema, "components": dict(components)},
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return sha256_bytes(payload)


class SpecializationCache:
    """Small disk cache for verified specialization artifacts."""

    def __init__(self, root: Path, schema: str = "lain-specialization-v1") -> None:
        self.root = root
        self.schema = schema
        self.root.mkdir(parents=True, exist_ok=True)

    def _paths(self, key: str) -> tuple[Path, Path]:
        return self.root / f"{key}.l1", self.root / f"{key}.stamp.json"

    def get(self, key: str) -> bytes | None:
        output, stamp = self._paths(key)
        if not cache_matches(output, stamp, self.schema, key):
            return None
        try:
            return output.read_bytes()
        except OSError:
            return None

    def put(self, key: str, artifact: bytes) -> None:
        output, stamp = self._paths(key)
        with tempfile.NamedTemporaryFile(
            dir=self.root, prefix=f".{key}.", suffix=".tmp", delete=False
        ) as handle:
            temporary = Path(handle.name)
            handle.write(artifact)
        try:
            os.replace(temporary, output)
            write_stamp(output, stamp, self.schema, key)
        finally:
            temporary.unlink(missing_ok=True)
