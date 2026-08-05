qtb riscv64 test framework
==========================

A small framework that boots Xen under QEMU on riscv64 and drives it to a
pass/fail verdict automatically from its console output.

It is built on QEMU's qtb (QEMU Test Bench) Python package, which gives
programmatic control of a QEMU process over QMP and qtest, plus access to the
consoles.

What it does
------------

1. Reads a machine description (what to boot: cpus, memory, interrupt
   controller) and a test description (what to assert).
2. Generates the host device tree.
3. Launches QEMU with Xen and the firmware wired in.
4. Reads the console and checks what Xen printed.

Core concepts
-------------

- Machine: test-agnostic description of what to boot. Reusable across test
  types. `config.yaml` -> `MachineConfig`.
- Test type: a `RiscvQtbTest` subclass implementing the logic of a kind of test
  (e.g. `console-test`). Identified by `type_id`. See `console_test/`.
- Test: one named, runnable instance of a type: a machine plus the type's
  parameters. Lives in the type's `<type>.yaml`.

A test type owns a config file describing its tests, each test names a machine
from the shared catalog (`config.yaml`) and layers its own parameters on top.

Layout
------

```
run_test.py              CLI entry point (<type> run | list)
qtb_test.py              RiscvQtbTest ABC + type registry
config.py                Machine catalog parser -> MachineConfig
xen_dt.py                Generates the host device tree from its Jinja2 template
dt.py                    Compile .dts -> .dtb with dtc (prebuilt .dtb passes through)
paths.py                 Path resolution (framework-relative) + scratch temp dir
riscv64_test_machine.py  RiscvTestMachine: assembles the QEMU command line

config.yaml              The machine catalog (shared across test types)
dts/                     Jinja2 device-tree templates (host, common)

console_test/            The console-test type
  __init__.py            Re-exports ConsoleTest so importing the package registers it
  console_test.py        ConsoleTest implementation
  console-test.yaml      Its tests

unit/                    pytest unit tests of the framework logic itself
```

How a type is selected
----------------------

The test type is the first positional argument (`run_test.py console-test run
...`). Types register themselves: defining a non-abstract `RiscvQtbTest`
subclass with a `type_id` adds it to the test registry automatically.

Each type declares a default `config_file`; `-c/--config` overrides it.

CLI usage
---------

```
# List every test the type defines in its config:
./run_test.py console-test list

# Run one test (drives it to PASS/FAIL, exit 0/1):
./run_test.py console-test run dom0less-1smp-0domu-1vcpu-aplic-imsic-null \
    --log-dir qtb-logs

# Use a different config file:
./run_test.py console-test run <test> -c some/other-config.yaml
```

`--log-dir` (run only) collects the QEMU process log, the qtest log, and each
console as `con<N>.log`. Omit it to write no logs. `-v/--verbose` raises the
log level to debug.

Config files
------------

`config.yaml` is the machine catalog. `binaries:` are build artifacts resolved
under `binaries/` (overridable with `$QTB_BINARIES_DIR`); absolute paths pass
through. YAML anchors (`*aplic_imsic`, `<<:`) keep shared values in one place.

Optional fields are omitted from an entry when they equal the default, so the
full set of fields lives here rather than in the file.

Machine (each entry under `machines:`):

- `pcpu` (required): host physical cpus.
- `interrupt_controller` (required): `aplic-imsic`.
- `aia_guests` (aplic-imsic only): VS-level guest interrupt files per hart.
- `memory` (default 2048): host memory (MB).
- `mmu_type` (default `sv48`): Xen host MMU type.
- `xen_bootargs` (default `""`): Xen command line.

`<type>.yaml` holds the type's tests logic description.

`console-test`
--------------

A test names a machine and maps a console index to the string(s) expected on
that console: 0 is Xen's own console.

```
machine_catalog: config.yaml    # the catalog to resolve machine names against
tests:
  dom0less-1smp-0domu-1vcpu-aplic-imsic-null:
    machine: dom0less-1smp-0domu-1vcpu-aplic-imsic-null   # a name in config.yaml
    expect:
      0: [All set up]           # Xen itself must print "All set up"
```

Logic, per console:

1. read the console
2. wait for each expected string in turn, in the order listed
3. bound each wait by `timeout` seconds, retrying a timed-out wait up to
   `attempts` times

The map itself must not be empty, otherwise the test would pass without
asserting anything.

Device trees (dts/)
-------------------

Jinja2 templates, generated per machine and compiled with dtc:

- `qemu-host.dts.j2` - the Xen host tree: the hart count, the host MMU type and
  the Xen command line.
- `qemu-common.dts.j2` - shared fragments included by the above.

Adding a test
-------------

To add a test to an existing type (e.g. `console-test`):

1. Pick a machine from `config.yaml`, or add a new one under `machines:` (reuse
   an interrupt-controller anchor, e.g. `<<: *aplic_imsic`, and set `pcpu` —
   see the field list above).
2. Add a test entry under `tests:` in the type's `<type>.yaml`, naming that
   machine and supplying the type's own parameters (for `console-test`, one
   `expect` list per console).
3. Run it: `./run_test.py console-test run <your-test-name>`.

No code change is needed, a test is pure config.

Adding a new test type
----------------------

1. Create `mytype/` with `mytype.py` defining a `RiscvQtbTest` subclass: set
   `type_id`, `description`, and `config_file`, and implement `from_config`,
   `list_tests`, and `run(vm)`.
2. Add `mytype/__init__.py` that does `from mytype.mytype import MyType` so
   importing the package registers the subclass.
3. `import mytype` in `run_test.py` so the type registers on startup.

Unit tests
----------

The `unit/` directory holds pytest tests of the framework's own logic (config
parsing, device-tree rendering, QEMU arg assembly). They do not boot QEMU and
are independent of the CI smoke tests.

```
python -m pytest unit/
```
