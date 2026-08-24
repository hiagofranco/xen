/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef RISCV_EMULATE_H
#define RISCV_EMULATE_H

struct cpu_user_regs;

void handle_guest_page_fault(struct cpu_user_regs *regs, unsigned long cause);

#endif /* RISCV_EMULATE_H */
