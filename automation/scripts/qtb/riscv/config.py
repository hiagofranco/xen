# SPDX-License-Identifier: GPL-2.0-only
"""YAML config parser for qtb-based Xen riscv64 tests.

Parsing steps:
    - validate the YAML
    - build the dataclasses: the host device tree is compiled on
      MachineConfig construction
"""

from __future__ import annotations

import functools
import inspect
from dataclasses import dataclass
from pathlib import Path

import yaml

from paths import resolve_binary, resolve_path
from xen_dt import build_xen_device_tree

XEN_MMU_TYPE_DEFAULT: str = "sv48"
XEN_BOOTARGS_DEFAULT: str = ""
XEN_MEMORY_DEFAULT: int = 2048

INTERRUPT_CONTROLLERS: set = {"aplic-imsic"}


def required_keys(argument_name: str, keys: set, label: str = ""):
    """Validate the dict passed as `argument_name` has every key in `keys` before the wrapped parser runs."""
    keys = set(keys)

    def decorate(func):
        sig = inspect.signature(func)

        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            arg = sig.bind(*args, **kwargs).arguments[argument_name]
            missing = keys - arg.keys()
            if missing:
                raise ValueError(
                    f"{label or func.__name__} missing keys: {sorted(missing)}"
                )
            return func(*args, **kwargs)

        return wrapper

    return decorate


@dataclass
class BinariesConfig:
    qemu: Path
    firmware: Path
    xen: Path


@required_keys("raw", {"qemu", "firmware", "xen"}, label="binaries config")
def _parse_binaries(raw: dict) -> BinariesConfig:
    return BinariesConfig(
        qemu=resolve_binary(raw["qemu"]),
        firmware=resolve_binary(raw["firmware"]),
        xen=resolve_binary(raw["xen"]),
    )


@required_keys("raw", {"pcpu", "interrupt_controller"}, label="machine config")
def _parse_machine(
    raw: dict,
    binaries: BinariesConfig,
    machine: str,
) -> MachineConfig:
    interrupt_controller = raw["interrupt_controller"]
    if interrupt_controller not in INTERRUPT_CONTROLLERS:
        raise ValueError(
            f"machine {machine!r}: unknown interrupt_controller "
            f"{interrupt_controller!r}; valid: {sorted(INTERRUPT_CONTROLLERS)}"
        )

    if interrupt_controller == "aplic-imsic" and "aia_guests" not in raw:
        raise ValueError(
            f"machine {machine!r}: 'aia_guests' is required for "
            "interrupt_controller 'aplic-imsic'"
        )

    return MachineConfig(
        name=machine,
        pcpu=raw["pcpu"],
        binaries=binaries,
        interrupt_controller=interrupt_controller,
        aia_guests=raw["aia_guests"],
        memory=raw.get("memory", XEN_MEMORY_DEFAULT),
        mmu_type=raw.get("mmu_type", XEN_MMU_TYPE_DEFAULT),
        xen_bootargs=raw.get("xen_bootargs", XEN_BOOTARGS_DEFAULT),
    )


@dataclass(frozen=True)
class MachineConfig:
    """One named machine: the test-agnostic description of what to boot.

    A machine is reusable across test types; a test (see RiscvQtbTest
    subclasses) picks a machine by name and layers its own parameters on top.
    """

    name: str
    pcpu: int
    memory: int  # MB
    binaries: BinariesConfig
    interrupt_controller: str
    aia_guests: int  # VS-level guest interrupt files per hart IMSIC.
    mmu_type: str  # Xen (host) MMU type, injected into the host dts cpus.
    xen_bootargs: str

    @classmethod
    def from_config(cls, file_name: str, machine: str) -> MachineConfig:
        parsers = {
            ".yaml": cls._from_yaml,
            ".yml": cls._from_yaml,
            ".json": cls._from_json,
        }
        ext = Path(file_name).suffix.lower()
        parser = parsers.get(ext)
        if parser is None:
            raise ValueError(f"unsupported config format {ext!r} for {file_name!r}")

        return parser(file_name, machine)

    @classmethod
    def _from_json(cls, file_name: str, machine: str) -> MachineConfig:
        raise NotImplementedError(f"{cls.__name__} has no JSON parser")

    @classmethod
    def _from_yaml(cls, file_name: str, machine: str) -> MachineConfig:
        """Build only the single named machine from the catalog at `path`.

        A test run boots one machine, so there is no need to construct the
        whole catalog: parse the YAML, validate the shared binaries, and
        build just the requested entry.
        """
        fpath: Path = resolve_path(file_name)
        raw: dict = yaml.safe_load(fpath.read_text())

        required = ("binaries", "machines")
        missing = [k for k in required if k not in raw]
        if missing:
            raise ValueError(f"Global config {file_name} missing keys: {missing}")

        binaries: BinariesConfig = _parse_binaries(raw["binaries"])

        machines = raw["machines"]
        if machine not in machines:
            known = ", ".join(sorted(machines)) or "(none)"
            raise ValueError(f"unknown machine {machine!r}; known machines: {known}")

        return _parse_machine(machines[machine], binaries, machine)

    def __post_init__(self):
        super().__setattr__("dt", build_xen_device_tree(self))
