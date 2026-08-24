/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * RISC-V instruction emulation for trapped guest accesses
 */

#include <xen/bug.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/csr.h>
#include <asm/current.h>
#include <asm/emulate.h>
#include <asm/riscv_encoding.h>
#include <asm/traps.h>

/*
 * The hardware-reported details of a guest page fault, gathered once by
 * handle_guest_page_fault() and passed down to the emulation of the faulted
 * access.
 */
struct guest_fault {
    /* The guest register state. */
    struct cpu_user_regs *regs;
    /* scause: a fetch, a load or a store/AMO guest page fault. */
    unsigned long cause;
    /*
     * htinst: the trapped instruction in its transformed form, or one of the
     * special values (zero, or a pseudoinstruction).
     */
    unsigned long htinst;
    /* htval: as written by hardware; see resolve_faulting_gpa(). */
    unsigned long htval;
    /* stval: the guest virtual address of the faulting access. */
    unsigned long stval;
    /* The faulting guest physical address, filled by resolve_faulting_gpa(). */
    paddr_t gpa;
};

/*
 * Is @htinst one of the special pseudoinstruction values, reported for a guest
 * page fault taken on an implicit memory access done for VS-stage address
 * translation?
 *
 * It is enough to check only bits[1:0] as according to the spec:
 *
 * The value is one of the special pseudoinstructions defined later, all of
 * which have bits 1:0 equal to 00.
 */
static bool htinst_is_pseudo(unsigned long htinst)
{
    return htinst && !(htinst & 3);
}

/*
 * Reconstruct the guest physical address the access faulted on into @gf->gpa.
 */
static int resolve_faulting_gpa(struct guest_fault *gf)
{
    return -EOPNOTSUPP;
}

static int emulate_load(const struct guest_fault *gf)
{
    return -EOPNOTSUPP;
}

static int emulate_store(const struct guest_fault *gf)
{
    return -EOPNOTSUPP;
}

static void inject_access_fault(const struct guest_fault *gf)
{
    struct trap_info utrap = {
        .sepc = gf->regs->sepc,
        .stval = gf->stval,
    };

    switch ( gf->cause )
    {
    case CAUSE_FETCH_GUEST_PAGE_FAULT:
        utrap.scause = CAUSE_FETCH_ACCESS;
        break;

    case CAUSE_LOAD_GUEST_PAGE_FAULT:
        utrap.scause = CAUSE_LOAD_ACCESS;
        break;

    case CAUSE_STORE_GUEST_PAGE_FAULT:
        utrap.scause = CAUSE_STORE_ACCESS;
        break;

    default:
        domain_crash(current->domain, "Impossible cause (%#lx)?\n", gf->cause);
        return;
    }

    trap_redirect(&utrap);
}

void handle_guest_page_fault(struct cpu_user_regs *regs, unsigned long cause)
{
    struct guest_fault gf = {
        .regs = regs,
        .cause = cause,
        .gpa = INVALID_PADDR,
    };
    int rc;

    gf.htinst = csr_read(CSR_HTINST);
    gf.htval = csr_read(CSR_HTVAL);
    gf.stval = csr_read(CSR_STVAL);

    /*
     * htinst reports one of the special pseudoinstruction values when both:
     *   (a) the fault is caused by an implicit memory access for VS-stage
     *       address translation
     *   (b) htval holds a nonzero value: the guest physical address of that
     *       VS-stage PTE, not of the guest's original access, which is
     *       unknown as VS-stage translation didn't complete.
     *
     * No access to emulate then: to match bare hardware behavior, inject an
     * access fault of the ORIGINAL access type (Instruction, Load, or
     * Store/AMO) that initiated the address translation.
     *
     * FIXME: This assumes that guest page tables never reside in an emulated
     * MMIO region, i.e. that an implicit access faulting at G-stage always
     * targets an unpopulated GPA. Guests may (even transiently) place page
     * tables in MMIO-backed memory, e.g. a video frame buffer. Supporting
     * that would require walking the VS-stage page tables in software,
     * accessing the PTEs (including A/D updates) through the MMIO handlers,
     * and then emulating the original access, instead of injecting a fault.
     */
    if ( htinst_is_pseudo(gf.htinst) )
    {
        inject_access_fault(&gf);

        return;
    }

    rc = resolve_faulting_gpa(&gf);
    if ( rc )
        goto out;

    switch ( cause )
    {
    case CAUSE_LOAD_GUEST_PAGE_FAULT:
        rc = emulate_load(&gf);
        break;

    case CAUSE_STORE_GUEST_PAGE_FAULT:
        rc = emulate_store(&gf);
        break;

    case CAUSE_FETCH_GUEST_PAGE_FAULT:
        /*
         * Guest is trying to reach unmapped/unpopulated, or G-stage PTE doesn't
         * allow execution (X=0). Generate fetch fault in this case.
         */
        inject_access_fault(&gf);
        return;

    default:
        ASSERT_UNREACHABLE();
        rc = -EOPNOTSUPP;

        break;
    }

 out:
    if ( rc )
        domain_crash(current->domain,
                     "unable to handle guest page fault (cause=%#lx) at gpa %#"PRIpaddr"\n",
                     cause, gf.gpa);
}
