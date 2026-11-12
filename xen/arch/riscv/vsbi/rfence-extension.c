/* SPDX-License-Identifier: (GPL-2.0-or-later) */

#include <xen/bitops.h>
#include <xen/lib.h>
#include <xen/sched.h>

#include <asm/current.h>
#include <asm/flushtlb.h>
#include <asm/processor.h>
#include <asm/riscv_encoding.h>
#include <asm/sbi.h>
#include <asm/vmid.h>
#include <asm/vsbi.h>

static int vsbi_rfence_ecall_handler(unsigned long eid, unsigned long fid,
                                     struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    unsigned long hmask = regs->a0;
    unsigned long hbase = regs->a1;
    vaddr_t start = regs->a2;
    size_t size = regs->a3;
    unsigned long asid = regs->a4;
    cpumask_t pcpus = {};
    struct vcpu *v;
    int rc = 0;

    (void) eid;

    switch ( fid )
    {
    case SBI_EXT_RFENCE_REMOTE_FENCE_I:
    case SBI_EXT_RFENCE_REMOTE_SFENCE_VMA:
    case SBI_EXT_RFENCE_REMOTE_SFENCE_VMA_ASID:
        break;
    default:
        /* HFENCE_* is an HS-mode operation; a VS-mode guest shouldn't use it */
        return SBI_ERR_NOT_SUPPORTED;
    }

    if ( hbase == ~0UL )
    {
        for ( unsigned int i = 0; i < d->max_vcpus; i++ )
        {
            v = d->vcpu[i];
            if ( !v || v == current )
                continue;

            if ( v->is_running )
                cpumask_set_cpu(v->processor, &pcpus);
            else
                vmid_flush_vcpu(v);
        }
    }
    else
    {
        for_each_set_bit ( bit, hmask )
        {
            unsigned long hartid = hbase + bit;

            v = domain_vcpu(d, hartid);
            if ( !v )
                return SBI_ERR_INVALID_PARAM;
            if ( v == current )
                continue;

            if ( v->is_running )
                cpumask_set_cpu(v->processor, &pcpus);
            else
                vmid_flush_vcpu(v);
        }
    }

    switch ( fid )
    {
    case SBI_EXT_RFENCE_REMOTE_FENCE_I:
        asm volatile ( "fence.i" ::: "memory" );
        if ( !cpumask_empty(&pcpus) )
            rc = sbi_remote_fence_i(&pcpus);
        break;

    case SBI_EXT_RFENCE_REMOTE_SFENCE_VMA:
        flush_tlb_guest_local();
        if ( !cpumask_empty(&pcpus) )
            rc = sbi_remote_hfence_vvma(&pcpus, start, size);
        break;

    case SBI_EXT_RFENCE_REMOTE_SFENCE_VMA_ASID:
        flush_tlb_guest_local();
        if ( !cpumask_empty(&pcpus) )
            rc = sbi_remote_hfence_vvma_asid(&pcpus, start, size, asid);
        break;
    }

    return rc ? SBI_ERR_FAILURE : SBI_SUCCESS;
}

VSBI_EXT(rfence, SBI_EXT_RFENCE, SBI_EXT_RFENCE, vsbi_rfence_ecall_handler);
