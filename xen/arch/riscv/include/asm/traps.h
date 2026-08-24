/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef RISCV_TRAPS_H
#define RISCV_TRAPS_H

#include <asm/processor.h>

#ifndef __ASSEMBLER__

struct trap_info {
    register_t sepc;
    register_t scause;
    register_t stval;
};

void do_trap(struct cpu_user_regs *cpu_regs);
void handle_trap(void);
void trap_init(void);

/* Reflect @trap back to the guest, i.e. enter its VS-mode trap handler. */
void trap_redirect(const struct trap_info *trap);

#endif /* __ASSEMBLER__ */

#endif /* RISCV_TRAPS_H */
