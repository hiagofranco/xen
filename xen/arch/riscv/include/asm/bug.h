/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2021-2023 Vates
 *
 */
#ifndef ASM__RISCV__BUG_H
#define ASM__RISCV__BUG_H

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

/*
 * The base instruction set has a fixed length of 32-bit naturally aligned
 * instructions.
 *
 * There are extensions of variable length ( where each instruction can be
 * any number of 16-bit parcels in length ).
 *
 * Compressed ISA is used now where the instruction length is 16 bit and
 * 'unimp' instruction, in this case, can be either 16 or 32 bit (
 * depending on if compressed ISA is used or not )
 */
#define INSN_LENGTH_MASK        _UL(0x3)
#define INSN_LENGTH_32          _UL(0x3)

#define COMPRESSED_INSN_MASK    _UL(0xffff)

#define GET_INSN_LENGTH(insn)                               \
    (((insn) & INSN_LENGTH_MASK) == INSN_LENGTH_32 ? 4 : 2) \

#endif /* !__ASSEMBLER__ */

#endif /* ASM__RISCV__BUG_H */
