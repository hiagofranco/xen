/* SPDX-License-Identifier: MIT */

/*
 * xen/arch/riscv/imsic.c
 *
 * RISC-V Incoming MSI Controller support
 *
 * (c) Microchip Technology Inc.
 * (c) Vates
 */

#include <xen/bitops.h>
#include <xen/const.h>
#include <xen/cpumask.h>
#include <xen/device_tree.h>
#include <xen/domain.h>
#include <xen/errno.h>
#include <xen/fdt-domain-build.h>
#include <xen/fdt-kernel.h>
#include <xen/init.h>
#include <xen/libfdt/libfdt.h>
#include <xen/macros.h>
#include <xen/rwlock.h>
#include <xen/sched.h>
#include <xen/smp.h>
#include <xen/spinlock.h>
#include <xen/xvmalloc.h>

#include <asm/aia.h>
#include <asm/imsic.h>
#include <asm/p2m.h>

#define IMSIC_HART_SIZE(guest_bits) (BIT(guest_bits, U) * IMSIC_MMIO_PAGE_SZ)

struct imsic_mmios {
    paddr_t base_addr;
    unsigned long size;
};

static struct imsic_config imsic_cfg = {
    .lock = SPIN_LOCK_UNLOCKED,
};

/*
 * Number of MSIs available to a guest. Determined by the host interrupt
 * controller, so it is identical for every domain -- hence a single global
 * rather than a per-domain value.
 */
static unsigned int __ro_after_init guest_num_msis;

#define GUEST_IMSIC_COMPATIBLE "riscv,imsics"

/*
 * Value is inspired by what QEMU is using for riscv,num-ids property for IMSIC
 * node.
 */
#define GUEST_IMSIC_MAX_MSIS 255U

#define IMSIC_DISABLE_EIDELIVERY    0
#define IMSIC_ENABLE_EIDELIVERY     1
#define IMSIC_DISABLE_EITHRESHOLD   1
#define IMSIC_ENABLE_EITHRESHOLD    0

#define imsic_csr_write(c, v)   \
do {                            \
    csr_write(CSR_SISELECT, c); \
    csr_write(CSR_SIREG, v);    \
} while (0)

#define imsic_csr_set(c, v)     \
do {                            \
    csr_write(CSR_SISELECT, c); \
    csr_set(CSR_SIREG, v);      \
} while (0)

#define imsic_csr_clear(c, v)   \
do {                            \
    csr_write(CSR_SISELECT, c); \
    csr_clear(CSR_SIREG, v);    \
} while (0)

#define imsic_vs_csr_write(c, v)    \
do {                                \
    csr_write(CSR_VSISELECT, c);    \
    csr_write(CSR_VSIREG, v);       \
} while ( 0 )

/*
 * Generic switchcase expansion pyramid.
 * F is the per-operation leaf macro, ireg is the base register index.
 * Optional extra args (e.g. an operation and/or a value) are forwarded to F
 * via __VA_ARGS__.
 *
 * imsic_switchcase_break(ireg, op, v) - emit "case ireg: op(ireg,v); break;"
 * imsic_switchcase_ret(ireg, op, ...) - emit "case ireg: return op(ireg[,v]);"
 *   The variadic tail is optional so the same leaf works for both read (no v)
 *   and swap (with v).
 */
#define imsic_switchcase_break(ireg, op, v) \
    case ireg:                              \
        op(ireg, v);                        \
        break;

#define imsic_switchcase_ret(ireg, op, ...) \
    case ireg:                              \
        return op(ireg, ##__VA_ARGS__);

#define imsic_switchcase_2(F, ireg, ...)    \
    F(ireg + 0, ##__VA_ARGS__)              \
    F(ireg + 1, ##__VA_ARGS__)
#define imsic_switchcase_4(F, ireg, ...)    \
    imsic_switchcase_2(F, ireg + 0, ##__VA_ARGS__)  \
    imsic_switchcase_2(F, ireg + 2, ##__VA_ARGS__)
#define imsic_switchcase_8(F, ireg, ...)    \
    imsic_switchcase_4(F, ireg + 0, ##__VA_ARGS__)  \
    imsic_switchcase_4(F, ireg + 4, ##__VA_ARGS__)
#define imsic_switchcase_16(F, ireg, ...)   \
    imsic_switchcase_8(F, ireg + 0, ##__VA_ARGS__)  \
    imsic_switchcase_8(F, ireg + 8, ##__VA_ARGS__)
#define imsic_switchcase_32(F, ireg, ...)   \
    imsic_switchcase_16(F, ireg + 0, ##__VA_ARGS__) \
    imsic_switchcase_16(F, ireg + 16, ##__VA_ARGS__)
#define imsic_switchcase_64(F, ireg, ...)   \
    imsic_switchcase_32(F, ireg + 0, ##__VA_ARGS__) \
    imsic_switchcase_32(F, ireg + 32, ##__VA_ARGS__)

static void imsic_eix_write(int ireg, unsigned long val)
{
    switch ( ireg )
    {
    imsic_switchcase_64(imsic_switchcase_break, IMSIC_EIP0,
                         imsic_vs_csr_write, val)
    imsic_switchcase_64(imsic_switchcase_break, IMSIC_EIE0,
                         imsic_vs_csr_write, val)
    default:
        ASSERT_UNREACHABLE();
    }
}

bool imsic_has_interrupt(const struct vcpu *vcpu)
{
    struct vimsic_state *imsic_state = vcpu->arch.vimsic_state;
    unsigned long flags;
    bool ret = false;

    /*
     * The IMSIC SW-file directly injects interrupt via hvip so
     * only check for interrupt when IMSIC VS-file is being used.
     */

    read_lock_irqsave(&imsic_state->vsfile_lock, flags);
    if ( imsic_state->vsfile_pcpu != NR_CPUS )
        ret = !!(csr_read(CSR_HGEIP) & BIT(imsic_state->guest_file_id, UL));
    read_unlock_irqrestore(&imsic_state->vsfile_lock, flags);

    return ret;
}

unsigned int vcpu_guest_file_id(const struct vcpu *v)
{
    return ACCESS_ONCE(v->arch.vimsic_state->guest_file_id);
}

void imsic_update_state(struct vcpu *v, unsigned int guest_file_id)
{
    unsigned long flags;
    struct vimsic_state *vimsic_state = v->arch.vimsic_state;
    unsigned long pcpu = ( !guest_file_id ) ?
                         NR_CPUS : cpuid_to_hartid(v->processor);

    write_lock_irqsave(&vimsic_state->vsfile_lock, flags);
    vimsic_state->guest_file_id = guest_file_id;
    vimsic_state->vsfile_pcpu = pcpu;
    write_unlock_irqrestore(&vimsic_state->vsfile_lock, flags);
}

void __init imsic_ids_local_delivery(bool enable)
{
    if ( enable )
    {
        imsic_csr_write(IMSIC_EITHRESHOLD, IMSIC_ENABLE_EITHRESHOLD);
        imsic_csr_write(IMSIC_EIDELIVERY, IMSIC_ENABLE_EIDELIVERY);
    }
    else
    {
        imsic_csr_write(IMSIC_EITHRESHOLD, IMSIC_DISABLE_EITHRESHOLD);
        imsic_csr_write(IMSIC_EIDELIVERY, IMSIC_DISABLE_EIDELIVERY);
    }
}

static void imsic_local_eix_update(unsigned long base_id, unsigned long num_id,
                                   bool pend, bool val)
{
    unsigned long id = base_id, last_id = base_id + num_id;

    while ( id < last_id )
    {
        unsigned long isel, ireg;
        unsigned long start_id = id & (__riscv_xlen - 1);
        unsigned long chunk = __riscv_xlen - start_id;
        unsigned long count = min(last_id - id, chunk);

        isel = id / __riscv_xlen;
        isel *= __riscv_xlen / IMSIC_EIPx_BITS;
        isel += pend ? IMSIC_EIP0 : IMSIC_EIE0;

        ireg = GENMASK(start_id + count - 1, start_id);

        id += count;

        if ( val )
            imsic_csr_set(isel, ireg);
        else
            imsic_csr_clear(isel, ireg);
    }
}

void imsic_irq_enable(unsigned int irq)
{
    /*
     * The only caller of imsic_irq_enable() is aplic_irq_enable(), which
     * already runs with IRQs disabled. Therefore, there's no need to use
     * spin_lock_irqsave() in this function.
     *
     * This ASSERT is added as a safeguard: if imsic_irq_enable() is ever
     * called from a context where IRQs are not disabled,
     * spin_lock_irqsave() should be used instead of spin_lock().
     */
    ASSERT(!local_irq_is_enabled());

    spin_lock(&imsic_cfg.lock);
    /*
     * There is no irq - 1 here (look at aplic_set_irq_type()) because:
     * From the spec:
     *   When an interrupt file supports distinct interrupt identities,
     *   valid identity numbers are between 1 and inclusive. The identity
     *   numbers within this range are said to be implemented by the interrupt
     *   file; numbers outside this range are not implemented. The number zero
     *   is never a valid interrupt identity.
     *   ...
     *   Bit positions in a valid eiek register that don’t correspond to a
     *   supported interrupt identity (such as bit 0 of eie0) are read-only zeros.
     *
     * So in EIx registers interrupt i corresponds to bit i in comparison wiht
     * APLIC's sourcecfg which starts from 0.
     */
    imsic_local_eix_update(irq, 1, false, true);
    spin_unlock(&imsic_cfg.lock);
}

void imsic_irq_disable(unsigned int irq)
{
   /*
    * The only caller of imsic_irq_disable() is aplic_irq_disable(), which
    * already runs with IRQs disabled. Therefore, there's no need to use
    * spin_lock_irqsave() in this function.
    *
    * This ASSERT is added as a safeguard: if imsic_irq_disable() is ever
    * called from a context where IRQs are not disabled,
    * spin_lock_irqsave() should be used instead of spin_lock().
    */
    ASSERT(!local_irq_is_enabled());

    spin_lock(&imsic_cfg.lock);
    imsic_local_eix_update(irq, 1, false, false);
    spin_unlock(&imsic_cfg.lock);
}

/* Callers aren't intended to changed imsic_cfg so return const. */
const struct imsic_config *imsic_get_config(void)
{
    return &imsic_cfg;
}

static int __init imsic_get_parent_hartid(const struct dt_device_node *node,
                                          unsigned int index,
                                          unsigned long *hartid)
{
    int res;
    struct dt_phandle_args args;

    res = dt_parse_phandle_with_args(node, "interrupts-extended",
                                     "#interrupt-cells", index, &args);
    if ( !res )
        res = dt_processor_hartid(args.np->parent, hartid);

    return res;
}

/*
 * Parses IMSIC DT node.
 *
 * Returns 0 if initialization is successful, a negative value on failure,
 * or IRQ_M_EXT if the IMSIC node corresponds to a machine-mode IMSIC,
 * which should be ignored by the hypervisor.
 */
static int __init imsic_parse_node(const struct dt_device_node *node,
                            unsigned int *nr_parent_irqs,
                            unsigned int *nr_mmios)
{
    int rc;
    unsigned int tmp;
    paddr_t base_addr;
    uint32_t *irq_range;

    *nr_parent_irqs = dt_number_of_irq(node);
    if ( !*nr_parent_irqs )
        panic("%s: irq_num can't be 0. Check %s node\n", __func__,
              dt_node_full_name(node));

    irq_range = xvzalloc_array(uint32_t, *nr_parent_irqs * 2);
    if ( !irq_range )
        panic("%s: irq_range[] allocation failed\n", __func__);

    if ( (rc = dt_property_read_u32_array(node, "interrupts-extended",
                                          irq_range, *nr_parent_irqs * 2)) )
        panic("%s: unable to find interrupt-extended in %s node: %d\n",
              __func__, dt_node_full_name(node), rc);

    /* Check that interrupts-extended property is well-formed. */
    for ( unsigned int i = 2; i < (*nr_parent_irqs * 2); i += 2 )
    {
        if ( irq_range[i + 1] != irq_range[1] )
            panic("%s: mode[%u] != %u\n", __func__, i + 1, irq_range[1]);
    }

    if ( irq_range[1] == IRQ_M_EXT )
    {
        /* Machine mode imsic node, ignore it. */
        xvfree(irq_range);

        return IRQ_M_EXT;
    }

    xvfree(irq_range);

    if ( !dt_property_read_u32(node, "riscv,guest-index-bits",
                               &imsic_cfg.guest_index_bits) )
        imsic_cfg.guest_index_bits = 0;
    tmp = BITS_PER_LONG - IMSIC_MMIO_PAGE_SHIFT;
    if ( tmp < imsic_cfg.guest_index_bits )
    {
        printk(XENLOG_ERR "%s: guest index bits too big\n",
               dt_node_name(node));
        return -ENOENT;
    }

    /* Find number of HART index bits */
    if ( !dt_property_read_u32(node, "riscv,hart-index-bits",
                               &imsic_cfg.hart_index_bits) )
        /* Assume default value */
        imsic_cfg.hart_index_bits = fls(*nr_parent_irqs - 1);
    tmp -= imsic_cfg.guest_index_bits;
    if ( tmp < imsic_cfg.hart_index_bits )
    {
        printk(XENLOG_ERR "%s: HART index bits too big\n",
               dt_node_name(node));
        return -ENOENT;
    }

    /* Find number of group index bits */
    if ( !dt_property_read_u32(node, "riscv,group-index-bits",
                               &imsic_cfg.group_index_bits) )
        imsic_cfg.group_index_bits = 0;
    tmp -= imsic_cfg.hart_index_bits;
    if ( tmp < imsic_cfg.group_index_bits )
    {
        printk(XENLOG_ERR "%s: group index bits too big\n",
               dt_node_name(node));
        return -ENOENT;
    }

    /* Find first bit position of group index */
    tmp = IMSIC_MMIO_PAGE_SHIFT * 2;
    if ( !dt_property_read_u32(node, "riscv,group-index-shift",
                               &imsic_cfg.group_index_shift) )
        imsic_cfg.group_index_shift = tmp;
    if ( imsic_cfg.group_index_shift < tmp )
    {
        printk(XENLOG_ERR "%s: group index shift too small\n",
               dt_node_name(node));
        return -ENOENT;
    }
    tmp = imsic_cfg.group_index_bits + imsic_cfg.group_index_shift - 1;
    if ( tmp >= BITS_PER_LONG )
    {
        printk(XENLOG_ERR "%s: group index shift too big\n",
               dt_node_name(node));
        return -ENOENT;
    }

    /* Find number of interrupt identities */
    if ( !dt_property_read_u32(node, "riscv,num-ids", &imsic_cfg.nr_ids) )
    {
        printk(XENLOG_ERR "%s: number of interrupt identities not found\n",
               node->name);
        return -ENOENT;
    }

    if ( dt_property_read_u32(node, "riscv,num-guest-ids", &tmp) )
        guest_num_msis = min(GUEST_IMSIC_MAX_MSIS, tmp);
    else
        guest_num_msis = GUEST_IMSIC_MAX_MSIS;

    if ( (imsic_cfg.nr_ids < IMSIC_MIN_ID) ||
         (imsic_cfg.nr_ids > IMSIC_MAX_ID) )
    {
        printk(XENLOG_ERR "%s: invalid number of interrupt identities\n",
               node->name);
        return -ENOENT;
    }

    /* Compute base address */
    *nr_mmios = 0;
    rc = dt_device_get_address(node, *nr_mmios, &base_addr, NULL);
    if ( rc )
    {
        printk(XENLOG_ERR "%s: first MMIO resource not found: %d\n",
               dt_node_name(node), rc);
        return rc;
    }

    imsic_cfg.base_addr = base_addr;
    imsic_cfg.base_addr &= ~(BIT(imsic_cfg.guest_index_bits +
                                 imsic_cfg.hart_index_bits +
                                 IMSIC_MMIO_PAGE_SHIFT, UL) - 1);
    imsic_cfg.base_addr &= ~((BIT(imsic_cfg.group_index_bits, UL) - 1) <<
                             imsic_cfg.group_index_shift);

    /* Find number of MMIO register sets */
    do {
        ++*nr_mmios;
    } while ( !dt_device_get_address(node, *nr_mmios, &base_addr, NULL) );

    return 0;
}

/*
 * Map the physical IMSIC guest interrupt file (G-file) assigned to vCPU v
 * into the domain's stage-2 guest-physical address space.
 *
 * In the machine's physical address space (SPA), each hart's IMSIC
 * supervisor-level file (S-file) is located at offset 0 of its address block,
 * followed contiguously by GEILEN guest files at offsets of 1, 2, ..., N pages.
 *
 * Because a guest OS running in VS-mode expects its own supervisor-level
 * interrupt file to be at offset 0 of its guest-physical IMSIC block, the
 * hypervisor must use stage-2 address translation to map the vCPU's
 * guest-physical "supervisor" page (GPA offset 0) to the specific
 * physical guest file page (SPA offset guest_file_id) on the physical hart.
 *
 * Xen pins each vCPU to a pCPU (v->processor) and assigns it a physical
 * guest file index (guest_file_id) from the vGEIN allocator. A guest_file_id
 * of 0 indicates that no hardware guest file is selected (matching the
 * architectural behavior where vGEIN = 0 in the hstatus CSR selects no
 * guest external interrupt source), requiring the VS-file to be emulated
 * in software.
 *
 * The base guest-physical address advertised to the guest in the device
 * tree matches offset 0 of the vCPU's virtual IMSIC block. Stage-2
 * translation ensures that guest supervisor accesses to this page are
 * transparently routed to the real hardware VS-file granted to it on
 * the current pCPU.
 */
int imsic_map_guest_file(struct vcpu *v, unsigned int vsfile_id)
{
    int res = 0;
    struct domain *d = v->domain;
    unsigned int cpu = v->processor;
    vaddr_t gaddr = imsic_cfg.base_addr + (IMSIC_MMIO_PAGE_SZ * v->vcpu_id);
    paddr_t paddr;
    unsigned long guest_stride;

    /* Nothing to map in the case of sw interrupt file. */
    if ( !vsfile_id )
        return res;

    guest_stride = vsfile_id * IMSIC_MMIO_PAGE_SZ;

    paddr = imsic_cfg.msi[cpu].base_addr + imsic_cfg.msi[cpu].offset +
            guest_stride;

#ifdef IMSIC_DEBUG
    printk("%s: %pv: ga(%#lx) -> pa(%#lx), cpu(%#x), guest_file_id(%d) "
           "base_addr(%#lx) offset(%#lx)\n", __func__, v, gaddr, paddr, cpu,
           vsfile_id, imsic_cfg.msi[cpu].base_addr, imsic_cfg.msi[cpu].offset);
#endif

    res = map_regions_p2mt(d, gaddr_to_gfn(gaddr),
                           PFN_DOWN(IMSIC_MMIO_PAGE_SZ), maddr_to_mfn(paddr),
                           arch_dt_passthrough_p2m_type());
    if ( res )
        printk("%s: Failed to map %#lx to the guest at %#lx\n",
               __func__, paddr, gaddr);

    return res;
}

void imsic_state_save(struct vcpu *v)
{
    struct vimsic_state *imsic_state = v->arch.vimsic_state;
    unsigned long flags;

    /*
     * SW interrupt file always has ->vsfile_pcpu = NR_CPUS so nothing specific
     * should be done in this case.
     */
    if ( !vcpu_guest_file_id(v) )
        return;

    write_lock_irqsave(&imsic_state->vsfile_lock, flags);
    imsic_state->vsfile_pcpu = cpuid_to_hartid(v->processor);
    /*
     * Start to observe the VS-file from HS-mode: while the vCPU isn't
     * running an interrupt pending in its VS-file is reported through HGEIP
     * instead of being delivered to VS-mode, which lets Xen wake the vCPU up.
     */
    csr_set(CSR_HGEIE, BIT(imsic_state->guest_file_id, UL));
    write_unlock_irqrestore(&imsic_state->vsfile_lock, flags);
}

void imsic_state_restore(struct vcpu *v)
{
    struct vimsic_state *imsic_state = v->arch.vimsic_state;
    unsigned long flags;

    /* A s/w VS-file is never observed through HGEIP. */
    if ( !vcpu_guest_file_id(v) )
        return;

    /*
     * The vCPU is about to run, so hstatus.VGEIN delivers the VS-file's
     * interrupts to it directly and there is nothing left for Xen to observe.
     */
    read_lock_irqsave(&imsic_state->vsfile_lock, flags);
    csr_clear(CSR_HGEIE, BIT(imsic_state->guest_file_id, UL));
    read_unlock_irqrestore(&imsic_state->vsfile_lock, flags);
}

int vcpu_imsic_init(struct vcpu *v)
{
    struct vimsic_state *imsic_state;

    /* Allocate IMSIC context */
    imsic_state = xvzalloc(struct vimsic_state);
    if ( !imsic_state )
        return -ENOMEM;

    /* Setup IMSIC context  */
    rwlock_init(&imsic_state->vsfile_lock);

    /*
     * xvzalloc() already cleared the context, so guest_file_id == 0, i.e. the
     * always-available s/w IMSIC VS-file. Only vsfile_pcpu needs an explicit
     * initializer as its s/w VS-file value is NR_CPUS rather than 0.
     */
    imsic_state->vsfile_pcpu = NR_CPUS;

    v->arch.vimsic_state = imsic_state;

    return 0;
}

void vcpu_imsic_deinit(struct vcpu *v)
{
    XVFREE(v->arch.vimsic_state);
}

/*
 * Initialize the imsic_cfg structure based on the IMSIC DT node.
 *
 * Returns 0 if initialization is successful, a negative value on failure,
 * or IRQ_M_EXT if the IMSIC node corresponds to a machine-mode IMSIC,
 * which should be ignored by the hypervisor.
 */
int __init imsic_init(const struct dt_device_node *node)
{
    int rc;
    unsigned long reloff, hartid;
    unsigned int nr_parent_irqs, index, nr_handlers = 0;
    paddr_t base_addr;
    unsigned int nr_mmios;
    struct imsic_mmios *mmios;
    struct imsic_msi *msi = NULL;

    /* Parse IMSIC node */
    rc = imsic_parse_node(node, &nr_parent_irqs, &nr_mmios);
    /*
     * If machine mode imsic node => ignore it.
     * If rc < 0 => parsing of IMSIC DT node failed.
     */
    if ( (rc == IRQ_M_EXT) || (rc < 0) )
        return rc;

    /* Allocate MMIO resource array */
    mmios = xvzalloc_array(struct imsic_mmios, nr_mmios);
    if ( !mmios )
    {
        rc = -ENOMEM;
        goto imsic_init_err;
    }

    msi = xvzalloc_array(struct imsic_msi, nr_parent_irqs);
    if ( !msi )
    {
        rc = -ENOMEM;
        goto imsic_init_err;
    }

    /* Check MMIO register sets */
    for ( unsigned int i = 0; i < nr_mmios; i++ )
    {
        unsigned int guest_bits = imsic_cfg.guest_index_bits;
        unsigned long expected_mmio_size =
            IMSIC_HART_SIZE(guest_bits) * nr_parent_irqs;

        rc = dt_device_get_address(node, i, &mmios[i].base_addr,
                                   &mmios[i].size);
        if ( rc )
        {
            printk(XENLOG_ERR "%s: unable to parse MMIO regset %u\n",
                   node->name, i);
            goto imsic_init_err;
        }

        base_addr = mmios[i].base_addr;
        base_addr &= ~(BIT(guest_bits +
                           imsic_cfg.hart_index_bits +
                           IMSIC_MMIO_PAGE_SHIFT, UL) - 1);
        base_addr &= ~((BIT(imsic_cfg.group_index_bits, UL) - 1) <<
                       imsic_cfg.group_index_shift);
        if ( base_addr != imsic_cfg.base_addr )
        {
            rc = -EINVAL;
            printk(XENLOG_ERR "%s: address mismatch for regset %u\n",
                   node->name, i);
            goto imsic_init_err;
        }

        if ( mmios[i].size != expected_mmio_size )
        {
            rc = -EINVAL;
            printk(XENLOG_ERR "%s: IMSIC MMIO size is incorrect %ld, expected MMIO size: %ld\n",
                   node->name, mmios[i].size, expected_mmio_size);
            goto imsic_init_err;
        }
    }

    /* Configure handlers for target CPUs */
    for ( unsigned int i = 0; i < nr_parent_irqs; i++ )
    {
        unsigned int cpu;

        rc = imsic_get_parent_hartid(node, i, &hartid);
        if ( rc )
        {
            printk(XENLOG_WARNING "%s: cpu ID for parent irq%u not found\n",
                   node->name, i);
            continue;
        }

        cpu = hartid_to_cpuid(hartid);

        /*
         * If .base_addr is not 0, it indicates that the CPU has already been
         * found.
         * In this case, skip re-initialization to avoid duplicate setup.
         * Also, print a warning message to signal that the DTS should be
         * reviewed for possible duplication.
         */
        if ( msi[cpu].base_addr )
        {
            printk("%s: cpu%u is found twice in interrupts-extended prop\n",
                   node->name, cpu);
            continue;
        }

        if ( cpu >= num_possible_cpus() )
        {
            printk(XENLOG_WARNING "%s: unsupported hart ID=%#lx for parent irq%u\n",
                   node->name, hartid, i);
            continue;
        }

        /* Find MMIO location of MSI page */
        reloff = i * IMSIC_HART_SIZE(imsic_cfg.guest_index_bits);
        for ( index = 0; index < nr_mmios; index++ )
        {
            if ( reloff < mmios[index].size )
                break;

            /*
             * MMIO region size may not be aligned to
             * IMSIC_HART_SIZE(guest_index_bits) if
             * holes are present.
             */
            reloff -= ROUNDUP(mmios[index].size,
                              IMSIC_HART_SIZE(imsic_cfg.guest_index_bits));
        }

        if ( index == nr_mmios )
        {
            printk(XENLOG_WARNING "%s: MMIO not found for parent irq%u\n",
                   node->name, i);
            continue;
        }

        if ( !IS_ALIGNED(mmios[index].base_addr + reloff, IMSIC_MMIO_PAGE_SZ) )
        {
            printk(XENLOG_WARNING "%s: MMIO address %#lx is not aligned on a page\n",
                   node->name, mmios[index].base_addr + reloff);

            continue;
        }

        msi[cpu].base_addr = mmios[index].base_addr;
        msi[cpu].offset = reloff;

        nr_handlers++;
    }

    if ( !nr_handlers )
    {
        printk(XENLOG_ERR "%s: No CPU handlers found\n", node->name);
        rc = -ENODEV;
        goto imsic_init_err;
    }

    /* Enable local interrupt delivery */
    imsic_ids_local_delivery(true);

    imsic_cfg.msi = msi;

    xvfree(mmios);

    return 0;

 imsic_init_err:
    xvfree(mmios);
    xvfree(msi);

    return rc;
}

static int __init guest_imsic_make_reg_property(struct domain *d, void *fdt)
{
    paddr_t size = IMSIC_MMIO_PAGE_SZ * d->max_vcpus;
    __be32 regs[4] = {
        cpu_to_be32(GUEST_IMSIC_S_BASE >> 32),
        cpu_to_be32(GUEST_IMSIC_S_BASE),
        cpu_to_be32(size >> 32),
        cpu_to_be32(size),
    };

    return fdt_property(fdt, "reg", regs, sizeof(regs));
}

static int __init guest_imsic_set_interrupt_extended_prop(struct domain *d,
                                                          void *fdt)
{
    unsigned int cpu, pos = 0;
    __be32 *irq_ext;
    int res;

    irq_ext = xvzalloc_array(__be32, d->max_vcpus * 2);
    if ( !irq_ext )
        return -ENOMEM;

    for ( cpu = 0; cpu < d->max_vcpus; cpu++ )
    {
        char buf[64];
        uint32_t phandle;

        snprintf(buf, ARRAY_SIZE(buf), "/cpus/cpu@%u/interrupt-controller", cpu);
        phandle = fdt_get_phandle(fdt, fdt_path_offset(fdt, buf));

        if ( !phandle )
        {
            res = -ENODEV;
            goto out;
        }

        irq_ext[pos++] = cpu_to_be32(phandle);
        irq_ext[pos++] = cpu_to_be32(IRQ_S_EXT);
    }

    res = fdt_property(fdt, "interrupts-extended", irq_ext,
                       d->max_vcpus * 2 * sizeof(*irq_ext));

 out:
    xvfree(irq_ext);

    return res;
}

int __init vimsic_make_domu_dt_node(struct kernel_info *kinfo,
                                    unsigned int *phandle)
{
    int res;
    void *fdt = kinfo->fdt;
    char vimsic_name[32];
    unsigned int vimsic_phandle;

    res = snprintf(vimsic_name, ARRAY_SIZE(vimsic_name), "/soc/imsic@%lx",
                   GUEST_IMSIC_S_BASE);
    if ( res >= ARRAY_SIZE(vimsic_name) )
    {
        dprintk(XENLOG_DEBUG, "vimsic name is truncated\n");
        return -ENOBUFS;
    }

    res = fdt_begin_node(fdt, vimsic_name);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", GUEST_IMSIC_COMPATIBLE);
    if ( res )
        return res;

    res = guest_imsic_make_reg_property(kinfo->bd.d, fdt);
    if ( res )
        return res;

    res = guest_imsic_set_interrupt_extended_prop(kinfo->bd.d, fdt);
    if ( res )
        return res;

    res = fdt_property_u32(fdt, "riscv,num-ids", guest_num_msis);
    if ( res )
        return res;

    res = fdt_property(fdt, "msi-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_u32(fdt, "#msi-cells", 0);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_u32(fdt, "#interrupt-cells", 0);
    if ( res )
        return res;

    vimsic_phandle = alloc_phandle(kinfo);
    if ( !vimsic_phandle )
        return -EOVERFLOW;

    res = fdt_property_cell(fdt, "phandle", vimsic_phandle);
    if ( res )
        return res;

    if ( phandle )
        *phandle = vimsic_phandle;

    return fdt_end_node(fdt);
}

static void imsic_vsfile_local_clear(int vsfile_hgei, unsigned int nr_eix)
{
    unsigned  int i;
    unsigned long new_hstatus, old_hstatus, old_vsiselect;

    /* We can only zero-out if we have a IMSIC VS-file */
    if ( vsfile_hgei <= 0 )
        return;

    old_vsiselect = csr_read(CSR_VSISELECT);
    old_hstatus = csr_read(CSR_HSTATUS);
    new_hstatus = old_hstatus & ~HSTATUS_VGEIN;
    new_hstatus |= ((unsigned long)vsfile_hgei) << HSTATUS_VGEIN_SHIFT;
    csr_write(CSR_HSTATUS, new_hstatus);

    imsic_vs_csr_write(IMSIC_EIDELIVERY, 0);
    imsic_vs_csr_write(IMSIC_EITHRESHOLD, 0);

    for ( i = 0; i < nr_eix; i++ )
    {
        imsic_eix_write(IMSIC_EIP0 + i * 2, 0);
        imsic_eix_write(IMSIC_EIE0 + i * 2, 0);
#ifdef CONFIG_RISCV_32
        imsic_eix_write(IMSIC_EIP0 + i * 2 + 1, 0);
        imsic_eix_write(IMSIC_EIE0 + i * 2 + 1, 0);
#endif
    }

    csr_write(CSR_HSTATUS, old_hstatus);
    csr_write(CSR_VSISELECT, old_vsiselect);
}

void imsic_migrate_vcpu(struct vcpu *v)
{
    unsigned int new_vsfile_hgei;
    unsigned int nr_hw_eix = DIV_ROUND_UP(imsic_cfg.nr_ids,
                                          BITS_PER_TYPE(uint64_t));
    struct vimsic_state *imsic_state = v->arch.vimsic_state;
    unsigned long flags;
    unsigned int old_vsfile_id;

    read_lock_irqsave(&imsic_state->vsfile_lock, flags);
    old_vsfile_id = imsic_state->guest_file_id;
    read_unlock_irqrestore(&imsic_state->vsfile_lock, flags);

    /*
     * The scheduler can mark a freshly created vCPU's unit as migrated and
     * invoke this before the vCPU has ever run (see the migrated branch in
     * schedule()). Such a vCPU still holds its initial software IMSIC VS-file
     * state, and its first run will assign a real VS-file via vcpu_aia_init().
     * Migrating it now would either hit the unsupported SW-file path or leak
     * the VS-file that vcpu_aia_init() subsequently assigns, so skip it.
     *
     * TODO: this check could be dropped when IMSIC sw interrupt file support
     *       will be added.
     */
    if ( v->arch.last_pcpu == NR_CPUS )
        return;

    /*
     * At this point, all interrupt producers are still using the old IMSIC
     * VS-file.
     */

    new_vsfile_hgei = vgein_assign(v);

    /* Zero-out new IMSIC VS-file */
    imsic_vsfile_local_clear(new_vsfile_hgei, nr_hw_eix);

    BUG_ON("unimplemented");
}
