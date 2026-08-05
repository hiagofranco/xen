# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for xen_dt device-tree generation."""

from __future__ import annotations

from unittest import mock

import xen_dt


# ---- _render_xen_dts ----


def test_render_injects_bootargs(make_machine):
    machine = make_machine(xen_bootargs="com1=poll sched=null")
    out = xen_dt._render_xen_dts(machine)

    assert 'xen,xen-bootargs = "com1=poll sched=null";' in out


def test_render_injects_xen_mmu_type(make_machine):
    machine = make_machine(mmu="sv39")
    out = xen_dt._render_xen_dts(machine)

    assert 'mmu-type = "riscv,sv39";' in out


# ---- build_xen_device_tree ----


def test_build_xen_device_tree_compiles(tmp_path, make_machine):
    machine = make_machine(name="unit-test")
    dts = tmp_path / "unit-test.dts"
    dtb = tmp_path / "unit-test.dtb"
    with (
        mock.patch.object(xen_dt, "temp_dir", return_value=tmp_path),
        mock.patch.object(xen_dt, "write_dts", return_value=dts) as write_dts,
        mock.patch.object(xen_dt, "compile_to_dtb", return_value=dtb) as compile_to_dtb,
    ):
        result = xen_dt.build_xen_device_tree(machine)

    write_dts.assert_called_once()
    assert write_dts.call_args.args[1] == machine.name
    compile_to_dtb.assert_called_once_with(dts, tmp_path)
    assert result.dts == dts
    assert result.dtb == dtb
