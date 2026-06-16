/* SPDX-License-Identifier: MIT */
/*
 * xen/arch/riscv/vaplic.c
 *
 * Virtual RISC-V Advanced Platform-Level Interrupt Controller support
 *
 * Copyright (c) Microchip.
 * Copyright (c) Vates
 */

#include <xen/errno.h>
#include <xen/fdt-kernel.h>
#include <xen/libfdt/libfdt.h>
#include <xen/sched.h>
#include <xen/xvmalloc.h>

#include <asm/aia.h>
#include <asm/imsic.h>
#include <asm/intc.h>
#include <asm/mmio.h>
#include <asm/vaplic.h>

#include "aplic-priv.h"

unsigned int __ro_after_init guest_aplic_num_sources;

#define VAPLIC_COMPATIBLE "riscv,aplic"

#define FDT_VAPLIC_INT_CELLS 2

#define AUTH_IRQ_BIT(d, irqn) ( \
    ((irqn) < (d)->arch.vintc->nr_virqs) && \
    test_bit(irqn, (d)->arch.vintc->used_irqs) )

/*
 * Convert a byte offset (within a SETIP/CLRIP/SETIE/CLRIE register group) to
 * a 32-bit word index into the allocated_irqs bitmap.  Each word covers 32
 * interrupt sources.  For SOURCECFG and TARGET groups the same division also
 * yields the interrupt number directly, because those arrays store one 32-bit
 * register per source.
 */
#define regoffset_to_word_idx(reg_val) ((reg_val) / sizeof(uint32_t))

static inline uint32_t generate_auth_mask(const struct domain *d,
                                          unsigned int word_idx)
{
    unsigned int first_bit = word_idx * sizeof(uint32_t) * BITS_PER_BYTE;

    if ( word_idx >= DIV_ROUND_UP(d->arch.vintc->nr_virqs,
                                  sizeof(uint32_t) * BITS_PER_BYTE) )
    {
        dprintk(XENLOG_DEBUG, "incorrect word_idx(%u) is passed\n", word_idx);

        return 0U;
    }

    return (uint32_t)(d->arch.vintc->used_irqs[first_bit / BITS_PER_LONG] >>
                      (first_bit % BITS_PER_LONG));
}

static int cf_check vaplic_emulate_load(const struct vcpu *v,
                                        const unsigned long addr,
                                        uint32_t *out)
{
    const struct domain *d = v->domain;
    const struct vaplic *vaplic = to_vaplic(d);
    const unsigned int offset = addr & APLIC_REG_OFFSET_MASK;
    uint32_t auth_mask;
    unsigned int i;

    switch ( offset )
    {
    case APLIC_DOMAINCFG:
        *out = vaplic->regs.domaincfg;

        return 0;

    case APLIC_SETIPNUM:
    case APLIC_SETIPNUM_LE:
    case APLIC_CLRIPNUM:
    case APLIC_SETIENUM:
    case APLIC_CLRIENUM:
    case APLIC_CLRIE_BASE ... APLIC_CLRIE_LAST:
        /*
         * Based on the RISC-V AIA spec a read of these registers
         * always returns zero
         */
        *out = 0;

        return 0;

    case APLIC_SETIP_BASE ... APLIC_SETIP_LAST:
    case APLIC_CLRIP_BASE ... APLIC_CLRIP_LAST:
    case APLIC_SETIE_BASE ... APLIC_SETIE_LAST:
        i = regoffset_to_word_idx(offset & APLIC_SETCLR_OFFSET_MASK);
        auth_mask = generate_auth_mask(d, i);

        break;

    case APLIC_TARGET_BASE ... APLIC_TARGET_LAST:
        /*
         * As target registers start from 1:
         *  0x3000 genmsi
         *  0x3004 target[1]
         *  0x3008 target[2]
         *   ...
         *  0x3FFC target[1023]
         * It is necessary to calculate an interrupt number by subtracting
         * APLIC_GENMSI instead of APLIC_TARGET_BASE.
         */
        i = regoffset_to_word_idx(offset - APLIC_GENMSI);

        if ( !AUTH_IRQ_BIT(d, i) )
        {
            *out = 0;

            return 0;
        }

        auth_mask = ~0U;

        break;

    default:
        gdprintk(XENLOG_WARNING, "Unhandled APLIC read at offset %#x\n",
                 offset);

        return -EINVAL;
    }

    *out = aplic_hw_read_reg(offset, auth_mask);

    return 0;
}

static int cf_check vaplic_emulate_store(const struct vcpu *v,
                                         unsigned long addr, uint32_t value)
{
    int rc = -EINVAL;
    const struct domain *d = v->domain;
    unsigned int offset = addr & APLIC_REG_OFFSET_MASK;

    switch ( offset )
    {
    case APLIC_SETIP_BASE ... APLIC_SETIP_LAST:
    case APLIC_CLRIP_BASE ... APLIC_CLRIP_LAST:
    case APLIC_SETIE_BASE ... APLIC_SETIE_LAST:
    case APLIC_CLRIE_BASE ... APLIC_CLRIE_LAST:
    {
        unsigned int word_idx =
            regoffset_to_word_idx(offset & APLIC_SETCLR_OFFSET_MASK);

        value &= generate_auth_mask(d, word_idx);

        break;
    }

    case APLIC_SOURCECFG_BASE ... APLIC_SOURCECFG_LAST:
        if ( value & APLIC_SOURCECFG_D )
        {
            rc = -EOPNOTSUPP;

            dprintk(XENLOG_ERR, "APLIC_SOURCECFG_D isn't supported\n");

            goto fail;
        }

        /*
         * As sourcecfg register starts from 1:
         *   0x0000 domaincfg
         *   0x0004 sourcecfg[1]
         *   0x0008 sourcecfg[2]
         *    ...
         *   0x0FFC sourcecfg[1023]
         * It is necessary to calculate an interrupt number by subtracting
         * APLIC_DOMAINCFG instead of APLIC_SOURCECFG_BASE.
         */
        if ( !AUTH_IRQ_BIT(d, regoffset_to_word_idx(offset - APLIC_DOMAINCFG)) )
            /* Interrupt not enabled, ignore it */
            return 0;

        if ( value > APLIC_SOURCECFG_SM_LEVEL_LOW )
        {
            gdprintk(XENLOG_ERR,
                     "value(%u) is incorrect for sourcecfg register\n", value);

            return 0;
        }

        break;

    case APLIC_TARGET_BASE ... APLIC_TARGET_LAST:
    {
        struct vcpu *target_vcpu = NULL;
        unsigned int hart_idx = value >> APLIC_TARGET_HART_IDX_SHIFT;

        /*
         * Look at vaplic_emulate_load() for explanation why
         * APLIC_GENMSI is subtracted.
         */
        if ( !AUTH_IRQ_BIT(d, regoffset_to_word_idx(offset - APLIC_GENMSI)) )
            /* Interrupt not enabled, ignore it */
            return 0;

        if ( hart_idx < v->domain->max_vcpus )
            target_vcpu = v->domain->vcpu[hart_idx];

        if ( !target_vcpu )
        {
            dprintk(XENLOG_ERR, "Invalid vCPU id in target register\n");

            /* Ignore such writings */
            return 0;
        }

        value = aplic_msi_target_gen(target_vcpu, value);

        break;
    }

    case APLIC_SETIPNUM:
    case APLIC_SETIPNUM_LE:
    case APLIC_CLRIPNUM:
    case APLIC_SETIENUM:
    case APLIC_CLRIENUM:
        if ( !value || !AUTH_IRQ_BIT(d, value) )
            return 0;

        break;

    case APLIC_DOMAINCFG:
    {
        struct vaplic *vaplic = to_vaplic(v->domain);

        /*
         * The domaincfg register has this format:
         * bits 31:24 read-only 0x80
         * bit 8      IE
         * bit 7      read-only 0
         * bit 2      DM (WARL)
         * bit 0      BE (WARL)
         *
         * The most interesting bit for us is IE(Interrupt Enable) bit.
         * At the moment, at least, Linux doesn't use domaincfg.IE bit to
         * disable interrupts globally, but if one day someone will use it
         * then extra actions should be done.
         *
         * Only DM (bit 2) and IE (bit 8) are writable here. They are assigned
         * (not OR-ed) so that a write of 0 can also clear them (WARL), and the
         * read-only high byte (0x80) is always kept set on read-back.
         */
        if ( value & ~(APLIC_DOMAINCFG_RO | APLIC_DOMAINCFG_DM |
                       APLIC_DOMAINCFG_IE) )
            printk_once("%s: Ignore writes to non-writable domaincfg bits as "
                        "they are set by aplic during initialization in Xen\n",
                        __func__);

        vaplic->regs.domaincfg = APLIC_DOMAINCFG_RO |
                                 (value & (APLIC_DOMAINCFG_DM |
                                           APLIC_DOMAINCFG_IE));

        return 0;
    }

    default:
        goto fail;
    }

    aplic_hw_write_reg(offset, value);

    return 0;

 fail:
    gdprintk(XENLOG_WARNING,
             "Unhandled APLIC write at offset %#x (value %#x)\n", offset,
             value);

    return rc;
}

static int cf_check vaplic_init(struct vcpu *v)
{
    return vcpu_imsic_init(v);
}

static void cf_check vaplic_deinit(struct vcpu *v)
{
    return vcpu_imsic_deinit(v);
}

static int __init cf_check vaplic_make_domu_dt_node(struct kernel_info *kinfo)
{
    struct domain *d = kinfo->bd.d;
    int res;
    void *fdt = kinfo->fdt;
    unsigned int msi_parent_phandle;
    char vaplic_name[32];
    unsigned int aplic_size = APLIC_SIZE(d->max_vcpus);
    const __be32 reg[] = {
        cpu_to_be32(GUEST_APLIC_S_BASE >> 32),
        cpu_to_be32(GUEST_APLIC_S_BASE),
        cpu_to_be32(0),
        cpu_to_be32(aplic_size),
    };

    BUILD_BUG_ON(APLIC_SIZE(MAX_VIRT_CPUS) > UINT_MAX);

    res = snprintf(vaplic_name, ARRAY_SIZE(vaplic_name), "/soc/aplic@%lx",
                   GUEST_APLIC_S_BASE);
    if ( res >= sizeof(vaplic_name) )
    {
        dprintk(XENLOG_DEBUG, "vaplic name is truncated\n");
        return -ENOBUFS;
    }

    res = vimsic_make_domu_dt_node(kinfo, &msi_parent_phandle);
    if ( res )
        return res;

    res = fdt_begin_node(fdt, vaplic_name);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", FDT_VAPLIC_INT_CELLS);
    if ( res )
        return res;

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "riscv,num-sources", guest_aplic_num_sources);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", VAPLIC_COMPATIBLE);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "msi-parent", msi_parent_phandle);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "phandle", kinfo->phandle_intc);
    if ( res )
        return res;

    return fdt_end_node(fdt);
}

static const struct vintc_init_ops __initconstrel init_ops = {
    .make_domu_dt_node = vaplic_make_domu_dt_node,
};

static enum io_state cf_check vaplic_mmio_read(struct vcpu *v, mmio_info_t *info,
                                               register_t *r)
{
    uint32_t data = 0;

    if ( info->len != sizeof(uint32_t) ||
         !IS_ALIGNED(info->gpa, sizeof(uint32_t)) )
    {
        gdprintk(XENLOG_DEBUG,
                 "VAPLIC: unaligned/wrong-width read gpa=%"PRIpaddr" len=%u\n",
                 info->gpa, info->len);
        return IO_ABORT;
    }

    if ( vaplic_emulate_load(v, info->gpa, &data) < 0 )
        return IO_ABORT;

    *r = data;
    return IO_HANDLED;
}

static enum io_state cf_check vaplic_mmio_write(struct vcpu *v, mmio_info_t *info,
                                                register_t r)
{
    if ( info->len != sizeof(uint32_t) ||
         !IS_ALIGNED(info->gpa, sizeof(uint32_t)) )
    {
        gdprintk(XENLOG_DEBUG,
                 "VAPLIC: unaligned/wrong-width write gpa=%"PRIpaddr" len=%u\n",
                 info->gpa, info->len);
        return IO_ABORT;
    }

    if ( vaplic_emulate_store(v, info->gpa, r) < 0 )
        return IO_ABORT;

    return IO_HANDLED;
}

static const struct mmio_handler_ops vaplic_mmio_ops = {
    .read  = vaplic_mmio_read,
    .write = vaplic_mmio_write,
};

void vaplic_state_save(struct vcpu *v)
{
    if ( has_msi_support() )
        imsic_state_save(v);
    else
        BUG_ON("unimplemented");
}

void vaplic_state_restore(struct vcpu *v)
{
    if ( has_msi_support() )
        imsic_state_restore(v);
    else
        BUG_ON("unimplemented");
}

static const struct vintc_ops vintc_ops = {
    .vcpu_init = vaplic_init,
    .vcpu_deinit = vaplic_deinit,
    .store_state = vaplic_state_save,
    .restore_state = vaplic_state_restore,
};

int domain_vaplic_init(struct domain *d)
{
    struct vaplic *vaplic = xvzalloc(struct vaplic);

    if ( !vaplic )
        return -ENOMEM;

    d->arch.vintc = &vaplic->vintc;
    d->arch.vintc->ops = &vintc_ops;
    d->arch.vintc->init_ops = &init_ops;

    vaplic->regs.domaincfg = APLIC_DOMAINCFG_RO;

    /*
     * APLIC source 0 is reserved; sources are numbered 1..guest_aplic_num_sources
     * and used directly as indices into used_irqs. Size the bitmap to
     * guest_aplic_num_sources + 1 so the highest source has a valid slot
     * (index 0 stays unused). Without the +1, vintc_reserve_virq() can't record
     * the top source, so domain_vintc_deinit() never releases it.
     */
    d->arch.vintc->nr_virqs = guest_aplic_num_sources + 1;

    vaplic->regs_start = GUEST_APLIC_S_BASE;
    vaplic->regs_size = APLIC_SIZE(d->max_vcpus);

    register_mmio_handler(d, &vaplic_mmio_ops,
                          vaplic->regs_start, vaplic->regs_size);

    return 0;
}

void domain_vaplic_deinit(struct domain *d)
{
    struct vaplic *vaplic;

    if ( !d->arch.vintc )
        return;

    vaplic = to_vaplic(d);
    d->arch.vintc = NULL;
    xvfree(vaplic);
}
