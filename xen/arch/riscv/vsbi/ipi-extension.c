/* SPDX-License-Identifier: (GPL-2.0-or-later) */

#include <xen/bitops.h>
#include <xen/lib.h>
#include <xen/sched.h>

#include <asm/current.h>
#include <asm/processor.h>
#include <asm/riscv_encoding.h>
#include <asm/sbi.h>
#include <asm/vsbi.h>

static int vsbi_ipi_ecall_handler(unsigned long eid, unsigned long fid,
                                  struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    unsigned long hmask = regs->a0;
    unsigned long hbase = regs->a1;
    struct vcpu *v;

    (void) eid;

    if ( fid != SBI_EXT_IPI_SEND_IPI )
        return SBI_ERR_NOT_SUPPORTED;

    if ( hbase == ~0UL )
    {
        for ( unsigned int i = 0; i < d->max_vcpus; i++ )
            vcpu_set_interrupt(d->vcpu[i], IRQ_VS_SOFT);
        return SBI_SUCCESS;
    }

    for_each_set_bit ( bit, hmask )
    {
        unsigned long hartid = hbase + bit;

        v = domain_vcpu(d, hartid);
        if ( !v )
            return SBI_ERR_INVALID_PARAM;

        vcpu_set_interrupt(v, IRQ_VS_SOFT);
    }

    return SBI_SUCCESS;
}

VSBI_EXT(ipi, SBI_EXT_IPI, SBI_EXT_IPI, vsbi_ipi_ecall_handler);
