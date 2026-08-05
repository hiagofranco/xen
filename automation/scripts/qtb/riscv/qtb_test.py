# SPDX-License-Identifier: GPL-2.0-only
"""Abstract base for the qtb riscv64 test types.

A test type is a RiscvQtbTest subclass owning a config file that describes its
tests, each bound to a machine from the shared catalog.
"""

from __future__ import annotations

import inspect
from abc import ABC, abstractmethod
from pathlib import Path
from typing import ClassVar

from config import MachineConfig
from riscv64_test_machine import RiscvTestMachine

# Default per-test timeout (seconds)
TIMEOUT_DEFAULT: int = 120


class RiscvQtbTest(ABC):
    """One runnable test bound to the machine it boots.

    A subclass sets `type_id`, `description`, and `config_file`, implements a
    parser per config format it supports (`_from_yaml`/`_from_json` for
    example), and implements `run` to drive the test logic. `from_config`
    dispatches to the right parser.
    """

    # Set by each concrete subclass.
    type_id: ClassVar[str] = ""
    # One-line summary of what the type does, shown in the CLI help.
    description: ClassVar[str] = ""
    # Default config file for the type; -c/--config overrides it.
    config_file: ClassVar[str] = ""

    # Set by the subclass parser.
    name: str
    data: dict
    machine: MachineConfig
    timeout: int = TIMEOUT_DEFAULT

    _registry: ClassVar[dict[str, type[RiscvQtbTest]]] = {}

    def __init_subclass__(cls, **kwargs) -> None:
        super().__init_subclass__(**kwargs)
        if not inspect.isabstract(cls) and cls.type_id:
            RiscvQtbTest._registry[cls.type_id] = cls

    @classmethod
    def known_types(cls) -> list[str]:
        """Every registered test type id, for CLI choices."""
        return sorted(cls._registry)

    @classmethod
    def for_type(cls, type_name: str) -> type[RiscvQtbTest]:
        """Return the subclass registered under `type_name`."""
        try:
            return cls._registry[type_name]
        except KeyError:
            known = ", ".join(sorted(cls._registry)) or "(none)"
            raise RuntimeError(f"unknown test type {type_name!r}; known types: {known}")

    @classmethod
    def from_config(cls, test_name: str, config_file: str) -> RiscvQtbTest:
        """Build the named test, dispatching on the config file's extension.

        A subclass overrides only the parsers it supports; an unimplemented
        format falls through to the base parser, which raises.
        """
        parsers = {
            ".yaml": cls._from_yaml,
            ".yml": cls._from_yaml,
            ".json": cls._from_json,
        }
        ext = Path(config_file).suffix.lower()
        parser = parsers.get(ext)
        if parser is None:
            raise ValueError(f"unsupported config format {ext!r} for {config_file!r}")
        return parser(config_file, test_name)

    @classmethod
    def _from_yaml(cls, config_file: str, test_name: str) -> RiscvQtbTest:
        raise NotImplementedError(f"{cls.__name__} has no YAML parser")

    @classmethod
    def _from_json(cls, config_file: str, test_name: str) -> RiscvQtbTest:
        raise NotImplementedError(f"{cls.__name__} has no JSON parser")

    @staticmethod
    @abstractmethod
    def list_tests(config_file: str) -> list[str]:
        """Return the names of every test declared in the config file."""

    @abstractmethod
    def run(self, vm: RiscvTestMachine) -> None:
        """Drive the already-launched machine and assert the expected result."""
