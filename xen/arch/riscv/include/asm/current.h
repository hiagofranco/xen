/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef RISCV_CURRENT_H
#define RISCV_CURRENT_H

#include <xen/bug.h>
#include <xen/cache.h>
#include <xen/percpu.h>

#include <asm/processor.h>

#ifndef __ASSEMBLER__

register struct pcpu_info *tp asm ( "tp" );

struct pcpu_info {
    unsigned int processor_id; /* Xen CPU id */
    unsigned long hart_id; /* physical CPU id */

    /*
     * struct cpu_info of the vCPU currently running on this hart, the idle
     * vCPU included. It occupies the topmost sizeof(struct cpu_info) bytes
     * of that vCPU's stack, and so is also the stack pointer Xen starts
     * from when a trap is taken from a guest.
     *
     * Kept in sync with current->arch.cpu_info by set_current().
     */
    struct cpu_info *curr_cpu_info;

    unsigned long tmp;
} __cacheline_aligned;

/* tp points to one of these */
extern struct pcpu_info pcpu_info[NR_CPUS];

/* Per-VCPU state that lives at the top of the stack */
struct cpu_info {
    /* This should be the first member. */
    struct cpu_user_regs guest_cpu_user_regs;
};

#define set_processor_id(id)    do { \
    tp->processor_id = (id);         \
} while (0)

static inline unsigned int smp_processor_id(void)
{
    unsigned int id = tp->processor_id;

    /* An entry whose processor_id isn't valid yet holds CPU_NONE. */
    BUILD_BUG_ON(CPU_NONE < NR_CPUS);
    BUG_ON(id >= NR_CPUS);

    return id;
}

/* Which VCPU is "current" on this PCPU. */
DECLARE_PER_CPU(struct vcpu *, curr_vcpu);

#define current            this_cpu(curr_vcpu)
#define get_cpu_current(cpu)  per_cpu(curr_vcpu, cpu)

/*
 * curr_cpu_info has to be updated together with current: get_cpu_info()
 * reads the former, and handle_trap() uses it to find the stack to switch
 * to when a trap comes in from a guest, so the two must never disagree.
 */
#define set_current(v)                          \
    do {                                        \
        struct vcpu *v_ = (v);                  \
                                                \
        current = v_;                           \
        tp->curr_cpu_info = v_->arch.cpu_info;  \
    } while ( 0 )

static inline struct cpu_info *get_cpu_info(void)
{
    return tp->curr_cpu_info;
}

#define guest_cpu_user_regs() ({ BUG_ON("unimplemented"); NULL; })
#define vcpu_guest_cpu_user_regs(vcpu) \
    (&(vcpu)->arch.cpu_info->guest_cpu_user_regs)

#define switch_stack_and_jump(stack, fn) do {               \
    asm volatile (                                          \
            "mv sp, %0\n"                                   \
            "jr %1" :: "r" (stack), "r" (fn) : "memory" );  \
    unreachable();                                          \
} while ( false )

#define reset_stack_and_jump(fn) switch_stack_and_jump(get_cpu_info(), fn)

#define get_per_cpu_offset() __per_cpu_offset[smp_processor_id()]

#endif /* __ASSEMBLER__ */

#endif /* RISCV_CURRENT_H */
