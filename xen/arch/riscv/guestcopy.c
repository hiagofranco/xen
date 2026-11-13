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

unsigned long copy_from_guest_phys(struct domain *d, void *buf, paddr_t gpa,
                                   unsigned long len)
{
    return copy_guest(buf, gpa, len, GPA_INFO(d),
                      COPY_from_guest | COPY_gpa);
}

unsigned long read_guest(unsigned long guest_addr, bool read_insn,
                         struct trap_info *trap)
{
    /*
     * Poison the result: if the very first access faults, the fixup skips
     * over the loads without writing it. Callers must check trap->scause.
     */
    unsigned long val = ~0UL, tmp;

    /*
     * hlv/hlvx use hstatus.SPVP for the privilege of the access, and the
     * live vsatp/hgatp for the translation. Xen never installs a value of
     * its own in hstatus (it is only saved on trap entry and restored
     * before sret) and it doesn't reschedule before returning to the
     * guest, so all three still belong to the vCPU which trapped.
     */
    if ( read_insn )
    {
        asm volatile ( "\n"
            "1: hlvx.hu %[val], (%[addr])\n"
            ASM_EXTABLE_TRAP_INFO(1b, 3f, %[ti])
            /*
             * bits[1:0] != 0b11: 16-bit (compressed) instruction, nothing
             * more to read.
             */
            "   andi %[tmp], %[val], 3\n"
            "   addi %[tmp], %[tmp], -3\n"
            "   bnez %[tmp], 3f\n"
            "   addi %[addr], %[addr], 2\n"
            "\n"
            "2: hlvx.hu %[tmp], (%[addr])\n"
            ASM_EXTABLE_TRAP_INFO(2b, 3f, %[ti])
            "   slli %[tmp], %[tmp], 16\n"
            "   or %[val], %[val], %[tmp]\n"
            "3:\n"
        : [val] "+&r" (val), [tmp] "=&r" (tmp), [addr] "+&r" (guest_addr),
          "+m" (*trap)
        : [ti] "r" (trap) );

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
            "1: "
#if defined(CONFIG_RISCV_64)
            "hlv.d %[val], (%[addr])\n"
#elif defined(CONFIG_RISCV_32)
            "hlv.w %[val], (%[addr])\n"
#else
# error "unsupported RISC-V variant: no hlv for a machine word"
#endif
            "2:\n"
            ASM_EXTABLE_TRAP_INFO(1b, 2b, %[ti])
        : [val] "+&r" (val), "+m" (*trap)
        : [addr] "r" (guest_addr), [ti] "r" (trap) );
    }

    return val;
}
