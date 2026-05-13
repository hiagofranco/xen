/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <xen/bitops.h>
#include <xen/cpu.h>
#include <xen/init.h>
#include <xen/notifier.h>
#include <xen/percpu.h>
#include <xen/smp.h>

#include <asm/atomic.h>
#include <asm/ipi.h>

static DEFINE_PER_CPU(atomic_t, pending_ipis);
static ipi_send_t __ro_after_init ipi_send;

static int cf_check cpu_callback(struct notifier_block *nfb,
                                 unsigned long action,
                                 void *hcpu)
{
    switch ( action )
    {
    case CPU_STARTING:
        csr_set(CSR_SIE, SIP_SSIP);
        break;
    }

    return notifier_from_errno(0);
}

static struct notifier_block ipi_nfb = {
    .notifier_call = cpu_callback,
};

void ipi_send_mask(enum ipis op, const cpumask_t *mask)
{
    unsigned int ipi_bit = BIT(op, U);
    cpumask_t send_mask = { 0 };
    int cpu;

    for_each_cpu ( cpu, mask )
    {
        unsigned int pending =
            atomic_fetch_or(ipi_bit, &per_cpu(pending_ipis, cpu));

        if ( !(pending & ipi_bit) )
            cpumask_set_cpu(cpu, &send_mask);
    }

    if ( !cpumask_empty(&send_mask) )
        ipi_send(&send_mask);
}

/* Handle multiplexed virtual IPIs */
void ipi_handler(void)
{
    unsigned int ipis;

    csr_clear(CSR_SIP, SIP_SSIP);

    ipis = (unsigned int)atomic_xchg(&this_cpu(pending_ipis), 0);

    for_each_set_bit ( ipi, ipis )
    {
        switch ( ipi )
        {
        case IPI_EVENT_CHECK:
            /* Nothing to do, will check for events on return path */
            break;

        case IPI_CALL_FUNCTION:
            smp_call_function_interrupt();
            break;

        default:
            printk("CPU%u: unhandled IPI%u\n", smp_processor_id(), ipi);
            break;
        }
    }
}

/* Initialize virtual IPIs on top of a single hardware IPI */
void __init ipi_init(ipi_send_t send_func)
{
    BUILD_BUG_ON(sizeof(this_cpu(pending_ipis)) * BITS_PER_BYTE < IPI_MAX);

    ipi_send = send_func;

    register_cpu_notifier(&ipi_nfb);

    csr_set(CSR_SIE, SIP_SSIP);
}
