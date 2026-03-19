/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RISCV_GPR_NUM_H
#define RISCV_GPR_NUM_H

/* GPR ABI names, in register-number order (x0 .. x31). */
#define GPR_ABI_NAMES                   \
    zero, ra, sp, gp, tp, t0, t1, t2,   \
    s0, s1, a0, a1, a2, a3, a4, a5,     \
    a6, a7, s2, s3, s4, s5, s6, s7,     \
    s8, s9, s10, s11, t3, t4, t5, t6

#ifdef __ASSEMBLER__

    .equ    .L_gpr_num, 0
    .irp    name, GPR_ABI_NAMES
    .equ    .L_gpr_num_\name, .L_gpr_num
    .equ    .L_gpr_num, .L_gpr_num + 1
    .endr

#else /* __ASSEMBLER__ */

#include <xen/stringify.h>

#define DEFINE_ASM_GPR_NUMS                         \
"   .equ    .L_gpr_num, 0\n"                        \
"   .irp    name, " __stringify(GPR_ABI_NAMES) "\n" \
"   .equ    .L_gpr_num_\\name, .L_gpr_num\n"        \
"   .equ    .L_gpr_num, .L_gpr_num + 1\n"           \
"   .endr\n"

#endif /* __ASSEMBLER__ */

#endif /* RISCV_GPR_NUM_H */
