"""Canonical source list for the unified Lain-written compiler."""

from __future__ import annotations

from pathlib import Path


MANIFEST = Path("src/lainc") / "parts" / "MANIFEST"


def compiler_source_dir(root: Path) -> Path:
    """Return the unified compiler module directory."""

    return root / "src" / "lainc" / "parts"


def compiler_sources(root: Path) -> tuple[Path, ...]:
    """Return compiler modules in the manifest's semantic order."""

    manifest = root / MANIFEST
    if not manifest.exists():
        raise FileNotFoundError(f"unified compiler manifest is missing: {manifest}")
    sources = []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path = compiler_source_dir(root) / (line if line.endswith(".lain") else f"{line}.lain")
        if path.suffix != ".lain" or not path.exists():
            raise FileNotFoundError(f"manifest source is missing: {path}")
        sources.append(path)
    return tuple(sources)


def compiler_source_names(root: Path) -> tuple[str, ...]:
    """Return active compiler source paths relative to the repository root."""

    return tuple(path.relative_to(root).as_posix() for path in compiler_sources(root))
