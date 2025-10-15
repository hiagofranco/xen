
/* SPDX-License-Identifier:  GPL-2.0-only */

#include <xen/bitops.h>
#include <xen/event.h>
#include <xen/lib.h>
#include <xen/sched.h>

#include <asm/current.h>
#include <asm/sbi.h>
#include <asm/vsbi.h>

static int vsbi_hsm_ecall_handler(unsigned long eid,
                                  unsigned long fid,
                                  struct cpu_user_regs *regs)
{
    int ret = 0;
    struct domain *d = current->domain;
    struct vcpu *rvcpu = NULL;

    switch ( fid )
    {
    case SBI_EXT_HSM_HART_START:
        for ( unsigned int i = 0; i < d->max_vcpus; i++ )
        {
            if ( d->vcpu[i]->vcpu_id == regs->a0 )
            {
                rvcpu = d->vcpu[i];
                break;
            }
        }

        if ( !rvcpu ||
             (rvcpu == current) ||
             (rvcpu->runstate.state == RUNSTATE_blocked) )
                return -EINVAL;

        if ( rvcpu->runstate.state == RUNSTATE_running )
            return -EBUSY;

        domain_lock(d);
        vcpu_guest_cpu_user_regs(rvcpu)->a0 = rvcpu->vcpu_id;
        vcpu_guest_cpu_user_regs(rvcpu)->a1 = regs->a2;
        vcpu_guest_cpu_user_regs(rvcpu)->sepc = regs->a1;

        rvcpu->is_initialised = 1;

        clear_bit(_VPF_down, &rvcpu->pause_flags);
        domain_unlock(d);

        vcpu_wake(rvcpu);

        printk("%pv has been started. Start address is %#lx\n",
               rvcpu, regs->a1);

        break;

    case SBI_EXT_HSM_HART_STOP:
    case SBI_EXT_HSM_HART_STATUS:
    case SBI_EXT_HSM_HART_SUSPEND:
    default:
        ret = -SBI_ERR_NOT_SUPPORTED;
        printk("%s: Unsupported ecall: FID: #%lx, EID: #%lx\n",
               __func__, fid, eid);
        break;
    }

    return ret;
}

VSBI_EXT(hsm, SBI_EXT_HSM, SBI_EXT_HSM, vsbi_hsm_ecall_handler);
