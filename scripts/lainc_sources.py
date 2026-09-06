"""Canonical source list for the unified Lain-written compiler."""

from __future__ import annotations

from pathlib import Path


MANIFEST = Path("src/lainc") / "COMPILER_SOURCES.txt"
LAINIR_API_MANIFEST = Path("src/lainir/api") / "SOURCES.txt"


def compiler_source_dir(root: Path) -> Path:
    """Return the unified compiler module directory."""

    return root / "src" / "lainc"


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
        path = root / line
        if not path.is_absolute():
            path = compiler_source_dir(root) / path
        if path.suffix != ".lain" or not path.exists():
            raise FileNotFoundError(f"manifest source is missing: {path}")
        sources.append(path)
    return tuple(sources)


def compiler_source_names(root: Path) -> tuple[str, ...]:
    """Return active compiler source paths relative to the repository root."""

    return tuple(path.relative_to(root).as_posix() for path in compiler_sources(root))


def lainir_api_sources(root: Path) -> tuple[Path, ...]:
    """Return the separately owned default LAINIR provider closure."""

    manifest = root / LAINIR_API_MANIFEST
    sources = []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path = root / line
        if path.suffix != ".lain" or not path.is_file():
            raise FileNotFoundError(f"LAINIR API source is missing: {path}")
        sources.append(path)
    return tuple(sources)


def composed_compiler_sources(root: Path) -> tuple[Path, ...]:
    """Return stdlib, selected provider, and compiler implementation."""

    return stdlib_sources(root) + lainir_api_sources(root) + compiler_sources(root)


def stdlib_sources(root: Path) -> tuple[Path, ...]:
    """Return the formal standard-library dependency closure."""

    return tuple(sorted((root / "std").rglob("*.lain")))
