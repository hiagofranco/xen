/* SPDX-License-Identifier: MIT */

/*
 * xen/arch/riscv/aplic.c
 *
 * RISC-V Advanced Platform-Level Interrupt Controller support
 *
 * Copyright (c) 2023-2024 Microchip.
 * Copyright (c) 2024-2025 Vates
 */

#include <xen/const.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/mm.h>
#include <xen/sections.h>
#include <xen/spinlock.h>
#include <xen/types.h>
#include <xen/vmap.h>

#include "aplic-priv.h"

#include <asm/device.h>
#include <asm/imsic.h>
#include <asm/intc.h>
#include <asm/io.h>
#include <asm/riscv_encoding.h>

static struct aplic_priv aplic = {
    .lock = SPIN_LOCK_UNLOCKED,
};

static struct intc_info __ro_after_init aplic_info = {
    .hw_variant = INTC_APLIC,
};

static bool aplic_msi_mode(void)
{
    return readl(&aplic.regs->domaincfg) & APLIC_DOMAINCFG_DM;
}

/*
 * The arrangement of IMSIC interrupt files in MMIO space follows a topology
 * defined by the RISC-V AIA specification. An IMSIC group is a set of
 * interrupt files (e.g., in a cluster or socket) co-located in memory.
 *
 * The physical address of an outgoing MSI is calculated by bitwise ORing a
 * Base Physical Page Number (Base PPN) with the group number (g), the
 * hart number relative to the group (h) and, for a supervisor-level
 * interrupt domain, the Guest Index:
 *
 *   ( Base PPN | (g << (HHXS + 12)) | (h << LHXS) | Guest Index ) << 12
 *
 * where Base PPN, HHXS, LHXS, HHXW and LHXW come from the {m,s}msiaddrcfg[h]
 * registers of the interrupt domain that sends the MSI:
 *
 * XLEN-1       HHXS+24          LHXS+12          12          0
 * |            |                |                |           |
 * ------------------------------------------------------------
 * |xxxx|   g   |xxxxxxxx|   h   |xxxx|Guest Index|     0     |
 * ------------------------------------------------------------
 *
 * - g: group number.
 * - h: hart number relative to the group.
 * - xxxx: remaining Base PPN bits; each gap may be zero-width.
 * - Guest Index: selects one of the 4 KiB pages right above the hart's own
 *   supervisor-level file, i.e. it starts at bit 12; LHXS must therefore be
 *   at least as large as the number of guest index bits.
 * - Bits 11:0: always zero because IMSIC files are 4 KiB page-aligned.
 *
 * For wired interrupts in MSI delivery mode (domaincfg.DM = 1), the APLIC
 * computes the MSI target address itself from the "Hart Index" field
 * (bits 31:18) of the corresponding target[i] register. This 14-bit field
 * holds both g and h:
 *
 * 13          lhxw+hhxw   lhxw       0
 * |           |           |          |
 * ------------------------------------
 * |     0     |     g     |    h     |
 * ------------------------------------
 *
 * - lhxw (Low Hart Index Width): the number of bits used for the hart number
 *   within a group.
 * - hhxw (High Hart Index Width): the number of bits used for the group
 *   number; the remaining bits of the field must be zero.
 *
 * The Guest Index isn't a part of it: for a supervisor-level interrupt domain
 * it has its own field (bits 17:12) in target[i].
 *
 * Because there are "xxxx" gaps (Base PPN bits) between the indices in the
 * physical address (depending on HHXS and LHXS), software must extract the
 * group and hart components separately and pack them into the APLIC-defined
 * Hart Index format to ensure correct MSI targeting.
 */
static unsigned long aplic_hart_index(unsigned int cpu)
{
    const struct imsic_config *imsic = imsic_get_config();
    const struct imsic_msi *msi = &imsic->msi[cpu];
    /* Low Hart Index Shift */
    unsigned int lhxs = imsic->guest_index_bits;
    /* Low Hart Index Width */
    unsigned int lhxw = imsic->hart_index_bits;
    /* High Hart Index Width */
    unsigned int hhxw = imsic->group_index_bits;
    /* High Hart Index Shift */
    unsigned int hhxs =
        imsic->group_index_shift - APLIC_xMSICFGADDR_PPN_SHIFT * 2;
    /*
     * msi->base_addr is the base of the MMIO regset this CPU's interrupt
     * files live in, and one regset can cover several harts; msi->offset
     * selects this CPU's block inside it. The hart index bits are part of
     * that offset, so both indexes have to be derived from the full address.
     */
    paddr_t target_addr = msi->base_addr + msi->offset;
    unsigned long tppn = target_addr >> APLIC_xMSICFGADDR_PPN_SHIFT;
    unsigned long g =
        (tppn >> APLIC_xMSICFGADDR_PPN_HHX_SHIFT(hhxs)) &
        APLIC_xMSICFGADDR_PPN_HHX_MASK(hhxw);
    unsigned long h =
        (tppn >> APLIC_xMSICFGADDR_PPN_LHX_SHIFT(lhxs)) &
        APLIC_xMSICFGADDR_PPN_LHX_MASK(lhxw);

    return (g << lhxw) | h;
}

static void __init aplic_init_hw_interrupts(void)
{
    unsigned int i;

    /* Disable all interrupts */
    for ( i = 0; i < ARRAY_SIZE(aplic.regs->clrie); i++)
        writel(~0U, &aplic.regs->clrie[i]);

    /* Set interrupt type and default priority for all interrupts */
    for ( i = 0; i < aplic_info.num_irqs; i++ )
    {
        writel(0, &aplic.regs->sourcecfg[i]);
        /*
         * Low bits of target register contains Interrupt Priority bits which
         * can't be zero according to AIA spec.
         * Thereby they are initialized to APLIC_TARGET_IPRIO_DEFAULT.
         */
        writel(APLIC_TARGET_IPRIO_DEFAULT, &aplic.regs->target[i]);
    }

    writel(APLIC_DOMAINCFG_IE | APLIC_DOMAINCFG_DM, &aplic.regs->domaincfg);
}

static int __init cf_check aplic_init(void)
{
    dt_phandle imsic_phandle;
    const __be32 *prop;
    uint64_t size, paddr;
    const struct dt_device_node *imsic_node;
    const struct dt_device_node *node = aplic_info.node;
    int rc;

    /* Check for associated imsic node */
    if ( !dt_property_read_u32(node, "msi-parent", &imsic_phandle) )
        panic("%s: IDC mode not supported\n", node->full_name);

    imsic_node = dt_find_node_by_phandle(imsic_phandle);
    if ( !imsic_node )
        panic("%s: unable to find IMSIC node\n", node->full_name);

    rc = imsic_init(imsic_node);
    if ( rc == IRQ_M_EXT )
        /* Machine mode imsic node, ignore this aplic node */
        return 0;

    if ( rc )
        panic("%s: Failed to initialize IMSIC\n", node->full_name);

    /* Find out number of interrupt sources */
    if ( !dt_property_read_u32(node, "riscv,num-sources",
                               &aplic_info.num_irqs) )
        panic("%s: failed to get number of interrupt sources\n",
              node->full_name);

    guest_aplic_num_sources = min(GUEST_APLIC_MAX_SOURCES, aplic_info.num_irqs);

    if ( aplic_info.num_irqs > ARRAY_SIZE(aplic.regs->sourcecfg) )
        aplic_info.num_irqs = ARRAY_SIZE(aplic.regs->sourcecfg);

    prop = dt_get_property(node, "reg", NULL);
    dt_get_range(&prop, node, &paddr, &size);
    if ( !paddr )
        panic("%s: first MMIO resource not found\n", node->full_name);

    if ( !IS_ALIGNED(paddr, KB(4)) )
        panic("%s: paddr of memory-mapped control region should be 4Kb aligned:%#lx\n",
              __func__, paddr);

    if ( !IS_ALIGNED(size, KB(4)) || (size < KB(16)) )
        panic("%s: control region size must be >= 16KB and 4KB-aligned:%#lx\n",
              __func__, size);

    aplic.paddr_start = paddr;
    aplic.size = size;

    aplic.regs = ioremap(paddr, size);
    if ( !aplic.regs )
        panic("%s: unable to map\n", node->full_name);

    /* Setup initial state APLIC interrupts */
    aplic_init_hw_interrupts();

    return 0;
}

static void cf_check aplic_irq_enable(struct irq_desc *desc)
{
    /*
     * TODO: Currently, APLIC is supported only with MSI interrupts.
     *       If APLIC without MSI interrupts is required in the future,
     *       this function will need to be updated accordingly.
     */
    ASSERT(aplic_msi_mode());

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock(&aplic.lock);

    desc->status &= ~IRQ_DISABLED;
    /*
     * wmb() (fence ow,ow) orders the ->status memory write (w) before the
     * CSR write inside imsic_irq_enable() (device output, o on RISC-V).
     * arch_lock_release_barrier() uses fence rw,rw which does not cover
     * device output (o), so wmb() is required to close that gap.
     */
    wmb();

    /* Enable interrupt in IMSIC */
    imsic_irq_enable(desc->irq);

    /* Enable interrupt in APLIC */
    writel(desc->irq, &aplic.regs->setienum);

    spin_unlock(&aplic.lock);
}

static void cf_check aplic_irq_disable(struct irq_desc *desc)
{
    /*
     * TODO: Currently, APLIC is supported only with MSI interrupts.
     *       If APLIC without MSI interrupts is required in the future,
     *       this function will need to be updated accordingly.
     */
    ASSERT(aplic_msi_mode());

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock(&aplic.lock);

    /* Disable interrupt in APLIC */
    writel(desc->irq, &aplic.regs->clrienum);

    /* Disable interrupt in IMSIC */
    imsic_irq_disable(desc->irq);
    /*
     * wmb() (fence ow,ow) ensures the CSR write (device output, o) inside
     * imsic_irq_disable() is globally visible before ->status is marked
     * IRQ_DISABLED. imsic_irq_disable()'s spin_unlock uses fence rw,rw
     * which does not order device output (o) writes before subsequent
     * memory writes (w), so an explicit wmb() is needed here.
     */
    wmb();

    desc->status |= IRQ_DISABLED;

    spin_unlock(&aplic.lock);
}

static unsigned int cf_check aplic_irq_startup(struct irq_desc *desc)
{
    aplic_irq_enable(desc);

    return 0;
}

static unsigned int aplic_get_cpu_from_mask(const cpumask_t *cpumask)
{
    cpumask_t mask;

    cpumask_and(&mask, cpumask, &cpu_online_map);

    return cpumask_any(&mask);
}

static void cf_check aplic_set_irq_affinity(struct irq_desc *desc, const cpumask_t *mask)
{
    unsigned int cpu = aplic_get_cpu_from_mask(mask);
    uint32_t value;

    /*
     * TODO: Currently, APLIC is supported only with MSI interrupts.
     *       If APLIC without MSI interrupts is required in the future,
     *       this function will need to be updated accordingly.
     */
    ASSERT(readl(&aplic.regs->domaincfg) & APLIC_DOMAINCFG_DM);

    ASSERT(!cpumask_empty(mask));

    ASSERT(spin_is_locked(&desc->lock));

    /*
     * desc->irq is used as the EIID as-is, which is only valid for APLIC
     * sources: they are numbered 1..num_irqs, with at most ARRAY_SIZE(target).
     */
    BUILD_BUG_ON(ARRAY_SIZE(aplic.regs->target) >
                 MASK_EXTR(~0U, APLIC_TARGET_EIID));
    ASSERT(desc->irq && desc->irq <= aplic_info.num_irqs);

    value = MASK_INSR(aplic_hart_index(cpu), APLIC_TARGET_HART_IDX) |
            desc->irq;

    spin_lock(&aplic.lock);

    writel(value, &aplic.regs->target[desc->irq - 1]);

    spin_unlock(&aplic.lock);
}

static void cf_check aplic_handle_interrupt(struct cpu_user_regs *regs)
{
    /* Disable to avoid more external interrupts */
    csr_clear(CSR_SIE, BIT(IRQ_S_EXT, UL));

    /* Clear the pending bit */
    csr_clear(CSR_SIP, BIT(IRQ_S_EXT, UL));

    /* Dispatch the interrupt */
    do_IRQ(regs, csr_swap(CSR_STOPEI, 0) >> TOPI_IID_SHIFT);

    /* Enable external interrupts */
    csr_set(CSR_SIE, BIT(IRQ_S_EXT, UL));
}

static void cf_check aplic_set_irq_type(struct irq_desc *desc,
                                        unsigned int type)
{
    /*
     * Interrupt 0 isn't possible based on the spec:
     *   Each of an APLIC’s interrupt sources has a fixed unique identity
     *   number in the range 1 to N, where N is the total number of sources at
     *   the APLIC. The number zero is not a valid interrupt identity number at
     *   an APLIC. The maximum number of interrupt sources an APLIC may support
     *   is 1023.
     *
     * Thereby interrupt 1 will correspond to bit 0 in sourcecfg[] register,
     * interrupt 2 ->sourcecfg[1] and so on.
     *
     * And that is the reason why we need -1.
     */
    unsigned int irq_bit = desc->irq - 1;

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock(&aplic.lock);

    switch ( type )
    {
    case IRQ_TYPE_EDGE_RISING:
        writel(APLIC_SOURCECFG_SM_EDGE_RISE, &aplic.regs->sourcecfg[irq_bit]);
        break;

    case IRQ_TYPE_EDGE_FALLING:
        writel(APLIC_SOURCECFG_SM_EDGE_FALL, &aplic.regs->sourcecfg[irq_bit]);
        break;

    case IRQ_TYPE_LEVEL_HIGH:
        writel(APLIC_SOURCECFG_SM_LEVEL_HIGH, &aplic.regs->sourcecfg[irq_bit]);
        break;

    case IRQ_TYPE_LEVEL_LOW:
        writel(APLIC_SOURCECFG_SM_LEVEL_LOW, &aplic.regs->sourcecfg[irq_bit]);
        break;

    case IRQ_TYPE_NONE:
    case IRQ_TYPE_INVALID:
        writel(APLIC_SOURCECFG_SM_INACTIVE, &aplic.regs->sourcecfg[irq_bit]);
        break;

    default:
        panic("%s: APLIC doesnt support IRQ type: 0x%x?\n", __func__, type);
    }

    spin_unlock(&aplic.lock);
}

static const hw_irq_controller aplic_xen_irq_type = {
    .typename     = "aplic",
    .startup      = aplic_irq_startup,
    .shutdown     = aplic_irq_disable,
    .enable       = aplic_irq_enable,
    .disable      = aplic_irq_disable,
    .set_affinity = aplic_set_irq_affinity,
};

static unsigned int cf_check aplic_guest_irq_startup(struct irq_desc *desc)
{
    BUG_ON("unimplemented");
}

/*
 * Shared by ->shutdown(), ->enable(), ->disable() and ->end(), which have
 * no state.
 */
static void cf_check aplic_guest_irq_stub(struct irq_desc *desc)
{
    BUG_ON("unimplemented");
}

static void cf_check aplic_guest_set_irq_affinity(struct irq_desc *desc,
                                                  const cpumask_t *mask)
{
    BUG_ON("unimplemented");
}

static const hw_irq_controller aplic_guest_irq_type = {
    .typename     = "aplic-guest",
    .startup      = aplic_guest_irq_startup,
    .shutdown     = aplic_guest_irq_stub,
    .enable       = aplic_guest_irq_stub,
    .disable      = aplic_guest_irq_stub,
    .end          = aplic_guest_irq_stub,
    .set_affinity = aplic_guest_set_irq_affinity,
};

static const struct intc_hw_operations aplic_ops = {
    .info                = &aplic_info,
    .host_irq_type       = &aplic_xen_irq_type,
    .guest_irq_type      = &aplic_guest_irq_type,
    .handle_interrupt    = aplic_handle_interrupt,
    .set_irq_type        = aplic_set_irq_type,
};

static const struct intc_hw_init_ops __initconstrel aplic_init_ops = {
    .ops                 = &aplic_ops,
    .init                = aplic_init,
};

static int cf_check aplic_irq_xlate(const uint32_t *intspec,
                                    unsigned int intsize,
                                    unsigned int *out_hwirq,
                                    unsigned int *out_type)
{
    if ( intsize < 2 )
        return -EINVAL;

    /* Mapping 1:1 */
    *out_hwirq = intspec[0];

    if ( out_type )
        *out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;

    return 0;
}

static int __init aplic_preinit(struct dt_device_node *node, const void *dat)
{
    if ( aplic_info.node )
    {
        printk("XEN doesn't support more than one S mode APLIC\n");
        return -ENODEV;
    }

    /* don't process if APLIC node is not for S mode */
    if ( dt_get_property(node, "riscv,children", NULL) )
        return -ENODEV;

    aplic_info.node = node;

    aplic.imsic_cfg = imsic_get_config();

    dt_irq_xlate = aplic_irq_xlate;

    register_intc_ops(&aplic_init_ops);

    /* Enable supervisor external interrupt */
    csr_set(CSR_SIE, BIT(IRQ_S_EXT, UL));

    return 0;
}

static const struct dt_device_match __initconstrel aplic_dt_match[] =
{
    DT_MATCH_COMPATIBLE("riscv,aplic"),
    { /* sentinel */ },
};

DT_DEVICE_START(aplic, "APLIC", DEVICE_INTERRUPT_CONTROLLER)
    .dt_match = aplic_dt_match,
    .init = aplic_preinit,
DT_DEVICE_END
