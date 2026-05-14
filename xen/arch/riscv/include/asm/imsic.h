/* SPDX-License-Identifier: MIT */

/*
 * xen/arch/riscv/include/asm/imsic.h
 *
 * RISC-V Incoming MSI Controller support
 *
 * (c) Microchip Technology Inc.
 */

#ifndef RISCV_IMSIC_H
#define RISCV_IMSIC_H

#include <xen/rwlock.h>
#include <xen/spinlock.h>
#include <xen/stdbool.h>
#include <xen/types.h>

#define IMSIC_MMIO_PAGE_SHIFT   12
#define IMSIC_MMIO_PAGE_SZ      (1UL << IMSIC_MMIO_PAGE_SHIFT)

#define IMSIC_MIN_ID            63
#define IMSIC_MAX_ID            2047

#define IMSIC_EIDELIVERY        0x70

#define IMSIC_EITHRESHOLD       0x72

#define IMSIC_EIP0              0x80
#define IMSIC_EIPx_BITS         32

#define IMSIC_EIE0              0xC0

struct imsic_msi {
    paddr_t base_addr;
    unsigned long offset;
};

struct imsic_config {
    /* Base address */
    paddr_t base_addr;

    /* Bits representing Guest index, HART index, and Group index */
    unsigned int guest_index_bits;
    unsigned int hart_index_bits;
    unsigned int group_index_bits;
    unsigned int group_index_shift;

    /* IMSIC phandle */
    unsigned int phandle;

    /* Number of parent irq */
    unsigned int nr_parent_irqs;

    /* Number off interrupt identities */
    unsigned int nr_ids;

    /*
     * Number of 64-bit EIx groups needed to cover all the interrupt
     * identities, which are 0 (never valid, but it still occupies a bit) up
     * to and including nr_ids.
     */
    unsigned int nr_eix;

    /* MSI */
    const struct imsic_msi *msi;

    /* Lock to protect access to IMSIC's stuff */
    spinlock_t lock;
};

/*
 * The identities of Xen's own interrupt file are allocated statically:
 *  - the reserved ones are taken from the top down; so far that's only the
 *    last one, nr_ids, for APLIC-to-hart synchronization;
 *  - 1 .. imsic_nr_free_ids(), i.e. all those below the reserved ones, are
 *    left for the APLIC sources, as Xen gives source N the EIID N
 *    (aplic_init() drops the sources which don't fit).
 * A new reserved identity has to be taken below the lowest one reserved so
 * far, with imsic_nr_free_ids() lowered accordingly.
 */

/* Interrupt identity reserved for APLIC-to-hart synchronization. */
static inline unsigned int imsic_sync_id(const struct imsic_config *cfg)
{
    return cfg->nr_ids;
}

/* Interrupt identities 1 .. imsic_nr_free_ids() aren't reserved. */
static inline unsigned int imsic_nr_free_ids(const struct imsic_config *cfg)
{
    return imsic_sync_id(cfg) - 1;
}

struct vimsic_state {
    /* IMSIC VS-file */
    rwlock_t vsfile_lock;
    /*
     * s/w IMSIC VS-file -> guest_file_id == 0
     * h/w IMSIC VS-file -> guest_file_id > 0
     */
    unsigned int guest_file_id;
    /*
     * s/w IMSIC VS-file -> vsfile_cpu == CPU_NONE
     * h/w IMSIC VS-file -> vsfile_cpu < NR_CPUS
     */
    unsigned int vsfile_cpu;
};

struct dt_device_node;
struct kernel_info;
struct vcpu;

int imsic_init(const struct dt_device_node *node);

const struct imsic_config *imsic_get_config(void);

void imsic_irq_enable(unsigned int hwirq);
void imsic_irq_disable(unsigned int hwirq);

void imsic_ids_local_delivery(bool enable);

int vcpu_imsic_init(struct vcpu *v);
void vcpu_imsic_deinit(struct vcpu *v);
unsigned int vcpu_guest_file_id(const struct vcpu *v);

int vimsic_make_domu_dt_node(struct kernel_info *kinfo, unsigned int *phandle);

void cf_check imsic_ctxt_switch_from(struct vcpu *p);
void cf_check imsic_ctxt_switch_to(struct vcpu *n);

int imsic_map_guest_file(struct vcpu *v, unsigned int vsfile_id);

void imsic_vsfile_attach(struct vcpu *v);

#endif /* RISCV_IMSIC_H */
