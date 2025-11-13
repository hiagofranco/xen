/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef RISCV_EMULATE_H
#define RISCV_EMULATE_H

struct cpu_user_regs;
struct vcpu;

void handle_guest_page_fault(struct cpu_user_regs *regs, unsigned long cause);

int handle_virt_instruction_fault(struct vcpu *v);

#endif /* RISCV_EMULATE_H */
