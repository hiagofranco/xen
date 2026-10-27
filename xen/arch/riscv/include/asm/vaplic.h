/* SPDX-License-Identifier: MIT */
/*
 * xen/arch/riscv/vaplic.c
 *
 * Virtual RISC-V Advanced Platform-Level Interrupt Controller support
 *
 * Copyright (c) Microchip.
 */

#ifndef RISCV_VAPLIC_H
#define RISCV_VAPLIC_H

#include <xen/kernel.h>
#include <xen/spinlock.h>
#include <xen/types.h>

#include <asm/intc.h>

struct domain;
struct vcpu;

#define to_vaplic(d) container_of((d)->arch.vintc, struct vaplic, vintc)

/* State of one vAPLIC interrupt source. */
struct vaplic_source {
    /*
     * Protects target and sourcecfg, together with the corresponding h/w
     * registers of a h/w source.
     */
    spinlock_t lock;

    /* Guest's view of the APLIC target register. */
    uint32_t target;

    /*
     * sourcecfg register as the guest sees it:
     *  - for a virtual source, this is the only place it is kept;
     *  - for a h/w source, it is a copy of the h/w register, which only the
     *    guest changes.
     */
    uint32_t sourcecfg;

    /*
     * Level of the line of a virtual source, as the emulator of the device
     * in Xen drives it through vintc_set_irq_level(). Protected by
     * vaplic->lock.
     */
    bool level;
};

struct vaplic_regs {
    uint32_t domaincfg;

    /* Register to generate an extempore MSI. */
    uint32_t genmsi;

    /*
     * Interrupt-enable bits as the guest sees them, a word per 32 sources:
     *  - for a virtual source, this is the only place its bit is kept;
     *  - for a h/w source, it is a copy of the h/w bit, which only the guest
     *    changes. While domaincfg.IE is 0, the h/w bit is kept clear and the
     *    guest's view is kept here only.
     */
    uint32_t *ie;

    /*
     * Interrupt-pending bits of the virtual sources, a word per 32 sources.
     * Those of the h/w sources change in the h/w by themselves, so they are
     * read from the h/w instead.
     */
    uint32_t *ip;

    /*
     * Indexed by source number. The array holds d->arch.vintc->nr_virqs
     * elements; sources[0] is unused as APLIC interrupt sources start from 1.
     */
    struct vaplic_source *sources;
};

struct vaplic {
    struct vintc vintc;
    struct vaplic_regs regs;

    /*
     * Protects access to the vAPLIC registers. A vaplic_source.lock, where
     * needed too, is taken first, to avoid an ABBA deadlock (see
     * vaplic_sourcecfg_write()).
     */
    spinlock_t lock;

    paddr_t regs_start;
    unsigned int regs_size;
};

int domain_vaplic_init(struct domain *d);
void domain_vaplic_deinit(struct domain *d);

void vaplic_reconfigure_target(const struct vcpu *v);

#endif /* RISCV_VAPLIC_H */
