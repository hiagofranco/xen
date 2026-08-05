# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for RiscvTestMachine QEMU argument assembly."""

from __future__ import annotations

import riscv64_test_machine as rtm  # noqa: E402
from config import BinariesConfig  # noqa: E402


def _binaries(tmp_path):
    paths = {}
    for name in ("qemu", "firmware", "xen"):
        p = tmp_path / name
        p.write_bytes(b"")
        paths[name] = str(p)
    return BinariesConfig(**paths)


def test_machine_args_wires_firmware_kernel_and_dtb(tmp_path, make_machine):
    mc = make_machine(name="machine", binaries=_binaries(tmp_path))

    # Bypass QtbMachine.__init__; _machine_args only reads self.machine_conf.
    vm = object.__new__(rtm.RiscvTestMachine)
    vm.machine_conf = mc

    args = list(vm._machine_args(memory=2048, cpus=4))

    assert args[args.index("-bios") + 1] == str(mc.binaries.firmware)
    assert args[args.index("-kernel") + 1] == str(mc.binaries.xen)
    assert args[args.index("-dtb") + 1] == str(mc.dt.dtb)
    assert args[args.index("-m") + 1] == "2048"
    assert args[args.index("-smp") + 1] == "4"
