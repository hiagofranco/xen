
/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/lib.h>
#include <xen/sched.h>

#include <asm/processor.h>
#include <asm/vsbi.h>
#include <asm/vsbi-uart.h>
#include <asm/vtimer.h>

static int vsbi_legacy_ecall_handler(unsigned long eid, unsigned long fid,
                                     struct cpu_user_regs *regs)
{
    int ret = 0;

    switch ( eid )
    {
    case SBI_EXT_0_1_CONSOLE_PUTCHAR:
        vsbi_print_char(regs->a0);
        break;

    case SBI_EXT_0_1_CONSOLE_GETCHAR:
        ret = SBI_ERR_NOT_SUPPORTED;
        break;

    case SBI_EXT_0_1_SET_TIMER:
        vtimer_set_timer(&current->arch.vtimer, regs->a0);
        regs->a0 = SBI_SUCCESS;
        break;

    default:
        /*
         * TODO: domain_crash() is acceptable here while things are still under
         * development.
         * It shouldn't stay like this in the end though: guests should not
         * be punished like this for something Xen hasn't implemented.
         */
        domain_crash(current->domain,
                     "%s: Unsupported legacy ecall: EID: #%#lx\n",
                     __func__, eid);
        break;
    }

    return ret;
}

VSBI_EXT(legacy, SBI_EXT_0_1_SET_TIMER, SBI_EXT_0_1_SHUTDOWN,
         vsbi_legacy_ecall_handler);
