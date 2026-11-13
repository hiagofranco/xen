/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/guest_access.h>
#include <xen/lib.h>
#include <xen/sched.h>

#include <asm/current.h>
#include <asm/guest_access.h>
#include <asm/processor.h>
#include <asm/sbi.h>
#include <asm/vsbi.h>
#include <asm/vsbi-uart.h>

#define DBCN_CHUNK_SIZE 64

static int dbcn_write(struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    unsigned long count = regs->a0;
    paddr_t gpa = (paddr_t)regs->a1 | ((paddr_t)regs->a2 << 32);
    unsigned long done = 0;
    char buf[DBCN_CHUNK_SIZE];

    while ( done < count )
    {
        unsigned long chunk = min(count - done, (unsigned long)sizeof(buf));

        if ( copy_from_guest_phys(d, buf, gpa + done, chunk) != 0 )
            return SBI_ERR_FAILURE;

        for ( unsigned long i = 0; i < chunk; i++ )
            vsbi_print_char(buf[i]);

        done += chunk;
    }

    regs->a1 = done;

    return SBI_SUCCESS;
}

static int dbcn_read(struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    unsigned long count = regs->a0;
    paddr_t gpa = (paddr_t)regs->a1 | ((paddr_t)regs->a2 << 32);
    unsigned long done = 0;
    char buf[DBCN_CHUNK_SIZE];

    while ( done < count )
    {
        unsigned long chunk = min(count - done, (unsigned long)sizeof(buf));
        unsigned long n = 0;
        int c;

        while ( n < chunk && (c = vsbi_uart_get_char(d)) >= 0 )
            buf[n++] = c;

        if ( n == 0 )
            break;

        if ( copy_to_guest_phys(d, gpa + done, buf, n) != 0 )
            return SBI_ERR_FAILURE;

        done += n;
    }

    regs->a1 = done;

    return SBI_SUCCESS;
}

static int vsbi_dbcn_ecall_handler(unsigned long eid, unsigned long fid,
                                   struct cpu_user_regs *regs)
{
    (void) eid;

    switch ( fid )
    {
    case SBI_EXT_DBCN_CONSOLE_WRITE:
        return dbcn_write(regs);

    case SBI_EXT_DBCN_CONSOLE_WRITE_BYTE:
        vsbi_print_char(regs->a0);
        return SBI_SUCCESS;

    case SBI_EXT_DBCN_CONSOLE_READ:
        return dbcn_read(regs);

    default:
        return SBI_ERR_NOT_SUPPORTED;
    }
}

VSBI_EXT(dbcn, SBI_EXT_DBCN, SBI_EXT_DBCN, vsbi_dbcn_ecall_handler);
