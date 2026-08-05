# SPDX-License-Identifier: GPL-2.0-only
"""QtbMachine for riscv64."""

from __future__ import annotations

from collections.abc import Sequence

from qemu.qtb import QtbMachine

from config import MachineConfig


class RiscvTestMachine(QtbMachine):
    arch_name = "riscv64"
    gdb_arch = "riscv:rv64"

    def __init__(
        self,
        mc: MachineConfig,
        *,
        timeout: int,
        log_dir: str | None = None,
    ) -> None:
        self.machine_conf = mc
        super().__init__(
            memory=mc.memory,
            cpus=mc.pcpu,
            mirror_console=False,
            timeout=timeout,
            log_dir=log_dir,
        )

    def _machine_args(self, memory: int, cpus: int) -> Sequence[str]:
        machine = self.machine_conf
        machine_opt = (
            f"virt,aclint=off,aia={machine.interrupt_controller},"
            f"aia-guests={machine.aia_guests}"
        )
        return [
            "-dtb",
            str(machine.dt.dtb),
            "-M",
            machine_opt,
            "-cpu",
            "rv64,svpbmt=on,smstateen=on,sstc=off",
            # QtbMachine strips the inherited "-accel qtest"; supply the
            # real (TCG) accelerator so the guest actually runs.
            "-accel",
            "tcg",
            "-smp",
            str(cpus),
            "-m",
            str(memory),
            "-bios",
            str(machine.binaries.firmware),
            "-kernel",
            str(machine.binaries.xen),
        ]

    def _resolve_binary(self) -> str:
        return str(self.machine_conf.binaries.qemu)
