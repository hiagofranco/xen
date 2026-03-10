/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * RISC-V Interrupt support
 *
 * Copyright (c) Vates
 */

#include <xen/bug.h>
#include <xen/cpumask.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <xen/xvmalloc.h>

#include <asm/hardirq.h>
#include <asm/intc.h>

/* Describe an IRQ assigned to a guest */
struct irq_guest
{
    struct domain *d;
    unsigned int virq;
    /*
     * The action of a guest IRQ has the same lifetime as this structure, so
     * embed it here to have both covered by a single allocation. Consequently
     * it must not be freed by release_irq() (see free_on_release below).
     */
    struct irqaction action;
};

static irq_desc_t irq_desc[NR_IRQS];

struct irq_desc *irq_to_desc(unsigned int irq)
{
    ASSERT(irq < ARRAY_SIZE(irq_desc));
    return &irq_desc[irq];
}

static bool irq_validate_new_type(unsigned int curr, unsigned int new)
{
    return curr == IRQ_TYPE_INVALID || curr == new;
}

static int irq_set_type(unsigned int irq, unsigned int type)
{
    unsigned long flags;
    struct irq_desc *desc = irq_to_desc(irq);
    int ret = -EBUSY;

    spin_lock_irqsave(&desc->lock, flags);

    if ( !irq_validate_new_type(desc->arch.type, type) )
        goto err;

    desc->arch.type = type;

    ret = 0;

 err:
    spin_unlock_irqrestore(&desc->lock, flags);

    return ret;
}

int platform_get_irq(const struct dt_device_node *device, int index)
{
    struct dt_irq dt_irq;
    int ret;

    if ( (ret = dt_device_get_irq(device, index, &dt_irq)) != 0 )
        return ret;

    BUILD_BUG_ON(NR_IRQS > INT_MAX);

    if ( dt_irq.irq >= NR_IRQS )
        panic("irq%d is bigger then NR_IRQS(%d)\n", dt_irq.irq, NR_IRQS);

    if ( (ret = irq_set_type(dt_irq.irq, dt_irq.type)) != 0 )
        return ret;

    return dt_irq.irq;
}

static int _setup_irq(struct irq_desc *desc, unsigned int irqflags,
                      struct irqaction *new)
{
    bool shared = irqflags & IRQF_SHARED;

    ASSERT(new != NULL);

    /*
     * Sanity checks:
     *  - if the IRQ is marked as shared
     *  - dev_id is not NULL when IRQF_SHARED is set
     */
    if ( desc->action != NULL && (!(desc->status & IRQF_SHARED) || !shared) )
        return -EINVAL;
    if ( shared && new->dev_id == NULL )
        return -EINVAL;

    if ( shared )
        desc->status |= IRQF_SHARED;

#ifdef CONFIG_IRQ_HAS_MULTIPLE_ACTION
    new->next = desc->action;
#endif

    desc->action = new;
    smp_wmb();

    return 0;
}

int setup_irq(unsigned int irq, unsigned int irqflags, struct irqaction *new)
{
    int rc;
    unsigned long flags;
    struct irq_desc *desc = irq_to_desc(irq);
    bool disabled;

    spin_lock_irqsave(&desc->lock, flags);

    disabled = (desc->action == NULL);

    if ( desc->status & IRQ_GUEST )
    {
        spin_unlock_irqrestore(&desc->lock, flags);
        /*
         * TODO: would be nice to have functionality to print which domain owns
         *       an IRQ.
         */
        printk(XENLOG_ERR "ERROR: IRQ %u is already in use by a domain\n", irq);
        return -EBUSY;
    }

    rc = _setup_irq(desc, irqflags, new);
    if ( rc )
        goto err;

    /* First time the IRQ is setup */
    if ( disabled )
    {
        /* Route interrupt to xen */
        intc_route_irq_to_xen(desc, IRQ_NO_PRIORITY);

        /*
         * We don't care for now which CPU will receive the
         * interrupt.
         *
         * TODO: Handle case where IRQ is setup on different CPU than
         *       the targeted CPU and the priority.
         */
        desc->handler->set_affinity(desc, cpumask_of(smp_processor_id()));

        desc->handler->startup(desc);

        /* Enable irq */
        desc->status &= ~IRQ_DISABLED;
    }

 err:
    spin_unlock_irqrestore(&desc->lock, flags);

    return rc;
}

int arch_init_one_irq_desc(struct irq_desc *desc)
{
    desc->arch.type = IRQ_TYPE_INVALID;

    return 0;
}

static int __init init_irq_data(void)
{
    unsigned int irq;

    for ( irq = 0; irq < NR_IRQS; irq++ )
    {
        struct irq_desc *desc = irq_to_desc(irq);
        int rc;

        desc->irq = irq;

        rc = init_one_irq_desc(desc);
        if ( rc )
            return rc;
    }

    return 0;
}

void __init init_IRQ(void)
{
    if ( init_irq_data() < 0 )
        panic("initialization of IRQ data failed\n");
}

/* Dispatch an interrupt */
void do_IRQ(struct cpu_user_regs *regs, unsigned int irq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    struct irqaction *action;

    irq_enter();

    spin_lock(&desc->lock);

    if ( desc->handler->ack )
        desc->handler->ack(desc);

    if ( desc->status & IRQ_GUEST )
        /*
         * With APLIC + IMSIC, guest interrupts bypass Xen and are delivered
         * directly to the guest. Without IMSIC, interrupts would be trapped
         * by Xen and would need injecting into the guest here.
         */
        panic("unimplemented");

    if ( desc->status & IRQ_DISABLED )
        goto out;

    desc->status |= IRQ_INPROGRESS;

    action = desc->action;

    spin_unlock_irq(&desc->lock);

#ifndef CONFIG_IRQ_HAS_MULTIPLE_ACTION
    action->handler(irq, action->dev_id);
#else
    do {
        action->handler(irq, action->dev_id);
        action = action->next;
    } while ( action );
#endif /* CONFIG_IRQ_HAS_MULTIPLE_ACTION */

    spin_lock_irq(&desc->lock);

    desc->status &= ~IRQ_INPROGRESS;

 out:
    if ( desc->handler->end )
        desc->handler->end(desc);

    spin_unlock(&desc->lock);
    irq_exit();
}

static struct irq_guest *irq_get_guest_info(struct irq_desc *desc)
{
    ASSERT(spin_is_locked(&desc->lock));
    ASSERT(test_bit(_IRQ_GUEST, &desc->status));
    ASSERT(desc->action != NULL);

    return desc->action->dev_id;
}

void release_irq(unsigned int irq, const void *dev_id)
{
    struct irq_desc *desc;
    unsigned long flags;
    struct irqaction *action, **action_ptr;

    desc = irq_to_desc(irq);

    spin_lock_irqsave(&desc->lock, flags);

    action_ptr = &desc->action;
#ifdef CONFIG_IRQ_HAS_MULTIPLE_ACTION
    for ( ;; )
    {
        action = *action_ptr;
        if ( !action )
        {
            printk(XENLOG_WARNING "Trying to free already-free IRQ %u\n", irq);
            spin_unlock_irqrestore(&desc->lock, flags);
            return;
        }

        if ( action->dev_id == dev_id )
            break;

        action_ptr = &action->next;
    }

    /* Found it - remove it from the action list */
    *action_ptr = action->next;
#else
    action = *action_ptr;
    *action_ptr = NULL;
#endif

    /* If this was the last action, shut down the IRQ */
    if ( !desc->action )
    {
        desc->handler->shutdown(desc);
        __clear_bit(_IRQ_GUEST, &desc->status);
    }

    spin_unlock_irqrestore(&desc->lock, flags);

    /*
     * Wait to make sure it's not being used on another CPU.
     *
     * The read barrier pairs with the spin_unlock() in do_IRQ(): once we
     * observe _IRQ_INPROGRESS cleared, we are guaranteed to also see the
     * writes do_IRQ() made to desc (e.g. desc->action) before releasing the
     * lock, so it is safe to free the action below.
     */
    do { smp_rmb(); } while ( test_bit(_IRQ_INPROGRESS, &desc->status) );

    if ( action->free_on_release )
        xvfree(action);
}

int release_guest_irq(struct domain *d, unsigned int virq)
{
    struct irq_desc *desc = irq_to_desc(virq);
    struct irq_guest *info;
    unsigned long flags;
    int ret = -EINVAL;

    spin_lock_irqsave(&desc->lock, flags);

    if ( !test_bit(_IRQ_GUEST, &desc->status) )
        goto unlock_err;

    info = irq_get_guest_info(desc);
    if ( d != info->d )
        goto unlock_err;

    /*
     * Live IRQ unrouting from a running domain is not supported: the tear-down
     * drops desc->lock across release_irq()/xvfree() and relies on no
     * concurrent route_irq_to_guest() being issued for this domain. Only permit
     * it for a dying domain, where assignment is frozen and no new routes can
     * appear.
     */
    if ( !d->is_dying )
    {
        ret = -EBUSY;
        goto unlock_err;
    }

    /*
     * Clear _IRQ_GUEST while still holding the lock so that a concurrent
     * release_guest_irq() for the same IRQ observes it and bails out, rather
     * than capturing the same 'info' and double-freeing it below.
     */
    __clear_bit(_IRQ_GUEST, &desc->status);

    spin_unlock_irqrestore(&desc->lock, flags);

    release_irq(desc->irq, info);
    xvfree(info);

    return 0;

 unlock_err:
    spin_unlock_irqrestore(&desc->lock, flags);
    return ret;
}

/* Route an IRQ to a specific guest */
int route_irq_to_guest(struct domain *d, unsigned int virq,
                       unsigned int irq, const char *devname)
{
    struct irq_guest *info;
    struct irq_desc *desc;
    unsigned long flags;
    int retval = 0;

    if ( d->is_dying )
        return -EINVAL;

    desc = irq_to_desc(irq);

    info = xvzalloc(struct irq_guest);
    if ( !info )
        return -ENOMEM;

    info->d = d;
    info->virq = virq;

    info->action.dev_id = info;
    info->action.name = devname;
    /* The action is part of 'info', thus it is freed together with it. */
    info->action.free_on_release = false;

    spin_lock_irqsave(&desc->lock, flags);

    /*
     * If the IRQ is already used by someone
     *  - If it's the same domain -> Xen doesn't need to update the IRQ desc.
     *  For safety check if we are not trying to assign the IRQ to a
     *  different vIRQ.
     *  - Otherwise -> For now, don't allow the IRQ to be shared between
     *  Xen and domains.
     */
    if ( desc->action != NULL )
    {
        if ( test_bit(_IRQ_GUEST, &desc->status) )
        {
            struct domain *ad = irq_get_guest_info(desc)->d;

            if ( d != ad )
            {
                printk(XENLOG_G_ERR "IRQ %u is already used by %pd\n",
                       irq, ad);
                retval = -EBUSY;
            }
            else if ( irq_get_guest_info(desc)->virq != virq )
            {
                printk(XENLOG_G_ERR
                       "%pd: IRQ %u is already assigned to vIRQ %u\n",
                       d, irq, irq_get_guest_info(desc)->virq);
                retval = -EBUSY;
            }
        }
        else
        {
            printk(XENLOG_G_ERR "IRQ %u is already used by Xen\n", irq);
            retval = -EBUSY;
        }
        goto out;
    }

    retval = _setup_irq(desc, 0, &info->action);
    if ( retval )
        goto out;

    retval = intc_route_irq_to_guest(desc, IRQ_NO_PRIORITY);

    spin_unlock_irqrestore(&desc->lock, flags);

    if ( retval )
    {
        release_irq(desc->irq, info);
        goto free_info;
    }

    return 0;

 out:
    spin_unlock_irqrestore(&desc->lock, flags);
 free_info:
    xvfree(info);

    return retval;
}
