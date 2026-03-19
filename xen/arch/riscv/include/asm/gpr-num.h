/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on Linux 6.16 arch/riscv/include/asm/gpr-num.h, with the
 * register list turned into GPR_LIST() instead of being open-coded
 * with .irp, so that it can also be used outside of assembly.
 */
#ifndef RISCV_GPR_NUM_H
#define RISCV_GPR_NUM_H

/*
 * GPRs by ABI name, together with their register number (x0 .. x31).
 *
 * This is the single source of truth for the mapping; users expand it with
 * their own per-register macro. Among them, the layout of struct cpu_user_regs
 * is checked against this list at build time (see build_assertions() in
 * domain.c), so neither list can be changed without the other.
 */
#define GPR_LIST(x)                                 \
    x(0,  zero) x(1,  ra)  x(2,  sp)  x(3,  gp)     \
    x(4,  tp)   x(5,  t0)  x(6,  t1)  x(7,  t2)     \
    x(8,  s0)   x(9,  s1)  x(10, a0)  x(11, a1)     \
    x(12, a2)   x(13, a3)  x(14, a4)  x(15, a5)     \
    x(16, a6)   x(17, a7)  x(18, s2)  x(19, s3)     \
    x(20, s4)   x(21, s5)  x(22, s6)  x(23, s7)     \
    x(24, s8)   x(25, s9)  x(26, s10) x(27, s11)    \
    x(28, t3)   x(29, t4)  x(30, t5)  x(31, t6)

/* Number of GPRs described by GPR_LIST(). */
#define NR_GPRS 32

#ifdef __ASSEMBLER__

/*
 * Generate the .L_gpr_num_<name> assembler symbols, used to turn a register
 * name emitted by the compiler into a register number.
 *
 * The separator is emitted ahead of each entry rather than after it, so that
 * the expansion doesn't end with one: whatever follows a use of the list has
 * to supply its own separator, instead of silently relying on a trailing one.
 */
#define GPR_NUM_EQU(num, name)  ; .equ .L_gpr_num_##name, num
GPR_LIST(GPR_NUM_EQU)
#undef GPR_NUM_EQU

#else /* __ASSEMBLER__ */

/* See the comment above the assembler GPR_NUM_EQU() about the separator. */
#define GPR_NUM_EQU(num, name)  "\n.equ .L_gpr_num_" #name ", " #num
#define DEFINE_ASM_GPR_NUMS     GPR_LIST(GPR_NUM_EQU)

#endif /* __ASSEMBLER__ */

#endif /* RISCV_GPR_NUM_H */
