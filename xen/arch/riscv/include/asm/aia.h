/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef RISCV_AIA_H
#define RISCV_AIA_H

struct vcpu;

bool aia_usable(void);

void aia_init(void);

unsigned int vgein_assign(struct vcpu *v);
void vgein_release(struct vcpu *v, unsigned int vgein_id, unsigned int cpu);

void hgei_interrupt(void);

bool vcpu_aia_has_interrupts(const struct vcpu *vcpu, uint64_t mask);

#ifdef CONFIG_RISCV_32
void vcpu_aia_flush_interrupts(struct vcpu *vcpu);
void vcpu_aia_sync_interrupts(struct vcpu *vcpu);
#else
static inline void vcpu_aia_flush_interrupts(struct vcpu *vcpu)
{
    /* Nothing to do as hviph is used only for RV32. */
}

static inline void vcpu_aia_sync_interrupts(struct vcpu *vcpu)
{
    /* Nothing to do as vsieh is used only for RV32. */
}
#endif

void vcpu_aia_update_hvip(const struct vcpu *v);

#endif /* RISCV_AIA_H */
