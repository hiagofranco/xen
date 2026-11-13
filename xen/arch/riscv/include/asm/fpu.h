/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_RISCV_FPU_H__
#define __ASM_RISCV_FPU_H__

#include <xen/types.h>

struct vcpu;

void fpu_init(void);
bool cpu_has_fpu(void);

#ifdef CONFIG_RISCV_64
void fpu_save(struct vcpu *v);
void fpu_restore(struct vcpu *v);
#else
static inline void fpu_save(struct vcpu *v) {}
static inline void fpu_restore(struct vcpu *v) {}
#endif

#endif /* __ASM_RISCV_FPU_H__ */
