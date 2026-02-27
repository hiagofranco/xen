#include <xen/bootfdt.h>
#include <xen/cpu.h>
#include <xen/cpumask.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/param.h>
#include <xen/percpu.h>
#include <xen/sched.h>
#include <xen/sections.h>
#include <xen/softirq.h>
#include <xen/types.h>

#include <asm/aia.h>
#include <asm/early_printk.h>
#include <asm/current.h>
#include <asm/intc.h>
#include <asm/traps.h>
#include <asm/vmid.h>

cpumask_t __read_mostly cpu_online_map;
cpumask_t __ro_after_init cpu_possible_map;

/* maxcpus: maximum number of CPUs to activate. */
static unsigned int __initdata max_cpus;
integer_param("maxcpus", max_cpus);

/* Representing the hart siblings (hardware threads) within each logcal core */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_sibling_mask);
/* Representing all harts sharing the same physical core */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_core_mask);

static unsigned char __initdata cpu_stack[STACK_SIZE * NR_CPUS]
    __aligned(STACK_SIZE);

extern char secondary_start_sbi[];

int __cpu_up(unsigned int cpu)
{
    unsigned long boot_addr = virt_to_maddr(secondary_start_sbi);
    unsigned long hartid = cpuid_to_hartid(cpu);
    unsigned long hsm_data = (unsigned long)&cpu_stack[STACK_SIZE * cpu];
    int ret;
    s_time_t deadline;

    dprintk(XENLOG_INFO, "CPU%ld boot addr: %#lx, stack addr: %#lx\n",
            hartid, boot_addr, hsm_data);

    ret = sbi_hsm_hart_start(hartid, boot_addr, hsm_data);
    if ( !ret )
    {
        deadline = NOW() + MILLISECS(1000);

        while ( !cpu_online(cpu) && NOW() < deadline )
        {
            cpu_relax();
            process_pending_softirqs();
        }

        /*
          * Ensure that other cpus' initializations are visible before
          * proceeding. Corresponds to smp_wmb() in start_secondary.
          */
        smp_rmb();

        if ( !cpu_online(cpu) )
        {
            printk("CPU%d never came online\n", cpu);
            return -EIO;
        }
    }
    else
        printk("CPU%d: failed to start\n", cpu);

    return ret;
}

/* C entry point for a secondary processor */
void __init noreturn smp_callin(unsigned int hartid)
{
    unsigned int xen_cpuid = hartid_to_cpuid(hartid);
    unsigned long tmp = 0;

    /*
     * If Xen's CPU ID is incorrect, tp cannot be set up reliably.
     * Without a valid tp, smp_processor_id() may return a wrong value,
     * breaking printk(). Use early_printk() instead.
     */
    if ( xen_cpuid == NR_CPUS )
    {
        char msg[256];

        snprintf(msg, sizeof(msg), "Got incorrect cpuid(%u) for hartid(%u)\n",
                 xen_cpuid, hartid);

        early_printk(msg);

        while (1);
    }

    asm volatile ("mv tp, %0" :: "r"((unsigned long)&pcpu_info[xen_cpuid]));

    set_processor_id(xen_cpuid);

    trap_init();

    vmid_init();

    local_irq_enable();

    set_current(idle_vcpu[xen_cpuid]);

    intc_init_cpu();

    if ( csr_read_safe(CSR_STIMECMP, &tmp) )
    {
        /*
         * A VS-timer interrupt becomes pending whenever the value of
         * (time + htimedelta) is greater than or equal to vstimecmp CSR.
         * Thereby to avoid spurious VS-timer irqs set vstimecmp CSR to
         * -1.
         *
         * It will be dropped when SSTC support for guest will be added.
         */
        csr_write64(VSTIMECMP, -1UL);
    }

    /* Run local notifiers */
    notify_cpu_starting(xen_cpuid);

    /*
     * Ensure that previous writes are visible before marking the cpu as
     * online.
     */
    smp_wmb();

    /* Now report this CPU is up */
    cpumask_set_cpu(xen_cpuid, &cpu_online_map);

    printk(XENLOG_DEBUG "CPU %u booted.\n", smp_processor_id());

    startup_cpu_idle_loop();
}

void __init smp_prepare_boot_cpu(void)
{
    set_processor_id(0);

    cpumask_set_cpu(0, &cpu_possible_map);
    cpumask_set_cpu(0, &cpu_online_map);
}

/**
 * dt_get_hartid - Get the hartid from a CPU device node
 *
 * @cpun: CPU number(logical index) for which device node is required
 *
 * Return: The hartid for the CPU node or ~0UL if not found.
 */
static unsigned long dt_get_hartid(const struct dt_device_node *cpun)
{
    const __be32 *cell;
    unsigned int ac;
    uint32_t len;

    ac = dt_n_addr_cells(cpun);
    cell = dt_get_property(cpun, "reg", &len);

    /*
     * If ac > 2, the result may be truncated or meaningless unless
     * dt_read_number() supports wider integers.
     *
     * TODO: drop (ac > 2) when dt_read_number() will support wider
     *       integers.
     */
    if ( !cell || !ac || (ac > 2) || (ac > len / sizeof(*cell)) )
        return ~0UL;

    return dt_read_number(cell, ac);
}

/*
 * Returns the hartid of the given device tree node, or -ENODEV if the node
 * isn't an enabled and valid RISC-V hart node.
 */
int dt_processor_hartid(const struct dt_device_node *node,
                        unsigned long *hartid)
{
    const char *isa;
    int ret;

    if ( !dt_device_is_compatible(node, "riscv") )
    {
        printk("Found incompatible CPU\n");
        return -ENODEV;
    }

    *hartid = dt_get_hartid(node);
    if ( *hartid == ~0UL )
    {
        printk("Found CPU without CPU ID\n");
        return -ENODATA;
    }

    if ( !dt_device_is_available(node))
    {
        printk("CPU with hartid=%#lx is not available\n", *hartid);
        return -ENODEV;
    }

    if ( (ret = dt_property_read_string(node, "riscv,isa", &isa)) )
    {
        printk("CPU with hartid=%#lx has no \"riscv,isa\" property\n", *hartid);
        return ret;
    }

    if ( isa[0] != 'r' || isa[1] != 'v' )
    {
        printk("CPU with hartid=%#lx has an invalid ISA of \"%s\"\n", *hartid,
               isa);
        return -ENODEV;
    }

    return 0;
}

void __init smp_init_cpus(void)
{
    struct dt_device_node *cpus = dt_find_node_by_path("/cpus");
    struct dt_device_node *cpu;
    unsigned long hart;
    bool found_boot_cpu = false;
    int cpuid = 1;
    int rc;

    dt_for_each_child_node( cpus, cpu )
    {
        if ( !dt_device_type_is_equal(cpu, "cpu") )
            continue;

        rc = dt_processor_hartid(cpu, &hart);
        if ( rc < 0 )
            continue;

        if ( hart == cpuid_to_hartid(0) )
        {
            BUG_ON(found_boot_cpu);

            found_boot_cpu = 1;

            continue;
        }

        if ( cpuid >= NR_CPUS )
        {
            printk("Invalid cpuid [%d] for hartid [%lu]\n", cpuid, hart);

            continue;
        }

        cpumask_set_cpu(cpuid, &cpu_possible_map);

        set_cpuid_to_hartid(cpuid, hart);

        cpuid++;
    }

    BUG_ON(!found_boot_cpu);

    if ( cpuid > NR_CPUS )
        printk("Total number of cpus [%d] is greater than nr_cpus option value [%d]\n",
               cpuid, NR_CPUS);

    if ( ( !max_cpus ) || ( max_cpus > nr_cpu_ids ) )
        max_cpus = nr_cpu_ids;

    cpuid = 0;

    for ( unsigned int i = 0; i < max_cpus; i++ )
        if ( cpu_possible(i) )
            cpuid++;

    nr_cpu_ids = cpuid;

    printk(XENLOG_INFO "SMP: Allowing %u CPUs\n", nr_cpu_ids);
}

static int setup_cpu_sibling_map(unsigned int cpu)
{
    if ( !zalloc_cpumask_var(&per_cpu(cpu_sibling_mask, cpu)) ||
         !zalloc_cpumask_var(&per_cpu(cpu_core_mask, cpu)) )
        return -ENOMEM;

    cpumask_set_cpu(cpu, per_cpu(cpu_sibling_mask, cpu));
    cpumask_set_cpu(cpu, per_cpu(cpu_core_mask, cpu));

    return 0;
}

void __init smp_prepare_cpus(void)
{
    int rc;

    cpumask_copy(&cpu_present_map, &cpu_possible_map);

    rc = setup_cpu_sibling_map(0);
    if ( rc )
        panic("Unable to allocate CPU sibling/core maps\n");
}

static int cf_check cpu_smpboot_callback(struct notifier_block *nfb,
                                         unsigned long action,
                                         void *hcpu)
{
    unsigned int cpu = (unsigned long)hcpu;
    unsigned int rc = 0;

    switch ( action )
    {
    case CPU_UP_PREPARE:
        rc = setup_cpu_sibling_map(cpu);
        if ( rc )
            printk(XENLOG_ERR
                   "Unable to allocate CPU sibling/core map  for CPU%u\n",
                   cpu);
        break;

    case CPU_DEAD:
        printk("%s: CPU_DEAD case is unimplemented\n", __func__);
        break;

    default:
        break;
    }

    return notifier_from_errno(rc);
}

static struct notifier_block cpu_smpboot_nfb = {
    .notifier_call = cpu_smpboot_callback,
};

static int __init cpu_smpboot_notifier_init(void)
{
    register_cpu_notifier(&cpu_smpboot_nfb);

    return 0;
}
presmp_initcall(cpu_smpboot_notifier_init);
