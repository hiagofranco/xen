/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/acpi.h>
#include <xen/bug.h>
#include <xen/bootinfo.h>
#include <xen/compile.h>
#include <xen/console.h>
#include <xen/device_tree.h>
#include <xen/domain.h>
#include <xen/dom0less-build.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/mm.h>
#include <xen/rcupdate.h>
#include <xen/sched.h>
#include <xen/serial.h>
#include <xen/shutdown.h>
#include <xen/smp.h>
#include <xen/static-evtchn.h>
#include <xen/tasklet.h>
#include <xen/timer.h>
#include <xen/virtual_region.h>
#include <xen/vmap.h>
#include <xen/xvmalloc.h>

#include <public/version.h>

#include <asm/extable.h>
#include <asm/cpufeature.h>
#include <asm/early_printk.h>
#include <asm/fixmap.h>
#include <asm/intc.h>
#include <asm/page.h>
#include <asm/p2m.h>
#include <asm/sbi.h>
#include <asm/setup.h>
#include <asm/traps.h>
#include <asm/vsbi.h>

/* Xen stack for bringing up the first CPU. */
unsigned char __initdata cpu0_boot_stack[STACK_SIZE]
    __aligned(STACK_SIZE);

/**
 * copy_from_paddr - copy data from a physical address
 * @dst: destination virtual address
 * @paddr: source physical address
 * @len: length to copy
 */
void __init copy_from_paddr(void *dst, paddr_t paddr, unsigned long len)
{
    const void *src = (void *)FIXMAP_ADDR(FIX_MISC);

    while ( len )
    {
        unsigned long s = paddr & (PAGE_SIZE - 1);
        unsigned long l = min(PAGE_SIZE - s, len);

        set_fixmap(FIX_MISC, maddr_to_mfn(paddr), PAGE_HYPERVISOR_RW);
        memcpy(dst, src + s, l);
        clear_fixmap(FIX_MISC);

        paddr += l;
        dst += l;
        len -= l;
    }
}

/* Relocate the FDT in Xen heap */
static void * __init relocate_fdt(paddr_t dtb_paddr, size_t dtb_size)
{
    void *fdt = xvmalloc_array(uint8_t, dtb_size);

    if ( !fdt )
        panic("Unable to allocate memory for relocating the Device-Tree.\n");

    copy_from_paddr(fdt, dtb_paddr, dtb_size);

    return fdt;
}

static void __init remove_early_mappings(void)
{
    int rc;

    rc = destroy_xen_mappings(BOOT_FDT_VIRT_START,
                              BOOT_FDT_VIRT_START + BOOT_FDT_VIRT_SIZE);

    BUG_ON(rc);
}

static __used void noreturn init_done(void)
{
    ASSERT(system_state == SYS_STATE_active);

    /* Must be done past setting system_state. */
    unregister_init_virtual_region();

    free_init_memory();

    modify_after_init_mappings();

    startup_cpu_idle_loop();
}

void __init noreturn start_xen(unsigned long bootcpu_id,
                               paddr_t dtb_addr)
{
    const char *cmdline;
    size_t fdt_size;
    int rc;
    struct domain *d;

    remove_identity_mapping();

    smp_prepare_boot_cpu();

    sort_exception_tables();

    set_cpuid_to_hartid(0, bootcpu_id);

    trap_init();

    sbi_init();

    setup_fixmap_mappings();

    device_tree_flattened = early_fdt_map(dtb_addr);
    if ( !device_tree_flattened )
        panic("Invalid device tree blob at physical address %#lx. The DTB must be 8-byte aligned and must not exceed %lld bytes in size.\n\n"
              "Please check your bootloader.\n",
              dtb_addr, BOOT_FDT_VIRT_SIZE);

    /* Register Xen's load address as a boot module. */
    if ( !add_boot_module(BOOTMOD_XEN, virt_to_maddr(_start),
                          _end - _start, false) )
        panic("Failed to add BOOTMOD_XEN\n");

    fdt_size = boot_fdt_info(device_tree_flattened, dtb_addr);

    cmdline = boot_fdt_cmdline(device_tree_flattened);
    printk("Command line: %s\n", cmdline);
    cmdline_parse(cmdline);

    setup_mm();

    vm_init();

    end_boot_allocator();

    check_vsbi_ext_ranges();

    /*
     * The memory subsystem has been initialized, we can now switch from
     * early_boot -> boot.
     */
    system_state = SYS_STATE_boot;

    init_constructors();

    if ( acpi_disabled )
    {
        printk("Booting using Device Tree\n");
        device_tree_flattened = relocate_fdt(dtb_addr, fdt_size);
        dt_unflatten_host_device_tree();
    }
    else
    {
        device_tree_flattened = NULL;
        panic("Booting using ACPI isn't supported\n");
    }

    tasklet_subsys_init();

    init_IRQ();

    riscv_fill_hwcap();

    init_csr_masks();

    init_hstatus_vsxl_settable_mask();

    preinit_xen_time();

    intc_preinit();

    rc = uart_init();
    if ( rc )
        panic("Failed to initialize the requested UART (%d)\n", rc);

    console_init_preirq();

    intc_init();

    timer_init();

    rcu_init();

    setup_system_domains();

    local_irq_enable();

    console_init_postirq();

    guest_mm_init();

    scheduler_init();
    set_current(idle_vcpu[0]);

    do_initcalls();

    if ( !is_dom0less_mode() )
        panic("Only Dom0Less mode is supported now\n");
    else
        printk(XENLOG_INFO "Xen dom0less mode detected\n");

    /*
     * Flush guest TLB once as it isn't guruanteed by spec. that it will
     * be empty, so flush guest TLB for all harts.
     */
    sbi_remote_hfence_gvma(NULL, 0, 0);

    if ( acpi_disabled )
    {
        create_domUs();
        alloc_static_evtchn();
    }

    /*
     * This needs to be called **before** heap_init_late() so modules
     * will be scrubbed (unless suppressed).
     */
    discard_initial_modules();

    remove_early_mappings();

    heap_init_late();

    console_endboot();

    /* Hide UART from DOM0 if we're using it */
    serial_endboot();

    system_state = SYS_STATE_active;

    for_each_domain( d )
        domain_unpause_by_systemcontroller(d);

    printk("All set up\n");

    switch_stack_and_jump(idle_vcpu[0]->arch.cpu_info, init_done);

    machine_halt();
}
