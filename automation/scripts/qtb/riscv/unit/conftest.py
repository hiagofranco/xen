# SPDX-License-Identifier: GPL-2.0-only
"""Shared pytest fixtures for the qtb unit tests."""

from __future__ import annotations

import pytest

from config import MachineConfig


@pytest.fixture
def make_file(tmp_path):
    """Return a factory creating a file of `size` bytes, yielding its path."""

    def _make(name: str, size: int = 16) -> str:
        p = tmp_path / name
        p.write_bytes(b"\0" * size)
        return str(p)

    return _make


@pytest.fixture
def make_machine():
    """Return a factory building a MachineConfig for tests."""

    def _make(
        *,
        name="m",
        binaries=None,
        mmu="sv48",
        xen_bootargs="",
    ) -> MachineConfig:
        return MachineConfig(
            name=name,
            pcpu=4,
            memory=2048,
            binaries=binaries,
            interrupt_controller="aplic-imsic",
            aia_guests=7,
            mmu_type=mmu,
            xen_bootargs=xen_bootargs,
        )

    return _make
