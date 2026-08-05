# SPDX-License-Identifier: GPL-2.0-only
"""Path helpers: resolve project-relative paths."""

from __future__ import annotations

import os
import tempfile
from functools import lru_cache
from pathlib import Path

# Every relative path in this module is resolved against this base.
_BASE = Path(__file__).resolve().parent

# Build artifacts (qemu, firmware, xen) base, overridable for CI.
_BINARIES_BASE = Path(os.environ.get("QTB_BINARIES_DIR", "binaries"))


@lru_cache(maxsize=1)
def _temp_dir_handle() -> tempfile.TemporaryDirectory:
    return tempfile.TemporaryDirectory(prefix="qtb-")


def temp_dir() -> Path:
    """Process-wide scratch dir for generated/compiled artifacts (singleton)."""
    return Path(_temp_dir_handle().name)


def cleanup_temp_dir() -> None:
    """Remove the scratch dir and clear the cache."""
    _temp_dir_handle().cleanup()
    _temp_dir_handle.cache_clear()


def resolve_path(file_name: str) -> Path:
    """Resolve `file_name` against _BASE, absolute paths pass through."""
    return _resolve_under(file_name, _BASE)


def resolve_binary(file_name: str) -> Path:
    """Resolve a build artifact against _BINARIES_BASE, absolute paths pass through."""
    return _resolve_under(file_name, _BINARIES_BASE)


def _resolve_under(file_name: str, base: Path) -> Path:
    """Resolve `file_name` against `base`, absolute paths pass through."""
    p = Path(file_name)
    path = p if p.is_absolute() else base / p
    if not path.exists():
        raise FileNotFoundError(f"cannot resolve {str(path)!r}: does not exist")
    return path
