/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ASM__RISCV__TRAPS_H
#define ASM__RISCV__TRAPS_H

#include <asm/processor.h>
#include <asm/riscv_encoding.h>

#ifndef __ASSEMBLER__

struct trap_info {
    register_t sepc;
    register_t scause;
    register_t stval;
};

static inline bool is_load_guest_page_fault(unsigned long scause)
{
    return (scause == CAUSE_LOAD_GUEST_PAGE_FAULT);
}

void do_trap(struct cpu_user_regs *cpu_regs);
void handle_trap(void);
void trap_init(void);

#endif /* __ASSEMBLER__ */

#endif /* ASM__RISCV__TRAPS_H */
