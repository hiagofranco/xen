# SPDX-License-Identifier: GPL-2.0-only
"""Build the Xen host device tree for a MachineConfig.

The tree is rendered from its Jinja2 template (dts/qemu-host.dts.j2), which
takes the hart count, the Xen MMU type and the Xen command line, then compiled
to a DTB with dtc.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import TYPE_CHECKING

from jinja2 import Environment, FileSystemLoader

from paths import resolve_path, temp_dir
from dt import compile_to_dtb, write_dts

if TYPE_CHECKING:
    from config import MachineConfig

# Directory holding the Jinja2 platform device tree templates.
_DTS_DIR = "dts"


@dataclass(frozen=True)
class DeviceTree:
    """Compiled device trees for one machine launch."""

    dts: Path
    dtb: Path


@lru_cache(maxsize=1)
def _env() -> Environment:
    return Environment(
        loader=FileSystemLoader(resolve_path(_DTS_DIR)),
        keep_trailing_newline=True,
    )


def _render_xen_dts(machine: MachineConfig) -> str:
    """Render the Xen host device tree source text for `machine`."""
    tmpl = _env().get_template("qemu-host.dts.j2")
    return tmpl.render(
        ncpus=machine.pcpu,
        mmu_type=machine.mmu_type,
        xen_bootargs=machine.xen_bootargs,
        # Number of address bits needed to select one of aia_guests VS-mode
        # guest interrupt files (index 0 reserved, unusable).
        guest_index_bits=machine.aia_guests.bit_length(),
    )


def build_xen_device_tree(machine: MachineConfig) -> DeviceTree:
    """Compile `machine` device tree into the shared scratch dir."""
    out = temp_dir()
    dts: Path = write_dts(_render_xen_dts(machine), machine.name, out)
    dtb: Path = compile_to_dtb(dts, out)
    return DeviceTree(dts=dts, dtb=dtb)
