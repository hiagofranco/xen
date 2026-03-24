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
#include <xen/nospec.h>
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

/*
 * Only a h/w source of a domain, one a h/w IRQ is routed to (see
 * vintc.hw_irqs), may be accessed in the h/w. A virtual source has nothing
 * there: until its state is emulated, it reads as zero and ignores writes,
 * like a source not assigned to the domain.
 */
static bool vaplic_is_hw_source(const struct domain *d, unsigned int irqn)
{
    const struct vintc *vintc = d->arch.vintc;

    return (irqn < vintc->nr_virqs) &&
           test_bit(array_index_nospec(irqn, vintc->nr_virqs),
                    vintc->hw_irqs);
}

/*
 * Convert a byte offset (within a SETIP/CLRIP/SETIE/CLRIE register group) to
 * a 32-bit word index into the hw_irqs bitmap. Each word covers 32
 * interrupt sources. For SOURCECFG and TARGET groups the same division also
 * yields the interrupt number directly, because those arrays store one 32-bit
 * register per source.
 */
#define regoffset_to_word_idx(reg_val) ((reg_val) / sizeof(uint32_t))

/*
 * vAPLIC keeps no shadow copy of sourcecfg[], and a guest's writes to it go
 * straight to the h/w register, so it is the h/w register which tells whether
 * a source is active.
 */
static bool vaplic_source_is_active(unsigned int irqn)
{
    uint32_t sourcecfg = aplic_hw_read_reg(APLIC_SOURCECFG_BASE +
                                           (irqn - 1) * sizeof(uint32_t));

    /*
     * A source is inactive if it is delegated to a child domain (D = 1), in
     * which case the low bits hold the Child Index rather than SM, or if it
     * isn't delegated and SM is Inactive.
     */
    return !(sourcecfg & APLIC_SOURCECFG_D) &&
           (MASK_EXTR(sourcecfg, APLIC_SOURCECFG_SM) !=
            APLIC_SOURCECFG_SM_INACTIVE);
}

static uint32_t vaplic_target_read(const struct domain *d, unsigned int irqn)
{
    struct vaplic *vaplic = to_vaplic(d);
    struct vaplic_source *src;
    unsigned long flags;
    uint32_t val = 0;

    /* target[0] doesn't exist so irqn == 0 should be impossible */
    if ( !irqn || irqn >= vaplic->vintc.nr_virqs )
        return 0;

    irqn = array_index_nospec(irqn, vaplic->vintc.nr_virqs);
    src = &vaplic->regs.sources[irqn];

    /*
     * src->lock makes the read atomic as a whole, against a write to
     * target[] which changes both this copy and the h/w register.
     */
    spin_lock_irqsave(&src->lock, flags);

    /* target[i] is read-only zero while source i is inactive. */
    if ( vaplic_source_is_active(irqn) )
        val = src->target;

    spin_unlock_irqrestore(&src->lock, flags);

    return val;
}

/* Emulate a write of @value to target[@irqn]. */
static void vaplic_target_write(const struct domain *d, unsigned int irqn,
                                uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    struct vimsic_state *vimsic_state;
    const struct vcpu *target_vcpu;
    struct vaplic_source *src;
    unsigned long flags;

    if ( !vaplic_is_hw_source(d, irqn) )
        return;

    irqn = array_index_nospec(irqn, d->arch.vintc->nr_virqs);

    /* target[i] is read-only zero while source i is inactive. */
    if ( !vaplic_source_is_active(irqn) )
        return;

    target_vcpu = domain_vcpu(d, MASK_EXTR(value, APLIC_TARGET_HART_IDX));
    if ( !target_vcpu )
    {
        gdprintk(XENLOG_ERR, "Invalid vCPU id in target register\n");

        return;
    }

    /*
     * Guest Index names an interrupt file of a nested guest, and the AIA
     * spec makes it read-only zero unless the domain's harts implement the
     * H extension, which a guest's harts don't. Bit 11 is reserved and reads
     * as zero, so only Hart Index and EIID are kept.
     * What ends up in the h/w register is Xen's own value anyway:
     * aplic_msi_target_gen() takes only EIID from it.
     */
    value &= APLIC_TARGET_HART_IDX | APLIC_TARGET_EIID;

    src = &vaplic->regs.sources[irqn];
    vimsic_state = target_vcpu->arch.vimsic_state;

    spin_lock_irqsave(&src->lock, flags);

    write_atomic(&src->target, value);

    /* IRQs are already off under src->lock. */
    read_lock(&vimsic_state->vsfile_lock);

    /*
     * The h/w register names the hart and the guest interrupt file MSIs are
     * sent to, and there is none of either until the target vCPU is given a
     * h/w guest interrupt file, so until then only the stored copy is
     * updated. imsic_vsfile_attach() writes it to the h/w, through
     * vaplic_reconfigure_target(), once there is such a file.
     *
     * TODO: once s/w guest interrupt files are supported, a vCPU using one
     * still needs its MSIs, so the h/w register will have to be written for
     * it too, pointing at an interrupt file Xen receives them in on the
     * vCPU's behalf.
     */
    if ( vimsic_state->vsfile_cpu != CPU_NONE )
        aplic_hw_write_reg(offsetof(struct aplic_regs, target[irqn - 1]),
                           aplic_msi_target_gen(target_vcpu, value));

    read_unlock(&vimsic_state->vsfile_lock);

    spin_unlock_irqrestore(&src->lock, flags);
}

/* Decode the target vCPU from a TARGET snapshot (NULL if none). */
static struct vcpu *vaplic_target_decode_vcpu(const struct domain *d,
                                              uint32_t target)
{
    unsigned int hart_idx = MASK_EXTR(target, APLIC_TARGET_HART_IDX);

    /*
     * Skip a source whose EIID is 0: interrupt identity 0 is never valid at
     * an IMSIC, so such a source delivers nothing and needs no retargeting.
     * The AIA spec leaves target[] UNSPECIFIED after reset, but the vAPLIC
     * state is zero-initialized, so this also covers every source the guest
     * hasn't programmed yet, which would otherwise decode as targeting vCPU0
     * (hart index 0) and have its h/w TARGET register needlessly rewritten
     * on every vCPU0 migration.
     * The hart index range check is only a guard: the write path refuses to
     * record values with an out-of-range hart index.
     */
    if ( !(target & APLIC_TARGET_EIID) || hart_idx >= d->max_vcpus )
        return NULL;

    return d->vcpu[hart_idx];
}

void vaplic_reconfigure_target(const struct vcpu *v)
{
    struct domain *d = v->domain;
    struct vaplic *vaplic = to_vaplic(d);
    struct vimsic_state *vimsic_state = v->arch.vimsic_state;
    unsigned int irqn;

    bitmap_for_each ( irqn, d->arch.vintc->hw_irqs, d->arch.vintc->nr_virqs )
    {
        struct vaplic_source *src = &vaplic->regs.sources[irqn];
        unsigned long flags;
        uint32_t target;

        /* APLIC interrupt sources start from 1, there is no source 0. */
        if ( !irqn )
            continue;

        spin_lock_irqsave(&src->lock, flags);

        target = read_atomic(&src->target);

        if ( vaplic_target_decode_vcpu(d, target) == v )
        {
            /*
             * IRQs are already off under src->lock so no need for *_irqsave
             * here.
             */
            read_lock(&vimsic_state->vsfile_lock);
            aplic_hw_write_reg(offsetof(struct aplic_regs, target[irqn - 1]),
                               aplic_msi_target_gen(v, target));
            read_unlock(&vimsic_state->vsfile_lock);
        }

        spin_unlock_irqrestore(&src->lock, flags);
    }
}

static uint32_t generate_auth_mask(const struct domain *currd,
                                   unsigned int word_idx)
{
    unsigned int nr_words = DIV_ROUND_UP(currd->arch.vintc->nr_virqs,
                                         sizeof(uint32_t) * BITS_PER_BYTE);
    unsigned int first_bit;

    if ( word_idx >= nr_words )
    {
        gdprintk(XENLOG_DEBUG, "incorrect word_idx(%u)\n", word_idx);

        return 0;
    }

    first_bit = array_index_nospec(word_idx, nr_words) *
                sizeof(uint32_t) * BITS_PER_BYTE;

    return currd->arch.vintc->hw_irqs[first_bit / BITS_PER_LONG] >>
           (first_bit % BITS_PER_LONG);
}

static bool vaplic_emulate_load(const struct vcpu *curr, paddr_t addr,
                                uint32_t *out)
{
    const struct domain *currd = curr->domain;
    const struct vaplic *vaplic = to_vaplic(currd);
    const unsigned int offset = addr - vaplic->regs_start;
    uint32_t auth_mask;
    unsigned int i;

    ASSERT(curr == current);

    switch ( offset )
    {
    case APLIC_DOMAINCFG:
        *out = vaplic->regs.domaincfg;

        return true;

    case APLIC_SOURCECFG_BASE ... APLIC_SOURCECFG_LAST:
        /*
         * Look at vaplic_emulate_store() for explanation why APLIC_DOMAINCFG
         * is subtracted.
         */
        i = regoffset_to_word_idx(offset - APLIC_DOMAINCFG);
        *out = vaplic_is_hw_source(currd, i) ? aplic_hw_read_reg(offset) : 0;

        return true;

    case APLIC_SETIP_BASE ... APLIC_SETIP_LAST:
    case APLIC_CLRIP_BASE ... APLIC_CLRIP_LAST:
    case APLIC_SETIE_BASE ... APLIC_SETIE_LAST:
        i = regoffset_to_word_idx(offset & APLIC_SETCLR_OFFSET_MASK);
        auth_mask = generate_auth_mask(currd, i);

        break;

    case APLIC_SETIPNUM:
    case APLIC_CLRIPNUM:
    case APLIC_SETIENUM:
    case APLIC_CLRIE_BASE ... APLIC_CLRIE_LAST:
    case APLIC_CLRIENUM:
    case APLIC_SETIPNUM_LE:
        /*
         * Based on the RISC-V AIA spec a read of these registers
         * always returns zero
         */
        *out = 0;

        return true;

    /*
     * vAPLIC is a supervisor-level domain with BE hardwired to 0, and such
     * a domain doesn't implement these registers: the MSI address
     * configuration ones exist in machine-level domains only, and
     * setipnum_be isn't needed by a little-endian only domain. Bytes of a
     * register which isn't implemented are read-only zero, like other
     * reserved bytes.
     */
    case APLIC_MMSICFGADDR:
    case APLIC_MMSICFGADDRH:
    case APLIC_SMSICFGADDR:
    case APLIC_SMSICFGADDRH:
    case APLIC_SETIPNUM_BE:
        *out = 0;

        return true;

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

        *out = vaplic_is_hw_source(currd, i) ? vaplic_target_read(currd, i) : 0;

        return true;

    /*
     * TODO: emulate genmsi. A write to it has to send an extempore MSI to
     * the target vCPU, and a guest typically waits for that MSI to become
     * pending in its interrupt file. Just recording the written value would
     * leave such a guest waiting forever, so until genmsi is emulated, an
     * access to it is treated as an unhandled one, which gets the domain
     * crashed.
     */
    case APLIC_GENMSI:
    default:
        gdprintk(XENLOG_WARNING, "Unhandled APLIC read at offset %#x\n",
                 offset);

        return false;
    }

    *out = aplic_hw_read_reg(offset) & auth_mask;

    return true;
}

static bool vaplic_emulate_store(const struct vcpu *curr, paddr_t addr,
                                 uint32_t value)
{
    const struct domain *currd = curr->domain;
    struct vaplic *vaplic = to_vaplic(currd);
    unsigned int offset = addr - vaplic->regs_start;

    ASSERT(curr == current);

    switch ( offset )
    {
    case APLIC_DOMAINCFG:
        /*
         * TODO: domaincfg.IE isn't honoured: the h/w forwards the MSIs of the
         * h/w sources whatever its value is. Linux as a guest sets IE before
         * it enables any source and never clears it, so a write may set IE,
         * but not clear it.
         */
        vaplic->regs.domaincfg |= value & APLIC_DOMAINCFG_WMASK;

        return true;

    case APLIC_SOURCECFG_BASE ... APLIC_SOURCECFG_LAST:
        /*
         * vAPLIC is a leaf domain: it has no child domain a source could be
         * delegated to. With D = 1, the low bits of the value would hold a
         * Child Index rather than SM, and for a leaf domain the AIA spec
         * makes such a write set the entire register to zero instead.
         */
        if ( value & APLIC_SOURCECFG_D )
        {
            gdprintk(XENLOG_DEBUG,
                     "sourcecfg write with D set at offset %#x, zeroing it\n",
                     offset);

            value = 0;
        }

        /*
         * SM is the only field left, the other bits are reserved and read as
         * zero, so ignore what a guest writes to them.
         */
        value &= APLIC_SOURCECFG_SM;

        /*
         * SM is WARL, so the reserved values 0x2 and 0x3 need no handling
         * here: vAPLIC keeps no shadow copy of sourcecfg[], the value is
         * written straight to the h/w register and is read back from it, so
         * it is the h/w which substitutes a legal value for an illegal one.
         *
         * TODO: when vAPLIC starts to shadow sourcecfg[], the WARL behaviour
         * will have to be emulated here instead, e.g.:
         *   if ( value == 0x2 || value == 0x3 )
         *       value = APLIC_SOURCECFG_SM_INACTIVE;
         */

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
        if ( !vaplic_is_hw_source(currd,
                                  regoffset_to_word_idx(offset -
                                                        APLIC_DOMAINCFG)) )
            /* Interrupt not enabled, ignore it */
            return true;

        break;

    case APLIC_SETIP_BASE ... APLIC_SETIP_LAST:
    case APLIC_CLRIP_BASE ... APLIC_CLRIP_LAST:
    case APLIC_SETIE_BASE ... APLIC_SETIE_LAST:
    case APLIC_CLRIE_BASE ... APLIC_CLRIE_LAST:
    {
        unsigned int word_idx =
            regoffset_to_word_idx(offset & APLIC_SETCLR_OFFSET_MASK);

        value &= generate_auth_mask(currd, word_idx);

        break;
    }

    case APLIC_SETIPNUM:
    case APLIC_CLRIPNUM:
    case APLIC_SETIENUM:
    case APLIC_CLRIENUM:
    case APLIC_SETIPNUM_LE:
        if ( !value || !vaplic_is_hw_source(currd, value) )
            return true;

        break;

    /* Not implemented by vAPLIC, see vaplic_emulate_load(). */
    case APLIC_MMSICFGADDR:
    case APLIC_MMSICFGADDRH:
    case APLIC_SMSICFGADDR:
    case APLIC_SMSICFGADDRH:
    case APLIC_SETIPNUM_BE:
        return true;

    case APLIC_TARGET_BASE ... APLIC_TARGET_LAST:
        /*
         * Look at vaplic_emulate_load() for explanation why APLIC_GENMSI is
         * subtracted.
         */
        vaplic_target_write(currd,
                            regoffset_to_word_idx(offset - APLIC_GENMSI),
                            value);

        return true;

    /* TODO: emulate genmsi, see vaplic_emulate_load(). */
    case APLIC_GENMSI:
    default:
        gdprintk(XENLOG_WARNING,
                 "Unhandled APLIC write at offset %#x (value %#x)\n",
                 offset, value);

        return false;
    }

    /*
     * What is left is passed through to the h/w, which can affect only the
     * h/w sources of currd:
     *  - setip, clrip, setie and clrie are write-1-to-set/clear, and the
     *    value was masked with currd's h/w sources above, so the bits of
     *    the other sources are written as 0, which leaves them untouched;
     *  - {set,clr}i{p,e}num, setipnum_le and sourcecfg[] name one source,
     *    which was checked above to be a h/w source of currd.
     * route_irq_to_guest() never assigns a source to more than one domain,
     * or to both a domain and Xen, and as virq == pirq, no translation is
     * needed either.
     */
    aplic_hw_write_reg(offset, value);

    return true;
}

static int __init cf_check vaplic_make_domu_dt_node(struct kernel_info *kinfo)
{
    int res;
    void *fdt = kinfo->fdt;
    unsigned int msi_parent_phandle;
    char vaplic_name[32];
    const __be32 reg[] = {
        cpu_to_be32(GUEST_APLIC_S_BASE >> 32),
        cpu_to_be32(GUEST_APLIC_S_BASE),
        cpu_to_be32(0),
        cpu_to_be32(APLIC_SIZE(0)),
    };

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

static enum io_state cf_check vaplic_mmio_read(struct vcpu *curr,
                                               mmio_info_t *info)
{
    uint32_t val;

    ASSERT(curr == current);

    if ( info->len != sizeof(uint32_t) ||
         !IS_ALIGNED(info->gpa, sizeof(uint32_t)) )
    {
        gdprintk(XENLOG_DEBUG,
                 "unaligned/wrong-width read gpa=%"PRIpaddr" len=%u\n",
                 info->gpa, info->len);

        return IO_ABORT;
    }

    if ( !vaplic_emulate_load(curr, info->gpa, &val) )
        return IO_ABORT;

    info->data = val;

    return IO_HANDLED;
}

static enum io_state cf_check vaplic_mmio_write(struct vcpu *curr,
                                                const mmio_info_t *info)
{
    ASSERT(curr == current);

    if ( info->len != sizeof(uint32_t) ||
         !IS_ALIGNED(info->gpa, sizeof(uint32_t)) )
    {
        gdprintk(XENLOG_DEBUG,
                 "unaligned/wrong-width write gpa=%"PRIpaddr" len=%u\n",
                 info->gpa, info->len);

        return IO_ABORT;
    }

    if ( !vaplic_emulate_store(curr, info->gpa, info->data) )
        return IO_ABORT;

    return IO_HANDLED;
}

static const struct mmio_handler_ops vaplic_mmio_ops = {
    .read  = vaplic_mmio_read,
    .write = vaplic_mmio_write,
};

static const struct vintc_ops vintc_ops = {
    .vcpu_init = vcpu_imsic_init,
    .vcpu_deinit = vcpu_imsic_deinit,
    /*
     * MSI delivery is the only supported mode, so the vAPLIC state to save
     * and restore is always the IMSIC one.
     */
    .ctxt_switch_from = imsic_ctxt_switch_from,
    .ctxt_switch_to = imsic_ctxt_switch_to,
    /*
     * No .move_irqs: the scheduler calls arch_move_irqs() from any pCPU, and
     * in sched_unit_migrate_finish() with the vCPU free to run on its new
     * pCPU meanwhile, while a guest interrupt file can only be moved safely
     * by the vCPU itself. It moves it on its way to the guest instead.
     */
    .enter_guest = imsic_migrate_vcpu,
};

int domain_vaplic_init(struct domain *d)
{
    struct vaplic *vaplic = xvzalloc(struct vaplic);
    unsigned int i;
    int rc;

    if ( !vaplic )
        return -ENOMEM;

    d->arch.vintc = &vaplic->vintc;
    d->arch.vintc->ops = &vintc_ops;
    d->arch.vintc->init_ops = &init_ops;

    vaplic->regs.domaincfg = APLIC_DOMAINCFG_RO | APLIC_DOMAINCFG_DM;

    /*
     * APLIC source 0 is reserved; sources are numbered
     * 1..guest_aplic_num_sources and used directly as indices into used_irqs.
     * Size the bitmap to guest_aplic_num_sources + 1 so the highest source has
     * a valid slot (index 0 stays unused). Without the +1,
     * vintc_reserve_virq() can't record the top source, so
     * domain_vintc_deinit() never releases it.
     */
    d->arch.vintc->nr_virqs = guest_aplic_num_sources + 1;

    /* Slot 0 is unused: APLIC source numbering starts at 1 (see used_irqs). */
    vaplic->regs.sources = xvzalloc_array(struct vaplic_source,
                                          d->arch.vintc->nr_virqs);
    if ( !vaplic->regs.sources )
    {
        domain_vaplic_deinit(d);

        return -ENOMEM;
    }

    for ( i = 0; i < d->arch.vintc->nr_virqs; i++ )
        spin_lock_init(&vaplic->regs.sources[i].lock);

    vaplic->regs_start = GUEST_APLIC_S_BASE;
    /*
     * IDC structures are used only in direct delivery mode, so the control
     * region of vAPLIC, which supports MSI delivery mode only, holds none.
     *
     * TODO: should direct delivery mode ever be supported, the region will
     * have to hold an IDC structure per vCPU, i.e. be APLIC_SIZE(d->max_vcpus)
     * here and in vaplic_make_domu_dt_node().
     */
    vaplic->regs_size = APLIC_SIZE(0);

    rc = register_mmio_handler(d, &vaplic_mmio_ops,
                               vaplic->regs_start, vaplic->regs_size);
    if ( rc )
        domain_vaplic_deinit(d);

    return rc;
}

void domain_vaplic_deinit(struct domain *d)
{
    struct vaplic *vaplic;

    if ( !d->arch.vintc )
        return;

    vaplic = to_vaplic(d);
    d->arch.vintc = NULL;
    xvfree(vaplic->regs.sources);
    xvfree(vaplic);
}
