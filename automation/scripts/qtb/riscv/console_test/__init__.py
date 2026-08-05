# SPDX-License-Identifier: GPL-2.0-only
"""console-test type package.

Importing the package registers ConsoleTest with RiscvQtbTest (the subclass
registration happens as a side effect of importing the module below).
"""

from console_test.console_test import ConsoleTest
