# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the console-test test type (console_test.py)."""

from __future__ import annotations

from itertools import chain, repeat
from unittest import mock

import pexpect
import pytest

from console_test.console_test import ConsoleTest
from config import MachineConfig


# ---- helpers ----


def _parse_test_data(machine, expect, name="dummy"):
    """Validate `expect` against `machine`, without reading a catalog file."""
    test_data = {"machine": "box", "expect": expect}
    with mock.patch.object(MachineConfig, "from_config", return_value=machine):
        return ConsoleTest._parse_test_data("config.yaml", test_data, name)


def _console(fail_times: int = 0, matches: int = 1) -> mock.Mock:
    """Stand in for a pexpect spawn: `fail_times` timeouts, then `matches` hits.

    Every wait past `matches` times out, so a test that waits more often than it
    should fails instead of silently passing.
    """
    cons = mock.Mock()
    cons.expect_exact.side_effect = chain(
        [pexpect.TIMEOUT("nope")] * fail_times,
        [None] * matches,
        repeat(pexpect.TIMEOUT("nope")),
    )
    return cons


def _asked(cons: mock.Mock) -> list[str]:
    """The strings waited for on `cons`, one entry per attempt (matched or not)."""
    return [call.args[0] for call in cons.expect_exact.call_args_list]


def _test(expect, machine, **opts):
    """Build a ConsoleTest bound to `machine`, skipping the YAML read."""
    raw = {
        "machine_catalog": "config.yaml",
        "tests": {"dummy": {"machine": "box", "expect": expect}},
        **opts,
    }
    with mock.patch.object(MachineConfig, "from_config", return_value=machine):
        return ConsoleTest(raw, "dummy")


# ---- _parse_test_data ----


def test_parse_test_data_accepts_a_list_of_strings(make_machine):
    machine = make_machine()
    name, data, got = _parse_test_data(machine, {0: ["Hello", "All set up"]})
    assert (name, got) == ("dummy", machine)
    assert data["expect"] == {0: ["Hello", "All set up"]}


def test_parse_test_data_bare_string_raises(make_machine):
    # A bare string is refused, not wrapped: the YAML must spell out the list.
    machine = make_machine()
    with pytest.raises(ValueError, match="Xen expects a list of string"):
        _parse_test_data(machine, {0: "All set up"})


@pytest.mark.parametrize("index", [-1, 1])
def test_parse_test_data_console_index_out_of_range_raises(index, make_machine):
    machine = make_machine()
    with pytest.raises(ValueError, match=f"console index {index} out of range"):
        _parse_test_data(machine, {index: ["All set up"]})


def test_parse_test_data_empty_list_raises(make_machine):
    machine = make_machine()
    with pytest.raises(ValueError, match="Xen has no expected string"):
        _parse_test_data(machine, {0: []})


def test_parse_test_data_empty_string_in_list_raises(make_machine):
    machine = make_machine()
    with pytest.raises(ValueError, match="Xen has an empty expected string"):
        _parse_test_data(machine, {0: ["All set up", ""]})


def test_parse_test_data_missing_expect_raises():
    with pytest.raises(ValueError, match="missing keys"):
        ConsoleTest._parse_test_data("config.yaml", {"machine": "box"}, "dummy")


# ---- _parse_test_cfg ----


def test_parse_test_cfg_missing_machine_catalog_raises():
    with pytest.raises(ValueError, match="missing keys"):
        ConsoleTest._parse_test_cfg({"tests": {}}, "dummy")


def test_parse_test_cfg_unknown_test_raises():
    raw = {"machine_catalog": "config.yaml", "tests": {"a": {}}}
    with pytest.raises(ValueError, match="unknown test 'dummy'"):
        ConsoleTest._parse_test_cfg(raw, "dummy")


# ---- __init__ ----


def test_timeout_and_attempts_are_read(make_machine):
    test = _test({0: ["All set up"]}, make_machine(), timeout=7, attempts=2)
    assert (test.timeout, test.attempts) == (7, 2)


def test_timeout_below_one_raises(make_machine):
    with pytest.raises(ValueError, match="timeout < 1"):
        _test({0: ["All set up"]}, make_machine(), timeout=0)


def test_attempts_below_one_raises(make_machine):
    with pytest.raises(ValueError, match="attempts < 1"):
        _test({0: ["All set up"]}, make_machine(), attempts=0)


# ---- run ----


def test_run_expects_each_string_in_order_on_con0(make_machine):
    test = _test({0: ["first", "then"]}, make_machine())
    vm = mock.Mock(console=_console(matches=2))

    test.run(vm)

    assert _asked(vm.console) == ["first", "then"]


def test_run_xen_console_not_wired_raises(make_machine):
    test = _test({0: ["All set up"]}, make_machine())
    vm = mock.Mock(console=None)

    with pytest.raises(RuntimeError, match="not launched"):
        test.run(vm)


# ---- _expect_string ----


def test_expect_string_retries_after_a_timeout(make_machine):
    test = _test({0: ["All set up"]}, make_machine(), attempts=3)
    cons = _console(fail_times=2)

    test._expect_string(cons, "All set up")

    assert _asked(cons) == ["All set up"] * 3


def test_expect_string_raises_once_attempts_are_spent(make_machine):
    test = _test({0: ["All set up"]}, make_machine(), attempts=2)
    cons = _console(matches=0)

    with pytest.raises(pexpect.TIMEOUT):
        test._expect_string(cons, "All set up")

    assert _asked(cons) == ["All set up"] * 2


# ---- config IO ----


def test_list_tests_reads_the_type_yaml():
    names = ConsoleTest.list_tests("console_test/console-test.yaml")
    assert "dom0less-1smp-0domu-1vcpu-aplic-imsic-null" in names


def test_from_yaml_builds_instance(make_machine):
    machine = make_machine()

    raw = {
        "machine_catalog": "config.yaml",
        "tests": {"dummy": {"machine": "box", "expect": {0: ["All set up"]}}},
    }
    # Mock the YAML read and the catalog lookup: only the build logic is under test.
    with (
        mock.patch.object(ConsoleTest, "_load_yaml", return_value=raw),
        mock.patch.object(MachineConfig, "from_config", return_value=machine),
    ):
        test = ConsoleTest._from_yaml("console-test.yaml", "dummy")

    assert test.name == "dummy"
    assert test.type_id == "console-test"
    assert test.machine is machine
    assert test.expect == {0: ["All set up"]}


def test_from_config_rejects_json():
    # ConsoleTest has no JSON parser yet, an unimplemented error must raise.
    with pytest.raises(NotImplementedError, match="no JSON parser"):
        ConsoleTest.from_config("whatever", "console-test.json")


def test_from_config_rejects_unknown_format():
    with pytest.raises(ValueError, match="unsupported config format"):
        ConsoleTest.from_config("whatever", "console-test.toml")
