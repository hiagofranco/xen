/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RISCV_DOMAIN_H
#define RISCV_DOMAIN_H

#include <xen/mm.h>
#include <xen/spinlock.h>
#include <xen/xmalloc.h>
#include <public/hvm/params.h>

#include <asm/cpufeature.h>
#include <asm/guest-layout.h>
#include <asm/p2m.h>
#include <asm/vsbi-uart.h>
#include <asm/vtimer.h>

struct vcpu_vmid {
    uint64_t generation;
    uint16_t vmid;
};

struct hvm_domain
{
    uint64_t              params[HVM_NR_PARAMS];
};

struct arch_vcpu_io {
};

struct arch_vcpu {
    struct vcpu_vmid vmid;

    /*
     * The last CPU this vCPU ran on. Initialised to CPU_NONE.
     */
    unsigned int last_cpu;

    /*
     * Callee saved registers for Xen's state used to switch from
     * prev's stack to the next's stack during context switch.
     */
    struct
    {
        register_t s0;
        register_t s1;
        register_t s2;
        register_t s3;
        register_t s4;
        register_t s5;
        register_t s6;
        register_t s7;
        register_t s8;
        register_t s9;
        register_t s10;
        register_t s11;
        register_t sp;
        register_t ra;
    } xen_saved_context;

    struct cpu_info *cpu_info;

    struct vtimer vtimer;

    struct vimsic_state *vimsic_state;

    register_t hcounteren;
    register_t hedeleg;
    register_t hideleg;
    uint64_t   henvcfg;
    register_t hstateen0;
    uint64_t   htimedelta;
    register_t hvip;
    register_t hie;

    register_t vsatp;
    register_t vscause;
    register_t vsepc;
    uint64_t   vsie;
    register_t vsiselect;
    register_t vsscratch;
    register_t vsstatus;
    register_t vstval;
    register_t vstvec;

    /*
     * VCPU interrupts
     *
     * We have a lockless approach for tracking pending VCPU interrupts
     * implemented using atomic bitops. The irqs_pending bitmap represent
     * pending interrupts whereas irqs_pending_mask represent bits changed
     * in irqs_pending. Our approach is modeled around multiple producer
     * and single consumer problem where the consumer is the VCPU itself.
     *
     * DECLARE_BITMAP() is needed here to support 64 vCPU local interrupts
     * on RV32 host.
     */
#define RISCV_VCPU_NR_IRQS MAX(BITS_PER_LONG, 64)
    DECLARE_BITMAP(irqs_pending, RISCV_VCPU_NR_IRQS);
    DECLARE_BITMAP(irqs_pending_mask, RISCV_VCPU_NR_IRQS);
};

struct paging_domain {
    spinlock_t lock;
    /* Free pages from the pre-allocated pool */
    struct page_list_head freelist;
    /* Number of pages from the pre-allocated pool */
    unsigned long total_pages;
};

struct arch_domain {
    struct hvm_domain hvm;

    /* Virtual MMU */
    struct p2m_domain p2m;

    struct paging_domain paging;

    const unsigned long *isa;

    struct vintc *vintc;

    struct vmmio *vmmio;

    struct vsbi_uart vsbi_uart;
};

#include <xen/sched.h>

static inline struct vcpu_guest_context *alloc_vcpu_guest_context(void)
{
    return xmalloc(struct vcpu_guest_context);
}

static inline void free_vcpu_guest_context(struct vcpu_guest_context *vgc)
{
    xfree(vgc);
}

struct guest_memory_policy {};
static inline void update_guest_memory_policy(struct vcpu *v,
                                              struct guest_memory_policy *gmp)
{}

static inline void arch_vcpu_block(struct vcpu *v) {}

int vcpu_set_interrupt(struct vcpu *v, unsigned int irq);
int vcpu_unset_interrupt(struct vcpu *v, unsigned int irq);
bool vcpu_has_interrupts(const struct vcpu *v, uint64_t mask);

void vcpu_sync_interrupts(struct vcpu *curr);
void vcpu_flush_interrupts(struct vcpu *curr);

bool hstatus_vsxl_settable(unsigned int vsxl);
unsigned int domain_vsxl(const struct domain *d);

#endif /* RISCV_DOMAIN_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
