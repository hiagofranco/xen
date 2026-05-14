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
    /* Protects target together with the h/w target register of the source. */
    spinlock_t lock;

    /* Guest's view of the APLIC target register. */
    uint32_t target;
};

struct vaplic_regs {
    uint32_t domaincfg;

    /*
     * Indexed by source number. The array holds d->arch.vintc->nr_virqs
     * elements; sources[0] is unused as APLIC interrupt sources start from 1.
     */
    struct vaplic_source *sources;
};

struct vaplic {
    struct vintc vintc;
    struct vaplic_regs regs;

    paddr_t regs_start;
    unsigned int regs_size;
};

int domain_vaplic_init(struct domain *d);
void domain_vaplic_deinit(struct domain *d);

void vaplic_reconfigure_target(const struct vcpu *v);

#endif /* RISCV_VAPLIC_H */
