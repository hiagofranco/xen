/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <xen/lib.h>
#include <xen/shutdown.h>

#include <asm/processor.h>
#include <asm/sbi.h>
#include <asm/system.h>

void machine_halt(void)
{
    sbi_shutdown();

    /* TODO: Cope with sbi_shutdown() not being implemented. */

    for ( ;; )
        asm volatile ( "wfi" );

    unreachable();
}

void machine_restart(unsigned int delay_millisecs)
{
    /*
     * TODO: mdelay(delay_millisecs)
     * TODO: Probe for #SRST support, where sbi_system_reset() has a
     *       shutdown/reboot parameter.
     */

    machine_halt();
}

void stop_cpu(void)
{
    int ret;

    local_irq_disable();

    ret = sbi_hsm_hart_stop();
    if ( ret )
        printk("%s: sbi_hsm_hart_stop() returns error code: %d\n",
               __func__, ret);

    for ( ; ; )
        wfi();

    unreachable();
}
