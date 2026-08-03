/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/acpi.h>
#include <xen/bug.h>
#include <xen/device_tree.h>
#include <xen/dt-overlay.h>
#include <xen/errno.h>
#include <xen/fdt-kernel.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <xen/xvmalloc.h>

#include <asm/aia.h>
#include <asm/csr.h>
#include <asm/intc.h>
#include <asm/riscv_encoding.h>
#include <asm/vaplic.h>

static const struct intc_hw_operations *__ro_after_init intc_hw_ops;

static const struct intc_hw_init_ops *__initdata intc_hw_init_ops;

void __init register_intc_ops(const struct intc_hw_init_ops *init_ops)
{
    intc_hw_ops = init_ops->ops;
    intc_hw_init_ops = init_ops;
}

void __init intc_preinit(void)
{
    if ( acpi_disabled )
        intc_dt_preinit();
    else
        panic("ACPI interrupt controller preinit() isn't implemented\n");
}

void __init intc_init(void)
{
    ASSERT(intc_hw_init_ops && intc_hw_init_ops->init);

    aia_init();

    if ( intc_hw_init_ops->init() )
        panic("Failed to initialize the interrupt controller drivers\n");

    intc_init_cpu();
}

void intc_init_cpu(void)
{
    ASSERT(intc_hw_ops);

    if ( intc_hw_ops->init_cpu_intc )
    {
        int rc = intc_hw_ops->init_cpu_intc();

        if ( rc )
        {
            printk(XENLOG_ERR
                   "CPU%u: intc init failed (%d), external interrupts stay masked\n",
                   smp_processor_id(), rc);

            return;
        }
    }

    csr_set(CSR_SIE, BIT(IRQ_S_EXT, UL));
}

/* desc->irq needs to be disabled before calling this function */
static void intc_set_irq_type(struct irq_desc *desc, unsigned int type)
{
    ASSERT(desc->status & IRQ_DISABLED);
    ASSERT(spin_is_locked(&desc->lock));
    ASSERT(type != IRQ_TYPE_INVALID);

    if ( intc_hw_ops->set_irq_type )
        intc_hw_ops->set_irq_type(desc, type);
}

static void intc_set_irq_priority(struct irq_desc *desc, unsigned int priority)
{
    ASSERT(spin_is_locked(&desc->lock));

    if ( intc_hw_ops->set_irq_priority )
        intc_hw_ops->set_irq_priority(desc, priority);
}

void intc_handle_external_irqs(struct cpu_user_regs *regs)
{
    intc_hw_ops->handle_interrupt(regs);
}

void intc_route_irq_to_xen(struct irq_desc *desc, unsigned int priority)
{
    ASSERT(desc->status & IRQ_DISABLED);
    ASSERT(spin_is_locked(&desc->lock));
    /* Can't route interrupts that don't exist */
    ASSERT(intc_hw_ops && desc->irq < intc_hw_ops->info->num_irqs);

    desc->handler = intc_hw_ops->host_irq_type;

    intc_set_irq_type(desc, desc->arch.type);
    intc_set_irq_priority(desc, priority);
}

int intc_route_irq_to_guest(const struct domain *d, unsigned int virq,
                            struct irq_desc *desc, unsigned int priority)
{
    ASSERT(spin_is_locked(&desc->lock));

    ASSERT(intc_hw_ops->guest_irq_type);

    /* The caller has reserved @virq, see vintc_reserve_virq(). */
    ASSERT(virq < d->arch.vintc->nr_virqs);

    desc->handler = intc_hw_ops->guest_irq_type;
    desc->status |= IRQ_GUEST;

    intc_set_irq_type(desc, IRQ_TYPE_NONE);
    intc_set_irq_priority(desc, priority);

    /* Connect @virq to the h/w IRQ: it isn't a virtual source. */
    set_bit(virq, d->arch.vintc->hw_irqs);

    return 0;
}

int __init make_intc_domU_node(struct kernel_info *kinfo)
{
    const struct vintc *vintc = kinfo->bd.d->arch.vintc;

    return vintc->init_ops->make_domu_dt_node(kinfo);
}

int domain_vintc_init(struct domain *d)
{
    int ret = -EOPNOTSUPP;
    const enum intc_variant variant = intc_hw_ops->info->hw_variant;

    switch ( variant )
    {
    case INTC_APLIC:
        ret = domain_vaplic_init(d);
        break;

    default:
        printk_once("vintc (variant:%d) isn't implemented\n", variant);
        break;
    }

    if ( !ret )
    {
        d->arch.vintc->used_irqs =
            xvzalloc_array(unsigned long,
                           BITS_TO_LONGS(d->arch.vintc->nr_virqs));
        d->arch.vintc->hw_irqs =
            xvzalloc_array(unsigned long,
                           BITS_TO_LONGS(d->arch.vintc->nr_virqs));
        if ( !d->arch.vintc->used_irqs || !d->arch.vintc->hw_irqs )
            ret = -ENOMEM;
    }

    return ret;
}

void domain_vintc_deinit(struct domain *d)
{
    const enum intc_variant variant = intc_hw_ops->info->hw_variant;

    if ( !d->arch.vintc )
        return;

    if ( d->arch.vintc->hw_irqs )
    {
        unsigned int virq;

        /* A virtual source has no h/w IRQ routed to the domain to release. */
        for ( virq = 0; virq < d->arch.vintc->nr_virqs; virq++ )
            if ( test_bit(virq, d->arch.vintc->hw_irqs) )
                release_guest_irq(d, virq);
    }

    XVFREE(d->arch.vintc->used_irqs);
    XVFREE(d->arch.vintc->hw_irqs);

    switch ( variant )
    {
    case INTC_APLIC:
        domain_vaplic_deinit(d);
        break;

    default:
        break;
    }
}

/*
 * Mark @virq as used by @d so that domain_vintc_deinit() knows that it has to
 * be released.
 *
 * Returns 0 on success, -EEXIST if @virq has already been reserved, which
 * legitimately happens when an IRQ is shared between devices, and -ERANGE if
 * @virq is outside the range of the interrupt sources the vINTC provides.
 * A virtual source can't be shared, so for one -EEXIST is a failure.
 */
int __overlay_init vintc_reserve_virq(const struct domain *d,
                                      unsigned int virq)
{
    if ( virq >= d->arch.vintc->nr_virqs )
        return -ERANGE;

    return test_and_set_bit(virq, d->arch.vintc->used_irqs) ? -EEXIST : 0;
}

void vintc_ctxt_switch_from(struct vcpu *p)
{
    const struct vintc_ops *ops = p->domain->arch.vintc->ops;

    ops->ctxt_switch_from(p);
}

void vintc_ctxt_switch_to(struct vcpu *n)
{
    const struct vintc_ops *ops = n->domain->arch.vintc->ops;

    ops->ctxt_switch_to(n);
}

/* Move vCPU's IRQs from one pCPU to another */
void arch_move_irqs(struct vcpu *v)
{
    const struct vintc_ops *ops = v->domain->arch.vintc->ops;

    if ( ops->move_irqs )
        ops->move_irqs(v);
}

/* Update vINTC state of @v, the vCPU about to enter the guest. */
void vintc_enter_guest(struct vcpu *v)
{
    const struct vintc_ops *ops = v->domain->arch.vintc->ops;

    ASSERT(v == current);

    if ( ops->enter_guest )
        ops->enter_guest(v);
}

/*
 * Set the level of the line of virtual source @virq of @d. To be used by the
 * emulator of a device in Xen, the line of which is connected to @virq: such
 * a source has to be reserved with vintc_reserve_virq(), and no h/w IRQ may be
 * routed to it.
 */
void vintc_set_irq_level(const struct domain *d, unsigned int virq,
                         bool level)
{
    const struct vintc_ops *ops = d->arch.vintc->ops;

    ops->set_irq_level(d, virq, level);
}
