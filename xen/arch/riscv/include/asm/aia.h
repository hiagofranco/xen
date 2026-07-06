/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef RISCV_AIA_H
#define RISCV_AIA_H

#include <xen/percpu.h>
#include <xen/spinlock.h>

struct vcpu;

bool aia_usable(void);

void aia_init(void);

unsigned int vgein_assign(struct vcpu *v);
void vgein_release(struct vcpu *v, unsigned int vgein_id);

void vcpu_aia_init(struct vcpu *v);

#endif /* RISCV_AIA_H */
