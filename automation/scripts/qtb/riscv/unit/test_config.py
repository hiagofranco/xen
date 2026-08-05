# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the YAML machine-catalog parser."""

from __future__ import annotations

from pathlib import Path

import pytest
import yaml

from config import (
    XEN_BOOTARGS_DEFAULT,
    XEN_MMU_TYPE_DEFAULT,
    MachineConfig,
    _parse_binaries,
)


@pytest.fixture
def binaries(make_file):
    """Raw binaries dict pointing at three existing files."""
    return {
        "qemu": make_file("qemu-system-riscv64"),
        "firmware": make_file("fw.bin"),
        "xen": make_file("xen"),
    }


# ---- _parse_binaries ----


def test_parse_binaries_resolves_existing_paths(binaries):
    cfg = _parse_binaries(binaries)
    assert cfg.qemu == Path(binaries["qemu"])
    assert cfg.firmware == Path(binaries["firmware"])
    assert cfg.xen == Path(binaries["xen"])


def test_parse_binaries_missing_key_raises(binaries):
    del binaries["xen"]
    with pytest.raises(ValueError, match="missing keys"):
        _parse_binaries(binaries)


def test_parse_binaries_missing_file_raises(binaries, tmp_path):
    binaries["xen"] = str(tmp_path / "absent")
    with pytest.raises(FileNotFoundError):
        _parse_binaries(binaries)


# ---- MachineConfig.from_config ----


def _write_yaml(tmp_path, binaries, name="machine-a", **machine_overrides):
    machine = {
        "pcpu": 1,
        "memory": 2048,
        "interrupt_controller": "aplic-imsic",
        "aia_guests": 7,
        "xen_bootargs": "com1=poll sched=null",
    }
    machine.update(machine_overrides)
    doc = {"binaries": binaries, "machines": {name: machine}}
    path = tmp_path / "config.yaml"
    path.write_text(yaml.safe_dump(doc))
    return str(path)


def test_from_config_builds_machineconfig(tmp_path, binaries):
    path = _write_yaml(tmp_path, binaries)

    mc = MachineConfig.from_config(path, "machine-a")

    assert mc.name == "machine-a"
    assert mc.pcpu == 1
    assert mc.memory == 2048
    assert mc.interrupt_controller == "aplic-imsic"
    assert mc.xen_bootargs == "com1=poll sched=null"
    assert mc.binaries.qemu == Path(binaries["qemu"])


def test_from_config_applies_optional_defaults(tmp_path, binaries):
    # A machine with only the required keys falls back to the module defaults.
    path = _write_yaml(
        tmp_path,
        binaries,
        name="bare",
        mmu_type=None,
        xen_bootargs=None,
    )
    # Drop the keys set to None so the parser sees them as absent.
    doc = yaml.safe_load(Path(path).read_text())
    for k in ("mmu_type", "xen_bootargs"):
        doc["machines"]["bare"].pop(k, None)
    Path(path).write_text(yaml.safe_dump(doc))

    mc = MachineConfig.from_config(path, "bare")

    assert mc.mmu_type == XEN_MMU_TYPE_DEFAULT
    assert mc.xen_bootargs == XEN_BOOTARGS_DEFAULT


def test_from_config_missing_interrupt_controller_raises(tmp_path, binaries):
    path = _write_yaml(tmp_path, binaries, name="bare")
    doc = yaml.safe_load(Path(path).read_text())
    del doc["machines"]["bare"]["interrupt_controller"]
    Path(path).write_text(yaml.safe_dump(doc))

    with pytest.raises(ValueError, match="interrupt_controller"):
        MachineConfig.from_config(path, "bare")


def test_from_config_unknown_interrupt_controller_raises(tmp_path, binaries):
    path = _write_yaml(tmp_path, binaries, interrupt_controller="bogus")
    with pytest.raises(ValueError, match="unknown interrupt_controller"):
        MachineConfig.from_config(path, "machine-a")


def test_from_config_unknown_machine_raises(tmp_path, binaries):
    path = _write_yaml(tmp_path, binaries)
    with pytest.raises(ValueError, match="unknown machine 'nope'"):
        MachineConfig.from_config(path, "nope")


def test_from_config_missing_binary_raises(tmp_path, binaries):
    binaries["qemu"] = str(tmp_path / "gone")
    path = _write_yaml(tmp_path, binaries)
    with pytest.raises(FileNotFoundError):
        MachineConfig.from_config(path, "machine-a")


def test_from_config_rejects_unknown_format(tmp_path):
    bogus = tmp_path / "config.toml"
    bogus.write_text("")
    with pytest.raises(ValueError, match="unsupported config format"):
        MachineConfig.from_config(str(bogus), "machine-a")
