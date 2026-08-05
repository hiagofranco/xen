#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""CLI launcher for the qtb riscv64 dom0less tests.

The test type comes first (e.g. `console-test`), then a command. Each type has
a default config file, which `-c/--config` overrides.

Commands:
    list  Print every test the type defines in the config, then exit.
    run   Boot one test's machine under QEMU and drive it to a pass/fail
          verdict.

Usage:
    ./run_test.py console-test list
    ./run_test.py console-test run dom0less-1smp-0domu-1vcpu-aplic-imsic-null
    ./run_test.py console-test run <test> -c some/other-config.yaml
"""

from __future__ import annotations

import argparse
import logging
import sys
from traceback import extract_tb, print_exc, format_exc
from collections.abc import Sequence

logger = logging.getLogger(__name__)

from qtb_test import RiscvQtbTest
from riscv64_test_machine import RiscvTestMachine


def _run_test(test: RiscvQtbTest, log_dir: str | None) -> int:
    """Compile the machine's device trees, boot it, and run the test."""
    vm = RiscvTestMachine(test.machine, timeout=test.timeout, log_dir=log_dir)
    try:
        with vm:
            vm.launch()
            test.run(vm)
    except Exception as exc:
        print(
            f"FAIL: {test.name}: {type(exc).__name__}: {exc}",
            file=sys.stderr,
            flush=True,
        )
        print_exc()
        return 1

    print(f"PASS: {test.name}", flush=True)
    return 0


def _config_for(ns: argparse.Namespace) -> tuple[type[RiscvQtbTest], str]:
    """Resolve the selected test type and its config file (with -c override)."""
    cls = RiscvQtbTest.for_type(ns.type)
    return cls, (ns.config or cls.config_file)


def _cmd_list(ns: argparse.Namespace) -> int:
    """`<type> list`: print every test the type defines."""
    cls, config = _config_for(ns)
    for name in cls.list_tests(config):
        print(name)
    return 0


def _cmd_run(ns: argparse.Namespace) -> int:
    """`<type> run`: build the named test and drive it to a verdict."""
    logging.basicConfig(
        level=logging.DEBUG if ns.verbose else logging.WARNING, format="%(message)s"
    )
    cls, config = _config_for(ns)
    test = cls.from_config(ns.test, config)
    return _run_test(test, ns.log_dir)


def setup_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Launch a qtb riscv64 dom0less test: run_test.py <type> <command>.",
    )
    # Every type shares the same `run`/`list` flags, so build each command's
    # arguments once as a parent parser and reuse them below. Each command
    # stores its handler via set_defaults(func=...); main() dispatches on it.
    config_arg = argparse.ArgumentParser(add_help=False)
    config_arg.add_argument(
        "-c",
        "--config",
        default=None,
        metavar="FILE",
        help="Override the type's default config file.",
    )
    run_args = argparse.ArgumentParser(add_help=False)
    run_args.add_argument("test", help="Name of the test to run.")
    run_args.add_argument(
        "--log-dir",
        default=None,
        metavar="DIR",
        help="Directory for all logs (QEMU process log, qtest, and the "
        "consoles as con<N>.log). When unset, no logs are written.",
    )
    run_args.add_argument(
        "-v", "--verbose", action="store_true", help="Print debug output"
    )

    # run_test.py <type> <command>
    types = parser.add_subparsers(dest="type", required=True)
    for t in RiscvQtbTest.known_types():
        desc = RiscvQtbTest.for_type(t).description
        cmds = types.add_parser(t, help=desc, description=desc).add_subparsers(
            dest="command", required=True
        )
        cmds.add_parser(
            "list",
            parents=[config_arg],
            description=desc,
            help="List the tests for the type and exit.",
        ).set_defaults(func=_cmd_list)
        cmds.add_parser(
            "run",
            parents=[config_arg, run_args],
            description=desc,
            help="Run one test.",
        ).set_defaults(func=_cmd_run)

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    ns = setup_parser().parse_args(argv)
    try:
        return ns.func(ns)
    except Exception as exc:
        logger.debug(format_exc())
        frame = extract_tb(exc.__traceback__)[-1]
        print(f"{frame.filename}:{frame.lineno}: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
