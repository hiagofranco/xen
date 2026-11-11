/* SPDX-License-Identifier: (GPL-2.0-or-later) */

#include <xen/lib.h>
#include <xen/sched.h>

#include <asm/current.h>
#include <asm/processor.h>
#include <asm/sbi.h>
#include <asm/vsbi.h>
#include <asm/vtimer.h>

static int vsbi_time_ecall_handler(unsigned long eid, unsigned long fid,
                                   struct cpu_user_regs *regs)
{
    (void) eid;

    if ( fid != SBI_EXT_TIME_SET_TIMER )
        return SBI_ERR_NOT_SUPPORTED;

    vtimer_set_timer(&current->arch.vtimer, regs->a0);
    return SBI_SUCCESS;
}

VSBI_EXT(time, SBI_EXT_TIME, SBI_EXT_TIME, vsbi_time_ecall_handler);
