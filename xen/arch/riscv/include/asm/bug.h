/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2021-2023 Vates
 *
 */
#ifndef RISCV_BUG_H
#define RISCV_BUG_H

#ifndef __ASSEMBLER__

/*
 * The de facto standard implementation of this instruction is:
 *   C.UNIMP: 0000. The all-zeroes pattern is not a valid instruction. Any
 *   system which traps on invalid instructions will thus trap on this UNIMP
 *   instruction form. Despite not being a valid instruction, it still fits
 *   the 16-bit (compressed) instruction format, and so 0000 0000 is
 *   interpreted as being two 16-bit UNIMP instructions.
 *
 *   UNIMP : C0001073. This is an alias for CSRRW x0, cycle, x0. Since cycle is
 *   a read-only CSR, then (whether this CSR exists or not) an attempt to write
 *   into it will generate an illegal instruction exception. This 32-bit form
 *   of UNIMP is emitted when targeting a system without the C extension, or
 *   when the .option norvc directive is used.
 */
#define BUG_INSTR "unimp"

/*
 * We consider it a feature that any length of instruction containing all zero
 * bits is not legal, as this quickly traps erroneous jumps into zeroed memory
 * regions. Similarly, we also reserve the instruction encoding containing all
 * ones to be an illegal instruction, to catch the other common pattern
 * observed with unprogrammed non-volatile memory devices, disconnected memory
 * buses, or broken memory devices.
 *
 * All ones is choosen as an illegal instruction here to be sure that it is
 * different from BUG_INSTR as BUG_INSTR is reserved for macros from xen/bug.h.
 */
#define ILLEGAL_INSTR           _AC(-1, U)

#endif /* !__ASSEMBLER__ */

#endif /* RISCV_BUG_H */
