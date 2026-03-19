/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef RISCV_EXTABLE_H
#define RISCV_EXTABLE_H

#include <asm/gpr-num.h>

#define EX_TYPE_FIXUP       0
#define EX_TYPE_TRAP_INFO   1

#ifdef __ASSEMBLER__

#define ASM_EXTABLE_RAW(insn, fixup, type, data)    \
    .pushsection .ex_table, "a";                    \
    .balign     4;                                  \
    .word       (insn) - .;                         \
    .word       (fixup) - .;                        \
    .half       (type);                             \
    .half       (data);                             \
    .popsection

.macro asm_extable, insn, fixup
    ASM_EXTABLE_RAW(\insn, \fixup, EX_TYPE_FIXUP, 0)
.endm

#else /* __ASSEMBLER__ */

#include <xen/stringify.h>
#include <xen/types.h>

struct cpu_user_regs;

#define ASM_EXTABLE_RAW(insn, fixup, type, data)    \
    ".pushsection .ex_table, \"a\"\n"               \
    ".balign    4\n"                                \
    ".word      (" insn ") - .\n"                   \
    ".word      (" fixup ") - .\n"                  \
    ".half      (" type ")\n"                       \
    ".half      (" data ")\n"                       \
    ".popsection\n"

#define ASM_EXTABLE(insn, fixup)    \
    ASM_EXTABLE_RAW(#insn, #fixup, __stringify(EX_TYPE_FIXUP), "0")

#define EX_TRAP_INFO_REG(gpr) "(.L_gpr_num_" #gpr ")"

#define ASM_EXTABLE_TRAP_INFO(insn, fixup, data)                    \
    DEFINE_ASM_GPR_NUMS "\n"                                        \
    ASM_EXTABLE_RAW(#insn, #fixup, __stringify(EX_TYPE_TRAP_INFO),  \
                    EX_TRAP_INFO_REG(data))

/*
 * Each exception table entry consists of two relative offsets and a
 * handler description: `insn` is the relative offset to an instruction
 * that is allowed to fault, `fixup` is the relative offset at which the
 * program should continue in case of a fault, `type` selects how the exception
 * is handled (EX_TYPE_*), and `data` holds auxiliary information for the
 * handler (e.g. for EX_TYPE_TRAP_INFO, the number of the GPR that contains a
 * pointer to a struct trap_info). No general-purpose registers are
 * modified by the exception handling mechanism itself, so it is up to
 * the fixup code to handle any necessary state cleanup.
 *
 * The exception table and fixup code live out of line with the main
 * instruction path. This means when everything is well, we don't even
 * have to jump over them. Further, they do not intrude on our cache or
 * tlb entries.
 */
struct exception_table_entry {
    int32_t insn, fixup;
    uint16_t type, data;
};

extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];

void sort_exception_tables(void);
bool fixup_exception(struct cpu_user_regs *regs, unsigned long cause);

#endif /* __ASSEMBLER__ */

#endif /* RISCV_EXTABLE_H */
