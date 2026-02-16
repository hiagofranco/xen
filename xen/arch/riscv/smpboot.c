#include <xen/bootfdt.h>
#include <xen/cpumask.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/param.h>
#include <xen/sections.h>
#include <xen/types.h>

#include <asm/current.h>

cpumask_t __read_mostly cpu_online_map;
cpumask_t __ro_after_init cpu_possible_map;

/* maxcpus: maximum number of CPUs to activate. */
static unsigned int __initdata max_cpus;
integer_param("maxcpus", max_cpus);

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
