# SPDX-License-Identifier: GPL-2.0-only
"""Device tree (DT) handling: compile a .dts source into a .dtb"""

from __future__ import annotations

import logging
import subprocess
from pathlib import Path

logger = logging.getLogger(__name__)


def _compile_dts(src: Path, out: Path):
    """Run dtc to compile .dts `src` into the .dtb file at `out`"""
    try:
        p = subprocess.run(
            ["dtc", "-I", "dts", "-O", "dtb", "-o", str(out), str(src)],
            check=True,
            capture_output=True,
            text=True,
        )
        logger.debug("dtc %s: stdout: %s stderr: %s", src, p.stdout, p.stderr)

    except FileNotFoundError as e:
        raise RuntimeError("dtc not found in PATH; install device-tree-compiler") from e
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            f"dtc failed on {str(src)!r} (exit {e.returncode}):\n stdout: {e.stdout}\n stderr: {e.stderr}"
        ) from e


def compile_to_dtb(src: Path, out: Path) -> Path:
    """
    Compile a .dts source to a .dtb under out dir and return the .dtb path.

    A prebuilt .dtb passes through unchanged.

    Raises FileNotFoundError if `src` or `out` don't exist.
    """
    if not src.exists():
        raise FileNotFoundError(
            f"Device tree {str(src)!r} not found "
            "(expected a .dts source or prebuilt .dtb)"
        )

    if not out.exists():
        raise FileNotFoundError(f"Device Tree output dir: {out} doesn't exist")

    if src.suffix == ".dtb":  # already compiled, nothing to do
        return src

    dtb = out / (src.stem + ".dtb")
    _compile_dts(src, dtb)
    return dtb


def write_dts(text: str, name: str, out_dir: Path) -> Path:
    """Write generated .dts text to <out_dir>/<name>.dts; return its path."""
    src = out_dir / f"{name}.dts"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_text(text)
    return src
