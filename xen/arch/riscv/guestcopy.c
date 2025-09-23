/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/domain_page.h>
#include <xen/page-size.h>
#include <xen/sched.h>
#include <xen/string.h>

#include <asm/guest_access.h>
#include <asm/traps.h>

#define COPY_from_guest     0U
#define COPY_to_guest       BIT(0, U)
#define COPY_gpa            0U
#define COPY_gva            BIT(1, U)

typedef union
{
    struct
    {
        struct vcpu *v;
    } gva;

    struct
    {
        struct domain *d;
    } gpa;
} copy_info_t;

#define GVA_INFO(vcpu) ((copy_info_t) { .gva = { vcpu } })
#define GPA_INFO(domain) ((copy_info_t) { .gpa = { domain } })

static struct page_info *translate_get_page(copy_info_t info, uint64_t addr,
                                            bool gva, bool write)
{
    p2m_type_t p2mt;
    struct page_info *page;

    /*
     * Not implemented yet.
     *
     * If gva == true, the operation will likely require a struct vcpu
     * rather than just a struct domain. For this reason copy_info_t is
     * already passed here instead of only struct domain.
     */
    BUG_ON(gva);

    page = get_page_from_gfn(info.gpa.d, paddr_to_pfn(addr), &p2mt, P2M_ALLOC);

    if ( !page )
        return NULL;

    if ( write ? p2mt != p2m_ram_rw : !p2m_is_ram(p2mt) )
    {
        put_page(page);
        return NULL;
    }

    return page;
}

static unsigned long copy_guest(void *buf, uint64_t addr, unsigned long len,
                                copy_info_t info, unsigned int flags)
{
    unsigned int offset = PAGE_OFFSET(addr);

    BUILD_BUG_ON((sizeof(addr)) < sizeof(vaddr_t));
    BUILD_BUG_ON((sizeof(addr)) < sizeof(paddr_t));

    while ( len )
    {
        void *p;
        unsigned long size = min(len, PAGE_SIZE + 0UL - offset);
        struct page_info *page;

        page = translate_get_page(info, addr, flags & COPY_gva,
                                  flags & COPY_to_guest);
        if ( !page )
            return len;

        p = __map_domain_page(page);
        p += offset;
        if ( flags & COPY_to_guest )
        {
            /*
             * buf will be NULL when the caller request to zero the
             * guest memory.
             */
            if ( buf )
                memcpy(p, buf, size);
            else
                memset(p, 0, size);
        }
        else
            memcpy(buf, p, size);

        unmap_domain_page(p);
        put_page(page);
        len -= size;
        buf += size;
        addr += size;

        /*
         * After the first iteration, guest virtual address is correctly
         * aligned to PAGE_SIZE.
         */
        offset = 0;
    }

    return 0;
}

unsigned long copy_to_guest_phys(struct domain *d, paddr_t gpa, void *buf,
                                 unsigned long len)
{
    return copy_guest(buf, gpa, len, GPA_INFO(d),
                      COPY_to_guest | COPY_gpa);
}

/*
 * Read machine word from Guest memory
 *
 * @read_insn: Flag representing whether we are reading instruction
 * @guest_addr: Guest address to read
 * @trap: Output pointer to trap details
 *
 * The hlv/hlvx instructions translate guest_addr through the live
 * vsatp/hgatp CSRs, so the read is only meaningful for the address
 * space of the currently running vCPU.
 */
unsigned long riscv_vcpu_unpriv_read(bool read_insn,
                                     unsigned long guest_addr,
                                     struct trap_info *trap)
{
    unsigned long val, tmp;
    unsigned long flags, old_hstatus;

    /*
     * As hstatus is going to be changed we don't want an interrupt to occur
     * with guest's hstatus register.
     */
    local_irq_save(flags);

    /*
     * The hypervisor virtual-machine load and store instructions are valid
     * only in M-mode or HS-mode, or in U-mode when hstatus.HU=1. Each
     * instruction performs an explicit memory access as though V=1; i.e.,
     * with the address translation and protection, and the endianness,
     * that apply to memory accesses in either VS-mode or VU-mode.
     * Field SPVP of hstatus controls the privilege level of the access.
     * The explicit memory access is done as though in VU-mode when SPVP=0,
     * and as though in VS-mode when SPVP=1.
     *
     * So it is necessary to restore vCPU's hstatus before execution of
     * hlv* instruction.
     */
    old_hstatus = csr_swap(CSR_HSTATUS,
                           vcpu_guest_cpu_user_regs(current)->hstatus);

    if ( read_insn )
    {
        asm volatile ( "\n"
            "1:\n"
            "   hlvx.hu %[val], (%[addr])\n"
            ASM_EXTABLE_TRAP_INFO(1b, 3f, %[ti])
            "   andi %[tmp], %[val], 3\n"
            "   addi %[tmp], %[tmp], -3\n"
            "   bne %[tmp], zero, 3f\n"
            "   addi %[addr], %[addr], 2\n"
            "\n"
            "2:\n"
            "   hlvx.hu %[tmp], (%[addr])\n"
            ASM_EXTABLE_TRAP_INFO(2b, 3f, %[ti])
            "   sll %[tmp], %[tmp], 16\n"
            "   add %[val], %[val], %[tmp]\n"
            "3:\n"
        : [val] "=&r" (val), [tmp] "=&r" (tmp), [addr] "+&r" (guest_addr)
        : [ti] "r" (trap) : "memory" );

        /*
         * Although HLVX instructions' explicit memory accesses require execute
         * permissions, they still raise the same exceptions as other load
         * instructions, rather than raising fetch exceptions instead.
         */
        if ( trap->scause == CAUSE_LOAD_PAGE_FAULT )
            trap->scause = CAUSE_FETCH_PAGE_FAULT;
    }
    else
    {
        asm volatile ( "\n"
            "1:\n"
#ifdef CONFIG_RISCV_64
            "hlv.d %[val], (%[addr])\n"
#else
            "hlv.w %[val], (%[addr])\n"
#endif
            "2:\n"
            ASM_EXTABLE_TRAP_INFO(1b, 2b, %[ti])
        : [val] "=&r" (val)
        : [addr] "r" (guest_addr), [ti] "r" (trap) : "memory" );
    }

    csr_write(CSR_HSTATUS, old_hstatus);

    local_irq_restore(flags);

    return val;
}
