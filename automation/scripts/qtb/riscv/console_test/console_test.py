# SPDX-License-Identifier: GPL-2.0-only
"""Console string expectation test (`type: console-test`).

Per console: assert each expected string is printed within the timeout.
Nothing is injected; this only watches console output.

YAML (console-test.yaml): each test names the machine it boots (tests may
share one) and maps a console index to the string(s) expected on it: 0 is
Xen's own console.

    machine_catalog: config.yaml
    tests:
      dom0less-1smp-0domu-1vcpu-aplic-imsic-null:  # test name (run positional)
        machine: dom0less-1smp-0domu-1vcpu-aplic-imsic-null  # from config.yaml
        expect:                                    # console index -> string(s)
          0: [All set up]

JSON (console-test.json): not supported yet
"""

from __future__ import annotations

from typing import ClassVar

import pexpect
import yaml

from paths import resolve_path
from config import MachineConfig, required_keys
from qtb_test import TIMEOUT_DEFAULT, RiscvQtbTest
from riscv64_test_machine import RiscvTestMachine

TYPE_ID: str = "console-test"

CONFIG_FILE_DEFAULT: str = "console_test/console-test.yaml"
DESCRIPTION_DEFAULT: str = "Assert expected string(s) are printed on the Xen console"
ATTEMPTS_DEFAULT: int = 3


class ConsoleTest(RiscvQtbTest):
    type_id: ClassVar[str] = TYPE_ID
    description: ClassVar[str] = DESCRIPTION_DEFAULT
    config_file: ClassVar[str] = CONFIG_FILE_DEFAULT

    def __init__(self, raw: dict, test_name: str) -> None:
        self.name, self.data, self.machine = self._parse_test_cfg(raw, test_name)

        self.expect = self.data["expect"]  # expected console string

        self.timeout = int(raw.get("timeout", TIMEOUT_DEFAULT))
        if self.timeout < 1:
            raise ValueError("timeout < 1, must be at least 1")

        self.attempts = int(raw.get("attempts", ATTEMPTS_DEFAULT))
        if self.attempts < 1:
            raise ValueError("attempts < 1, must be at least 1")

    @staticmethod
    def _load_yaml(path) -> dict:
        return yaml.safe_load(resolve_path(path).read_text())

    @classmethod
    def _from_yaml(cls, config_file: str, test_name: str) -> ConsoleTest:
        return cls(cls._load_yaml(config_file), test_name)

    @staticmethod
    @required_keys("test_data", {"machine", "expect"})
    def _parse_test_data(
        machine_catalog: str, test_data: dict, test_name: str
    ) -> tuple[str, dict, MachineConfig]:
        """Parse test data dictonnary"""
        test_machine = MachineConfig.from_config(machine_catalog, test_data["machine"])
        for cons_idx, strings in test_data["expect"].items():
            if cons_idx != 0:
                raise ValueError(
                    f"test {test_name!r}: console index {cons_idx} out of range "
                    "(0 is Xen's own console, the only one)"
                )
            if not isinstance(strings, list):
                raise ValueError(
                    f"test {test_name!r}: Xen expects a list of "
                    f"string(s), got {type(strings).__name__}"
                )
            if not strings:
                raise ValueError(f"test {test_name!r}: Xen has no expected string")

            if any(not s for s in strings):
                raise ValueError(
                    f"test {test_name!r}: Xen has an empty expected string"
                )
        return (test_name, test_data, test_machine)

    @staticmethod
    @required_keys("raw", {"machine_catalog", "tests"})
    def _parse_test_cfg(raw: dict, test_name: str) -> tuple[str, dict, MachineConfig]:
        """Read the config: return the named test dict and its machine."""

        tests = raw["tests"]
        if test_name not in tests:
            known = ", ".join(sorted(tests)) or "(none)"
            raise ValueError(f"unknown test {test_name!r}; known tests: {known}")

        test_data = tests[test_name]
        return ConsoleTest._parse_test_data(
            raw["machine_catalog"], test_data, test_name
        )

    @staticmethod
    def list_tests(config_file: str) -> list[str]:
        return list(ConsoleTest._load_yaml(config_file).get("tests", {}))

    @staticmethod
    def _console(vm: RiscvTestMachine):
        """Xen's own console (con0)."""
        if vm.console is None:
            raise RuntimeError("Xen console not wired up, machine not launched?")
        return vm.console

    def run(self, vm: RiscvTestMachine) -> None:
        for strings in self.expect.values():
            cons = self._console(vm)
            for s in strings:
                self._expect_string(cons, s)

    def _expect_string(self, cons, expected: str) -> None:
        """Wait for `expected` on the console, retrying on timeout."""
        for attempt in range(self.attempts):
            try:
                cons.expect_exact(expected, timeout=self.timeout)
                return
            except pexpect.TIMEOUT:
                if attempt == self.attempts - 1:
                    raise
