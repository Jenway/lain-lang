"""Canonical build-output locations for the Lain repository."""
from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
SEED_BIN = BUILD / "seed" / "bin"
SELFHOST_BIN = BUILD / "selfhost" / "bin"


def _with_suffix(path: Path, name: str) -> Path:
    return path / f"{name}.exe" if os.name == "nt" else path / name


def seed_exe(name: str) -> Path:
    """Locate a seed (Zig) binary installed under ``build/seed/bin``."""
    return _with_suffix(SEED_BIN, name)


def selfhost_exe(name: str) -> Path:
    """Locate a self-hosted compiler binary installed under ``build/selfhost/bin``."""
    return _with_suffix(SELFHOST_BIN, name)
