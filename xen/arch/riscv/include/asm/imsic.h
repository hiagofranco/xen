/* SPDX-License-Identifier: MIT */

/*
 * xen/arch/riscv/include/asm/imsic.h
 *
 * RISC-V Incoming MSI Controller support
 *
 * (c) Microchip Technology Inc.
 */

#ifndef ASM_RISCV_IMSIC_H
#define ASM_RISCV_IMSIC_H

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

    /*
     * MSI Target Address Scheme
     *
     * XLEN-1                                                12     0
     * |                                                     |     |
     * -------------------------------------------------------------
     * |xxxxxx|Group Index|xxxxxxxxxxx|HART Index|Guest Index|  0  |
     * -------------------------------------------------------------
     */

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
     * Interrupt identity reserved exclusively for APLIC-to-hart
     * synchronization.
     *
     * Must not be allocated to any interrupt source.
     */
    unsigned int sync_id;

    /* MSI */
    const struct imsic_msi *msi;

    /* Lock to protect access to IMSIC's stuff */
    spinlock_t lock;
};

struct vimsic_state {
    /* IMSIC VS-file */
    rwlock_t vsfile_lock;
    /*
     * s/w IMSIC VS-file -> guest_file_id == 0
     * h/w IMSIC VS-file -> guest_file_id > 0
     */
    unsigned int guest_file_id;
    /*
     * s/w IMSIC VS-file -> vsfile_pcpu == NR_CPUS
     * h/w IMSIC VS-file -> vsfile_pcpu < NR_CPUS
     */
    unsigned int vsfile_pcpu;
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

void imsic_update_state(struct vcpu *v, unsigned int guest_file_id);

int vimsic_make_domu_dt_node(struct kernel_info *kinfo, unsigned int *phandle);

int imsic_map_guest_file(struct vcpu *v, unsigned int vsfile_id);

void imsic_state_save(struct vcpu *v);
void imsic_state_restore(struct vcpu *v);

bool imsic_has_interrupt(const struct vcpu *vcpu);

void imsic_migrate_vcpu(struct vcpu *v);

#endif /* ASM_RISCV_IMSIC_H */
