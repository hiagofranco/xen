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
 * there: all its registers are emulated.
 *
 * A write passed through to the h/w can thus affect only the h/w sources of
 * the domain: a write to setip[], in_clrip[], setie[] or clrie[] is masked
 * with them, which leaves the bits of the other sources at 0, i.e. untouched,
 * and sourcecfg[] names a source checked to be one of them.
 * route_irq_to_guest() never assigns a source to more than one domain, or to
 * both a domain and Xen, and as virq == pirq, no translation is needed either.
 */
static bool vaplic_is_hw_source(const struct domain *d, unsigned int irqn)
{
    const struct vintc *vintc = d->arch.vintc;

    return (irqn < vintc->nr_virqs) &&
           test_bit(array_index_nospec(irqn, vintc->nr_virqs),
                    vintc->hw_irqs);
}

/* Whether source @irqn is assigned to @d, be it a h/w or a virtual one. */
static bool vaplic_is_assigned(const struct domain *d, unsigned int irqn)
{
    const struct vintc *vintc = d->arch.vintc;

    return (irqn < vintc->nr_virqs) &&
           test_bit(array_index_nospec(irqn, vintc->nr_virqs),
                    vintc->used_irqs);
}

/*
 * Convert a byte offset (within a SETIP/CLRIP/SETIE/CLRIE register group) to
 * a 32-bit word index, each word covering 32 interrupt sources. For SOURCECFG
 * and TARGET groups the same division also yields the interrupt number
 * directly, because those arrays store one 32-bit register per source.
 */
#define regoffset_to_word_idx(reg_val) ((reg_val) / sizeof(uint32_t))

/* Number of interrupt sources covered by a SETIP/CLRIP/SETIE/CLRIE word. */
#define IRQS_PER_WORD 32U

/*
 * Whether source @irqn of @d is active, as the guest's view of its
 * sourcecfg[] says. For a h/w source, that is what the h/w register reads.
 */
static bool vaplic_source_is_active(const struct domain *d, unsigned int irqn)
{
    const struct vaplic *vaplic = to_vaplic(d);
    uint32_t sourcecfg = read_atomic(&vaplic->regs.sources[irqn].sourcecfg);

    /*
     * vAPLIC is a leaf domain: a write with D set zeroes the register (see
     * vaplic_sourcecfg_write()).
     */
    ASSERT(!(sourcecfg & APLIC_SOURCECFG_D));

    return MASK_EXTR(sourcecfg, APLIC_SOURCECFG_SM) !=
           APLIC_SOURCECFG_SM_INACTIVE;
}

/* Whether virtual source @src is level-sensitive. */
static bool vaplic_is_level(const struct vaplic_source *src)
{
    unsigned int sm = MASK_EXTR(src->sourcecfg, APLIC_SOURCECFG_SM);

    return (sm == APLIC_SOURCECFG_SM_LEVEL_HIGH) ||
           (sm == APLIC_SOURCECFG_SM_LEVEL_LOW);
}

/*
 * The rectified input value of virtual source @src: the level of its line,
 * inverted for an Edge0 or Level0 source, and zero for an inactive or
 * Detached one.
 */
static bool vaplic_rectified_input(const struct vaplic_source *src)
{
    switch ( MASK_EXTR(src->sourcecfg, APLIC_SOURCECFG_SM) )
    {
    case APLIC_SOURCECFG_SM_EDGE_RISE:
    case APLIC_SOURCECFG_SM_LEVEL_HIGH:
        return src->level;

    case APLIC_SOURCECFG_SM_EDGE_FALL:
    case APLIC_SOURCECFG_SM_LEVEL_LOW:
        return !src->level;

    default:
        return false;
    }
}

/* Emulate a read of target[@irqn]. */
static uint32_t vaplic_target_read(const struct domain *d, unsigned int irqn)
{
    struct vaplic *vaplic = to_vaplic(d);
    struct vaplic_source *src;
    unsigned long flags;
    uint32_t val = 0;

    /* target[] of a source not assigned to @d is read-only zero. */
    if ( !vaplic_is_assigned(d, irqn) )
        return 0;

    irqn = array_index_nospec(irqn, vaplic->vintc.nr_virqs);
    src = &vaplic->regs.sources[irqn];

    /*
     * src->lock makes the read atomic as a whole, against a write to
     * target[] which changes both this copy and the h/w register.
     */
    spin_lock_irqsave(&src->lock, flags);

    /* target[i] is read-only zero while source i is inactive. */
    if ( vaplic_source_is_active(d, irqn) )
        val = src->target;

    spin_unlock_irqrestore(&src->lock, flags);

    return val;
}

/* Emulate a write of @value to target[@irqn]. */
static void vaplic_target_write(const struct domain *d, unsigned int irqn,
                                uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    const struct vcpu *target_vcpu;
    struct vaplic_source *src;
    unsigned long flags;

    if ( !vaplic_is_assigned(d, irqn) )
        return;

    irqn = array_index_nospec(irqn, d->arch.vintc->nr_virqs);

    /* target[i] is read-only zero while source i is inactive. */
    if ( !vaplic_source_is_active(d, irqn) )
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
     * What ends up in the h/w register of a h/w source is Xen's own value
     * anyway: aplic_msi_target_gen() takes only EIID from it.
     */
    value &= APLIC_TARGET_HART_IDX | APLIC_TARGET_EIID;

    src = &vaplic->regs.sources[irqn];

    spin_lock_irqsave(&src->lock, flags);

    write_atomic(&src->target, value);

    /* Only a h/w source has a h/w register to update. */
    if ( vaplic_is_hw_source(d, irqn) )
    {
        struct vimsic_state *vimsic_state = target_vcpu->arch.vimsic_state;

        /* IRQs are already off under src->lock. */
        read_lock(&vimsic_state->vsfile_lock);

        /*
         * The h/w register names the hart and the guest interrupt file MSIs
         * are sent to, and there is none of either until the target vCPU is
         * given a h/w guest interrupt file, so until then only the stored
         * copy is updated. imsic_vsfile_attach() writes it to the h/w, through
         * vaplic_reconfigure_target(), once there is such a file.
         *
         * TODO: once s/w guest interrupt files are supported, a vCPU using
         * one still needs its MSIs, so the h/w register will have to be
         * written for it too, pointing at an interrupt file Xen receives
         * them in on the vCPU's behalf.
         */
        if ( vimsic_state->vsfile_cpu != CPU_NONE )
            aplic_hw_write_reg(offsetof(struct aplic_regs, target[irqn - 1]),
                               aplic_msi_target_gen(target_vcpu, value));

        read_unlock(&vimsic_state->vsfile_lock);
    }

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

/* Number of setie[] words covering the sources of @d. */
static unsigned int vaplic_nr_words(const struct domain *d)
{
    return DIV_ROUND_UP(d->arch.vintc->nr_virqs, IRQS_PER_WORD);
}

/* Word @word_idx, in the setie[] layout, of @bitmap of @d's sources. */
static uint32_t vaplic_bitmap_word(const struct domain *d,
                                   const unsigned long *bitmap,
                                   unsigned int word_idx)
{
    unsigned int nr_words = vaplic_nr_words(d);
    unsigned int first_bit;

    if ( word_idx >= nr_words )
    {
        gdprintk(XENLOG_DEBUG, "incorrect word_idx(%u)\n", word_idx);

        return 0;
    }

    first_bit = array_index_nospec(word_idx, nr_words) * IRQS_PER_WORD;

    return bitmap[first_bit / BITS_PER_LONG] >> (first_bit % BITS_PER_LONG);
}

/* The h/w sources of @d in word @word_idx, in the setie[] layout. */
static uint32_t vaplic_hw_sources(const struct domain *d,
                                  unsigned int word_idx)
{
    return vaplic_bitmap_word(d, d->arch.vintc->hw_irqs, word_idx);
}

/* The virtual sources of @d in word @word_idx, in the setie[] layout. */
static uint32_t vaplic_virtual_sources(const struct domain *d,
                                       unsigned int word_idx)
{
    return vaplic_bitmap_word(d, d->arch.vintc->used_irqs, word_idx) &
           ~vaplic_bitmap_word(d, d->arch.vintc->hw_irqs, word_idx);
}

/* The bits of @mask, a subset of setie[@word_idx], of the active sources. */
static uint32_t vaplic_active_sources(const struct domain *d,
                                      unsigned int word_idx, uint32_t mask)
{
    uint32_t active = 0;

    for_each_set_bit ( bit, mask )
        if ( vaplic_source_is_active(d, word_idx * IRQS_PER_WORD + bit) )
            active |= BIT(bit, U);

    return active;
}

/* Emulate a read of sourcecfg[@irqn]. */
static uint32_t vaplic_sourcecfg_read(const struct domain *d, unsigned int irqn)
{
    struct vaplic *vaplic = to_vaplic(d);
    struct vaplic_source *src;
    unsigned long flags;
    uint32_t val;

    /*
     * A source not assigned to @d appears not to be implemented, and
     * sourcecfg[] of such a source is read-only zero.
     */
    if ( !vaplic_is_assigned(d, irqn) )
        return 0;

    irqn = array_index_nospec(irqn, d->arch.vintc->nr_virqs);
    src = &vaplic->regs.sources[irqn];

    /*
     * src->lock makes the read atomic as a whole, against a write to
     * sourcecfg[] which changes the h/w register, this copy and the enable
     * and pending bits.
     */
    spin_lock_irqsave(&src->lock, flags);
    val = src->sourcecfg;
    spin_unlock_irqrestore(&src->lock, flags);

    return val;
}

/* Emulate a write of @value to sourcecfg[@irqn]. */
static void vaplic_sourcecfg_write(const struct domain *d, unsigned int irqn,
                                   uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    unsigned int offset = offsetof(struct aplic_regs, sourcecfg[irqn - 1]);
    struct vaplic_source *src;
    unsigned long flags;

    if ( !vaplic_is_assigned(d, irqn) )
        return;

    /*
     * vAPLIC is a leaf domain: it has no child domain a source could be
     * delegated to. With D = 1, the low bits of the value would hold a
     * Child Index rather than SM, and for a leaf domain the AIA spec makes
     * such a write set the entire register to zero instead.
     */
    if ( value & APLIC_SOURCECFG_D )
    {
        gdprintk(XENLOG_DEBUG, "sourcecfg[%u] write with D set, zeroing it\n",
                 irqn);

        value = 0;
    }

    /*
     * SM is the only field left, the other bits are reserved and read as
     * zero, so ignore what a guest writes to them.
     */
    value &= APLIC_SOURCECFG_SM;

    irqn = array_index_nospec(irqn, d->arch.vintc->nr_virqs);
    src = &vaplic->regs.sources[irqn];

    spin_lock_irqsave(&src->lock, flags);

    /*
     * SM is WARL. For a h/w source, it is the h/w which substitutes a legal
     * value for an illegal one, so the guest's view is read back from the
     * h/w register. For a virtual source, it is vAPLIC which has to: the
     * reserved values 0x2 and 0x3 are taken as Inactive, and any other value
     * is a legal one and is kept as written.
     */
    if ( vaplic_is_hw_source(d, irqn) )
    {
        aplic_hw_write_reg(offset, value);
        value = aplic_hw_read_reg(offset);
    }
    else if ( value == 0x2 || value == 0x3 )
        value = APLIC_SOURCECFG_SM_INACTIVE;

    /*
     * vaplic->lock keeps a concurrent write to an enable or pending register
     * from setting a bit of the source while it is made inactive: such a
     * write sees either the source still active, and then its bit is cleared
     * below afterwards, or the source inactive, and then it doesn't set the
     * bit. IRQs are already off under src->lock.
     */
    spin_lock(&vaplic->lock);

    write_atomic(&src->sourcecfg, value);

    /*
     * According to the AIA spec:
     *   Whenever interrupt source i is inactive in an interrupt domain, the
     *   corresponding interrupt-pending and interrupt-enable bits within the
     *   domain are read-only zeros, [...]. If source i is changed from
     *   inactive to an active mode, the interrupt source's pending and
     *   enable bits remain zeros, unless set automatically [...].
     *
     * So making the source inactive clears them. For a h/w source, the h/w
     * clears its own bits and regs.ie[] follows; regs.ip[] holds none of
     * them.
     */
    if ( !vaplic_source_is_active(d, irqn) )
    {
        write_atomic(&vaplic->regs.ie[irqn / IRQS_PER_WORD],
                     vaplic->regs.ie[irqn / IRQS_PER_WORD] &
                     ~BIT(irqn % IRQS_PER_WORD, U));
        vaplic->regs.ip[irqn / IRQS_PER_WORD] &= ~BIT(irqn % IRQS_PER_WORD, U);
    }
    /*
     * The pending bit of a level-sensitive source is cleared whenever its
     * rectified input value is low, which the new source mode may make it.
     * The AIA spec permits a write to sourcecfg[] not to set the pending bit
     * when the value is high under the new source mode, so it is left alone
     * then. The h/w takes care of all that for a h/w source.
     */
    else if ( !vaplic_is_hw_source(d, irqn) && vaplic_is_level(src) &&
              !vaplic_rectified_input(src) )
        vaplic->regs.ip[irqn / IRQS_PER_WORD] &= ~BIT(irqn % IRQS_PER_WORD, U);

    spin_unlock(&vaplic->lock);

    /* No MSI to send: this write can only clear the enable and pending bits. */

    spin_unlock_irqrestore(&src->lock, flags);
}

/*
 * Forward by MSI the pending and enabled virtual sources among @srcs, word
 * @word_idx in the setip[] layout.
 */
static void vaplic_forward_msis(const struct domain *d, unsigned int word_idx,
                                uint32_t srcs)
{
    struct vaplic *vaplic = to_vaplic(d);
    uint32_t ready;

    ASSERT(spin_is_locked(&vaplic->lock));

    /*
     * According to the AIA spec:
     *   An MSI is sent for a specific source only when the source's
     *   corresponding pending and enable bits are both one and the IE field
     *   of register domaincfg is also one.
     */
    if ( !(vaplic->regs.domaincfg & APLIC_DOMAINCFG_IE) )
        return;

    /* The h/w forwards the MSIs of the h/w sources by itself. */
    ready = srcs & vaplic_virtual_sources(d, word_idx) &
            vaplic->regs.ie[word_idx] & vaplic->regs.ip[word_idx];

    for_each_set_bit ( bit, ready )
    {
        const struct vaplic_source *src =
            &vaplic->regs.sources[word_idx * IRQS_PER_WORD + bit];
        /*
         * src->lock can't be taken under vaplic->lock (see the lock order in
         * struct vaplic), so target is read without it, but only once, so
         * that the hart index and the EIID come from the same value. An MSI
         * racing with a write to target[] may reach the old target, which the
         * AIA spec allows an APLIC too.
         */
        uint32_t target = read_atomic(&src->target);
        const struct vcpu *v = vaplic_target_decode_vcpu(d, target);

        if ( v )
            vcpu_imsic_inject(v, MASK_EXTR(target, APLIC_TARGET_EIID));
    }

    /*
     * According to the AIA spec:
     *   If and when an MSI is sent, the source's interrupt pending bit is
     *   cleared.
     */
    vaplic->regs.ip[word_idx] &= ~ready;
}

/* Emulate a read of setie[@word_idx]. */
static uint32_t vaplic_setie_read(const struct domain *d, unsigned int word_idx)
{
    const struct vaplic *vaplic = to_vaplic(d);
    uint32_t srcs = vaplic_hw_sources(d, word_idx) |
                    vaplic_virtual_sources(d, word_idx);

    if ( !srcs )
        return 0;

    word_idx = array_index_nospec(word_idx, vaplic_nr_words(d));

    /*
     * A word of regs.ie[] is updated in one go, so a read sees either all of
     * a write to setie[] or none of it, as with a single register in the h/w.
     */
    return read_atomic(&vaplic->regs.ie[word_idx]) & srcs;
}

/* Emulate a write of @value to setie[@word_idx]. */
static void vaplic_setie_write(const struct domain *d, unsigned int word_idx,
                               uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    uint32_t hw_srcs = vaplic_hw_sources(d, word_idx);
    unsigned long flags;

    value &= hw_srcs | vaplic_virtual_sources(d, word_idx);
    if ( !value )
        return;

    word_idx = array_index_nospec(word_idx, vaplic_nr_words(d));

    spin_lock_irqsave(&vaplic->lock, flags);

    /* The enable bit of an inactive source is read-only zero. */
    value = vaplic_active_sources(d, word_idx, value);

    /* Ahead of the h/w, so a guest which gets the MSI reads the bit set. */
    write_atomic(&vaplic->regs.ie[word_idx],
                 vaplic->regs.ie[word_idx] | value);

    /* While domaincfg.IE is 0, the h/w bits are kept clear. */
    if ( (value & hw_srcs) && (vaplic->regs.domaincfg & APLIC_DOMAINCFG_IE) )
        aplic_hw_write_reg(offsetof(struct aplic_regs, setie[word_idx]),
                           value & hw_srcs);

    /* A pending virtual source this write has enabled gets its MSI now. */
    vaplic_forward_msis(d, word_idx, value & ~hw_srcs);

    spin_unlock_irqrestore(&vaplic->lock, flags);
}

/* Emulate a write of @value to clrie[@word_idx]. */
static void vaplic_clrie_write(const struct domain *d, unsigned int word_idx,
                               uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    uint32_t hw_srcs = vaplic_hw_sources(d, word_idx);
    uint32_t virt_srcs = vaplic_virtual_sources(d, word_idx);
    unsigned long flags;

    value &= hw_srcs | virt_srcs;
    if ( !value )
        return;

    word_idx = array_index_nospec(word_idx, vaplic_nr_words(d));

    spin_lock_irqsave(&vaplic->lock, flags);

    /* While domaincfg.IE is 0, the h/w bits are clear already. */
    if ( (value & hw_srcs) && (vaplic->regs.domaincfg & APLIC_DOMAINCFG_IE) )
        aplic_hw_write_reg(offsetof(struct aplic_regs, clrie[word_idx]),
                           value & hw_srcs);

    write_atomic(&vaplic->regs.ie[word_idx],
                 vaplic->regs.ie[word_idx] & ~value);

    spin_unlock_irqrestore(&vaplic->lock, flags);
}

static uint32_t vaplic_domaincfg_read(const struct domain *d)
{
    struct vaplic *vaplic = to_vaplic(d);
    unsigned long flags;
    uint32_t val;

    /*
     * vaplic->lock makes the read atomic as a whole, against a write to
     * domaincfg which changes IE and the h/w enable bits of the h/w sources.
     */
    spin_lock_irqsave(&vaplic->lock, flags);
    val = vaplic->regs.domaincfg;
    spin_unlock_irqrestore(&vaplic->lock, flags);

    return val;
}

static void vaplic_domaincfg_write(const struct domain *d, uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    uint32_t domaincfg = APLIC_DOMAINCFG_RO | APLIC_DOMAINCFG_DM |
                         (value & APLIC_DOMAINCFG_WMASK);
    unsigned long flags;
    unsigned int i;
    uint32_t old;

    spin_lock_irqsave(&vaplic->lock, flags);

    old = vaplic->regs.domaincfg;

    /* vaplic_forward_msis() below has to see the new IE. */
    vaplic->regs.domaincfg = domaincfg;

    /* Only a change of IE needs more than storing the new value. */
    if ( (old ^ domaincfg) & APLIC_DOMAINCFG_IE )
    {
        for ( i = 0; i < vaplic_nr_words(d); i++ )
        {
            uint32_t hw_srcs = vaplic_hw_sources(d, i);

            /* The pending and enabled virtual sources get their MSIs now. */
            vaplic_forward_msis(d, i, ~0U);

            if ( !hw_srcs )
                continue;

            if ( domaincfg & APLIC_DOMAINCFG_IE )
                aplic_hw_write_reg(offsetof(struct aplic_regs, setie[i]),
                                   vaplic->regs.ie[i] & hw_srcs);
            else
                aplic_hw_write_reg(offsetof(struct aplic_regs, clrie[i]),
                                   hw_srcs);
        }
    }

    spin_unlock_irqrestore(&vaplic->lock, flags);
}

/*
 * The bits of @srcs, virtual sources in setip[@word_idx], whose pending bits
 * a write to setip[] can set.
 */
static uint32_t vaplic_pendable_sources(const struct domain *d,
                                        unsigned int word_idx, uint32_t srcs)
{
    const struct vaplic *vaplic = to_vaplic(d);
    uint32_t pendable = 0;

    ASSERT(spin_is_locked(&vaplic->lock));

    for_each_set_bit ( bit, srcs )
    {
        unsigned int irqn = word_idx * IRQS_PER_WORD + bit;
        const struct vaplic_source *src = &vaplic->regs.sources[irqn];

        /*
         * The pending bit of an inactive source is read-only zero. For a
         * level-sensitive one in MSI delivery mode, according to the AIA spec:
         *   The pending bit may also be set by a relevant write to a setip or
         *   setipnum register when the rectified input value is high, but not
         *   when the rectified input value is low.
         */
        if ( vaplic_source_is_active(d, irqn) &&
             (!vaplic_is_level(src) || vaplic_rectified_input(src)) )
            pendable |= BIT(bit, U);
    }

    return pendable;
}

/*
 * Emulate a read of setip[@word_idx] (@setip) or of in_clrip[@word_idx]: the
 * pending bits or the rectified input values of the sources.
 */
static uint32_t vaplic_ip_read(const struct domain *d, unsigned int word_idx,
                               bool setip)
{
    struct vaplic *vaplic = to_vaplic(d);
    uint32_t hw_srcs = vaplic_hw_sources(d, word_idx);
    uint32_t virt_srcs = vaplic_virtual_sources(d, word_idx);
    unsigned long flags;
    uint32_t val = 0;

    if ( !hw_srcs && !virt_srcs )
        return 0;

    word_idx = array_index_nospec(word_idx, vaplic_nr_words(d));

    /*
     * vaplic->lock makes the read atomic as a whole, against a write to
     * setip[] or in_clrip[] which changes both kinds of bits.
     */
    spin_lock_irqsave(&vaplic->lock, flags);

    if ( hw_srcs )
        val = aplic_hw_read_reg(setip ? offsetof(struct aplic_regs,
                                                 setip[word_idx])
                                      : offsetof(struct aplic_regs,
                                                 in_clrip[word_idx])) &
              hw_srcs;

    if ( setip )
        val |= vaplic->regs.ip[word_idx] & virt_srcs;
    else
    {
        for_each_set_bit ( bit, virt_srcs )
        {
            const struct vaplic_source *src =
                &vaplic->regs.sources[word_idx * IRQS_PER_WORD + bit];

            if ( vaplic_rectified_input(src) )
                val |= BIT(bit, U);
        }
    }

    spin_unlock_irqrestore(&vaplic->lock, flags);

    return val;
}

/*
 * Emulate a write of @value to setip[@word_idx] (@setip) or to
 * in_clrip[@word_idx].
 */
static void vaplic_ip_write(const struct domain *d, unsigned int word_idx,
                            uint32_t value, bool setip)
{
    struct vaplic *vaplic = to_vaplic(d);
    uint32_t hw_srcs = vaplic_hw_sources(d, word_idx);
    uint32_t virt_srcs = vaplic_virtual_sources(d, word_idx);
    unsigned long flags;

    value &= hw_srcs | virt_srcs;
    if ( !value )
        return;

    word_idx = array_index_nospec(word_idx, vaplic_nr_words(d));

    spin_lock_irqsave(&vaplic->lock, flags);

    if ( value & hw_srcs )
        aplic_hw_write_reg(setip ? offsetof(struct aplic_regs, setip[word_idx])
                                 : offsetof(struct aplic_regs,
                                            in_clrip[word_idx]),
                           value & hw_srcs);

    if ( setip )
    {
        vaplic->regs.ip[word_idx] |=
            vaplic_pendable_sources(d, word_idx, value & virt_srcs);

        /* An enabled source this write has made pending gets its MSI now. */
        vaplic_forward_msis(d, word_idx, value & virt_srcs);
    }
    else
        vaplic->regs.ip[word_idx] &= ~(value & virt_srcs);

    spin_unlock_irqrestore(&vaplic->lock, flags);
}

/*
 * Set the level of the line of virtual source @virq of @d, as the emulator of
 * the device in Xen driving it does.
 */
static void cf_check vaplic_set_irq_level(const struct domain *d,
                                          unsigned int virq, bool level)
{
    struct vaplic *vaplic = to_vaplic(d);
    struct vaplic_source *src;
    unsigned int word_idx;
    unsigned long flags;
    uint32_t bit;
    bool was_high;

    /* Only a virtual source of @d has its line driven by an emulator. */
    if ( !vaplic_is_assigned(d, virq) || vaplic_is_hw_source(d, virq) )
    {
        ASSERT_UNREACHABLE();
        return;
    }

    virq = array_index_nospec(virq, d->arch.vintc->nr_virqs);
    src = &vaplic->regs.sources[virq];
    word_idx = virq / IRQS_PER_WORD;
    bit = BIT(virq % IRQS_PER_WORD, U);

    spin_lock_irqsave(&vaplic->lock, flags);

    was_high = vaplic_rectified_input(src);
    src->level = level;

    /*
     * According to the AIA spec, in MSI delivery mode a low-to-high
     * transition of the rectified input value sets the pending bit of an
     * edge- as well as of a level-sensitive source, and the pending bit of a
     * level-sensitive one is cleared whenever the value is low. Neither
     * happens to an inactive or a Detached source, whose rectified input
     * value is always low.
     */
    if ( !was_high && vaplic_rectified_input(src) )
        vaplic->regs.ip[word_idx] |= bit;
    else if ( vaplic_is_level(src) && !vaplic_rectified_input(src) )
        vaplic->regs.ip[word_idx] &= ~bit;

    /* An enabled source the new level has made pending gets its MSI now. */
    vaplic_forward_msis(d, word_idx, bit);

    spin_unlock_irqrestore(&vaplic->lock, flags);
}

static uint32_t vaplic_genmsi_read(const struct domain *d)
{
    /*
     * Busy always reads as zero, as the MSI is sent before the write to genmsi
     * completes.
     *
     * No lock is needed: a read racing with a write may return the new value
     * before the MSI has been sent, which looks the same to the guest as an
     * MSI of an APLIC which hasn't taken effect at the IMSIC yet.
     */
    return read_atomic(&to_vaplic(d)->regs.genmsi);
}

static void vaplic_genmsi_write(const struct domain *d, uint32_t value)
{
    struct vaplic *vaplic = to_vaplic(d);
    const struct vcpu *v;
    unsigned long flags;

    /*
     * Hart Index and EIID behave as in a target register, Busy is read-only
     * and all the other bits are reserved.
     */
    value &= APLIC_GENMSI_HART_IDX | APLIC_GENMSI_EIID;

    v = domain_vcpu(d, MASK_EXTR(value, APLIC_GENMSI_HART_IDX));
    if ( !v )
    {
        gdprintk(XENLOG_ERR, "Invalid vCPU id in genmsi\n");

        /* Ignore such writes */
        return;
    }

    /*
     * According to the AIA spec:
     *   All MSIs previously sent from this APLIC to the same hart must be
     *   visible at the hart's IMSIC before the extempore MSI becomes visible
     *   at the hart's IMSIC.
     *
     * The physical genmsi can't send an MSI to a guest interrupt file, so
     * wait for the MSIs already sent to @v's file to arrive there.
     *
     * The barrier may need an IPI, which takes IRQs enabled, while a trap
     * taken from a guest is handled with them disabled. Enable them for the
     * time of the barrier: no lock is held here, and the CSRs describing the
     * trap have been read already.
     */
    ASSERT(!local_irq_is_enabled());
    local_irq_enable();
    vcpu_imsic_aplic_barrier(v);
    local_irq_disable();

    spin_lock_irqsave(&vaplic->lock, flags);

    write_atomic(&vaplic->regs.genmsi, value);

    if ( vcpu_imsic_inject(v, MASK_EXTR(value, APLIC_GENMSI_EIID)) )
        gdprintk(XENLOG_DEBUG, "genmsi: %pv has no h/w interrupt file\n", v);

    spin_unlock_irqrestore(&vaplic->lock, flags);
}

static bool vaplic_emulate_load(const struct vcpu *curr, paddr_t addr,
                                uint32_t *out)
{
    const struct domain *currd = curr->domain;
    const struct vaplic *vaplic = to_vaplic(currd);
    const unsigned int offset = addr - vaplic->regs_start;
    unsigned int i;

    ASSERT(curr == current);

    switch ( offset )
    {
    case APLIC_DOMAINCFG:
        *out = vaplic_domaincfg_read(currd);

        return true;

    case APLIC_SOURCECFG_BASE ... APLIC_SOURCECFG_LAST:
        /*
         * Look at vaplic_emulate_store() for explanation why APLIC_DOMAINCFG
         * is subtracted.
         */
        i = regoffset_to_word_idx(offset - APLIC_DOMAINCFG);
        *out = vaplic_sourcecfg_read(currd, i);

        return true;

    case APLIC_SETIP_BASE ... APLIC_SETIP_LAST:
    case APLIC_CLRIP_BASE ... APLIC_CLRIP_LAST:
        i = regoffset_to_word_idx(offset & APLIC_SETCLR_OFFSET_MASK);
        *out = vaplic_ip_read(currd, i, offset <= APLIC_SETIP_LAST);

        return true;

    case APLIC_SETIE_BASE ... APLIC_SETIE_LAST:
        i = regoffset_to_word_idx(offset & APLIC_SETCLR_OFFSET_MASK);
        *out = vaplic_setie_read(currd, i);

        return true;

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

        *out = vaplic_target_read(currd, i);

        return true;

    case APLIC_GENMSI:
        *out = vaplic_genmsi_read(currd);

        return true;

    default:
        gdprintk(XENLOG_WARNING, "Unhandled APLIC read at offset %#x\n",
                 offset);

        return false;
    }
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
        vaplic_domaincfg_write(currd, value);

        return true;

    case APLIC_SOURCECFG_BASE ... APLIC_SOURCECFG_LAST:
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
        vaplic_sourcecfg_write(currd,
                               regoffset_to_word_idx(offset - APLIC_DOMAINCFG),
                               value);

        return true;

    case APLIC_SETIE_BASE ... APLIC_SETIE_LAST:
        vaplic_setie_write(currd,
                           regoffset_to_word_idx(offset &
                                                 APLIC_SETCLR_OFFSET_MASK),
                           value);

        return true;

    case APLIC_CLRIE_BASE ... APLIC_CLRIE_LAST:
        vaplic_clrie_write(currd,
                           regoffset_to_word_idx(offset &
                                                 APLIC_SETCLR_OFFSET_MASK),
                           value);

        return true;

    case APLIC_SETIENUM:
    case APLIC_CLRIENUM:
        if ( !vaplic_is_assigned(currd, value) )
            return true;

        if ( offset == APLIC_SETIENUM )
            vaplic_setie_write(currd, value / IRQS_PER_WORD,
                               BIT(value % IRQS_PER_WORD, U));
        else
            vaplic_clrie_write(currd, value / IRQS_PER_WORD,
                               BIT(value % IRQS_PER_WORD, U));

        return true;

    case APLIC_SETIP_BASE ... APLIC_SETIP_LAST:
    case APLIC_CLRIP_BASE ... APLIC_CLRIP_LAST:
        vaplic_ip_write(currd,
                        regoffset_to_word_idx(offset &
                                              APLIC_SETCLR_OFFSET_MASK),
                        value, offset <= APLIC_SETIP_LAST);

        return true;

    case APLIC_SETIPNUM:
    case APLIC_CLRIPNUM:
    case APLIC_SETIPNUM_LE:
        if ( vaplic_is_assigned(currd, value) )
            vaplic_ip_write(currd, value / IRQS_PER_WORD,
                            BIT(value % IRQS_PER_WORD, U),
                            offset != APLIC_CLRIPNUM);

        return true;

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

    case APLIC_GENMSI:
        vaplic_genmsi_write(currd, value);

        return true;

    default:
        gdprintk(XENLOG_WARNING,
                 "Unhandled APLIC write at offset %#x (value %#x)\n",
                 offset, value);

        return false;
    }
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
    .set_irq_level = vaplic_set_irq_level,
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

    vaplic->regs.ie = xvzalloc_array(uint32_t, vaplic_nr_words(d));
    vaplic->regs.ip = xvzalloc_array(uint32_t, vaplic_nr_words(d));
    if ( !vaplic->regs.ie || !vaplic->regs.ip )
    {
        domain_vaplic_deinit(d);

        return -ENOMEM;
    }

    spin_lock_init(&vaplic->lock);

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
    xvfree(vaplic->regs.ip);
    xvfree(vaplic->regs.ie);
    xvfree(vaplic->regs.sources);
    xvfree(vaplic);
}
