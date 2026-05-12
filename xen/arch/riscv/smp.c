#include <xen/cpumask.h>
#include <xen/smp.h>

#include <asm/ipi.h>
#include <asm/sbi.h>

/*
 * FIXME: make pcpu_info[] dynamically allocated when necessary
 *        functionality will be ready
 */
/*
 * tp points to one of these per cpu.
 *
 * hart_id would be valid (no matter which value) if its
 * processor_id field is valid (not CPU_NONE).
 */
struct pcpu_info pcpu_info[NR_CPUS] = { [0 ... NR_CPUS - 1] = {
    .processor_id = CPU_NONE,
}};

void smp_send_event_check_mask(const cpumask_t *mask)
{
    ipi_send_mask(IPI_EVENT_CHECK, mask);
}

void smp_send_call_function_mask(const cpumask_t *mask)
{
    cpumask_t target;
    unsigned int cpu = smp_processor_id();

    cpumask_andnot(&target, mask, cpumask_of(cpu));

    ipi_send_mask(IPI_CALL_FUNCTION, &target);

    if ( cpumask_test_cpu(cpu, mask) )
    {
        local_irq_disable();
        smp_call_function_interrupt();
        local_irq_enable();
    }
}
