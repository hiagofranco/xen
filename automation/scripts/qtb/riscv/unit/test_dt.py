# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the device-tree compile path (dt.py)."""

from __future__ import annotations

import shutil
from pathlib import Path
from unittest import mock

import pytest

import dt
from dt import _compile_dts, compile_to_dtb, write_dts

_HAS_DTC = shutil.which("dtc") is not None
_MINIMAL_DTS = "/dts-v1/;\n/ { };\n"


# ---- _compile_dts (dtc wrapper) ----


def test_compile_dts_invokes_dtc(tmp_path):
    src = tmp_path / "in.dts"
    src.write_text(_MINIMAL_DTS)
    dtb = tmp_path / "in.dtb"

    with mock.patch.object(dt.subprocess, "run") as run:
        _compile_dts(src, dtb)

    run.assert_called_once()
    argv = run.call_args.args[0]
    assert argv == ["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb), str(src)]
    assert run.call_args.kwargs["check"] is True
    assert run.call_args.kwargs["capture_output"] is True


def test_compile_dts_missing_dtc_raises_runtimeerror(tmp_path):
    src, dtb = tmp_path / "a.dts", tmp_path / "a.dtb"
    src.write_text(_MINIMAL_DTS)

    with mock.patch.object(dt.subprocess, "run", side_effect=FileNotFoundError):
        with pytest.raises(RuntimeError, match="dtc not found"):
            _compile_dts(src, dtb)


# ---- compile_to_dtb path handling ----


def test_compile_to_dtb_passes_prebuilt_through_without_compiling(tmp_path):
    prebuilt = tmp_path / "tree.dtb"
    prebuilt.write_bytes(b"\xd0\x0d\xfe\xed")

    with mock.patch.object(dt, "_compile_dts") as compile_dts:
        result = compile_to_dtb(prebuilt, tmp_path)

    compile_dts.assert_not_called()
    assert result == prebuilt


def test_compile_to_dtb_missing_source_raises_filenotfound(tmp_path):
    with pytest.raises(FileNotFoundError, match="not found"):
        compile_to_dtb(tmp_path / "nope.dts", tmp_path)


def test_compile_to_dtb_source_goes_to_out_dir_with_dtb_suffix(tmp_path):
    src = tmp_path / "host-1smp.dts"
    src.write_text(_MINIMAL_DTS)
    out = tmp_path / "binaries"
    out.mkdir()

    with mock.patch.object(dt, "_compile_dts") as compile_mock:
        result = compile_to_dtb(src, out)

    assert result == out / "host-1smp.dtb"
    compile_mock.assert_called_once_with(src, out / "host-1smp.dtb")


# ---- write_dts ----


def test_write_dts_writes_source_under_out_dir(tmp_path):
    src = write_dts(_MINIMAL_DTS, name="unit", out_dir=tmp_path)

    assert src == tmp_path / "unit.dts"
    assert src.read_text() == _MINIMAL_DTS


@pytest.mark.skipif(not _HAS_DTC, reason="dtc not installed")
def test_write_then_compile_produces_dtb(tmp_path):
    src = write_dts(_MINIMAL_DTS, name="unit", out_dir=tmp_path)
    dtb = compile_to_dtb(src, tmp_path)

    assert dtb.is_file()
    assert dtb.suffix == ".dtb"
