/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/cpumask.h>
#include <xen/init.h>
#include <xen/livepatch.h>
#include <xen/mm.h>
#include <xen/rcupdate.h>
#include <xen/sections.h>
#include <xen/sched.h>
#include <xen/smp.h>
#include <xen/softirq.h>
#include <xen/tasklet.h>
#include <xen/vmap.h>

#include <asm/aia.h>
#include <asm/aplic.h>
#include <asm/bitops.h>
#include <asm/cpufeature.h>
#include <asm/csr.h>
#include <asm/current.h>
#include <asm/imsic.h>
#include <asm/intc.h>
#include <asm/mmio.h>
#include <asm/processor.h>
#include <asm/riscv_encoding.h>
#include <asm/vmid.h>
#include <asm/sbi.h>
#include <asm/p2m.h>
#include <asm/vtimer.h>

struct csr_masks {
    register_t hedeleg;
    register_t henvcfg;
    register_t hideleg;
    register_t hstateen0;

    struct {
        register_t hstateen0;
    } ro_one;
};

#define HEDELEG_DEFAULT (BIT(CAUSE_MISALIGNED_FETCH, U) | \
                         BIT(CAUSE_FETCH_ACCESS, U) | \
                         BIT(CAUSE_ILLEGAL_INSTRUCTION, U) | \
                         BIT(CAUSE_BREAKPOINT, U) | \
                         BIT(CAUSE_MISALIGNED_LOAD, U) | \
                         BIT(CAUSE_LOAD_ACCESS, U) | \
                         BIT(CAUSE_MISALIGNED_STORE, U) | \
                         BIT(CAUSE_STORE_ACCESS, U) | \
                         BIT(CAUSE_USER_ECALL, U) | \
                         BIT(CAUSE_FETCH_PAGE_FAULT, U) | \
                         BIT(CAUSE_LOAD_PAGE_FAULT, U) | \
                         BIT(CAUSE_STORE_PAGE_FAULT, U))

#define HIDELEG_DEFAULT (MIP_VSSIP | MIP_VSTIP | MIP_VSEIP)

static struct csr_masks __ro_after_init csr_masks;

#define HEDELEG_VALID_MASK ULONG_MAX
#define HIDELEG_VALID_MASK ULONG_MAX
#define HENVCFG_VALID_MASK 0xe0000003000000ffUL
#define HSTATEEN0_VALID_MASK 0xde00000000000007UL

void __init init_csr_masks(void)
{
    /*
     * The mask specifies the bits that may be safely modified without
     * causing side effects.
     *
     * For example, registers such as henvcfg or hstateen0 contain WPRI
     * fields that must be preserved. Any write to the full register must
     * therefore retain the original values of those fields.
     */
#define INIT_CSR_MASK(csr, field) do { \
        register_t old = csr_read_set(CSR_ ## csr, csr ## _VALID_MASK); \
        csr_masks.field = csr_swap(CSR_ ## csr, old); \
    } while (0)

#define INIT_RO_ONE_MASK(csr, field) do { \
        register_t old = csr_read_clear(CSR_ ## csr, csr ## _VALID_MASK); \
        csr_masks.ro_one.field = csr_swap(CSR_ ## csr, old) & \
                                 csr ## _VALID_MASK; \
    } while (0)

    INIT_CSR_MASK(HEDELEG, hedeleg);
    INIT_CSR_MASK(HIDELEG, hideleg);

    INIT_CSR_MASK(HENVCFG, henvcfg);

    if ( riscv_isa_extension_available(NULL, RISCV_ISA_EXT_smstateen) )
    {
        INIT_CSR_MASK(HSTATEEN0, hstateen0);
        INIT_RO_ONE_MASK(HSTATEEN0, hstateen0);
    }

#undef INIT_CSR_MASK
#undef INIT_RO_ONE_MASK
}

static void vcpu_csr_init(struct vcpu *v)
{
    v->arch.hedeleg = HEDELEG_DEFAULT & csr_masks.hedeleg;

    vcpu_guest_cpu_user_regs(v)->hstatus = HSTATUS_SPV | HSTATUS_SPVP;

    v->arch.hideleg = HIDELEG_DEFAULT & csr_masks.hideleg;

    /*
     * VS should access only the time counter directly.
     * Everything else should trap.
     */
    v->arch.hcounteren = HCOUNTEREN_TM;

    if ( riscv_isa_extension_available(NULL, RISCV_ISA_EXT_svpbmt) )
        v->arch.henvcfg = ENVCFG_PBMTE & csr_masks.henvcfg;

    if ( riscv_isa_extension_available(NULL, RISCV_ISA_EXT_smstateen) )
    {
        /* Allow guest to access CSR_SENVCFG */
        register_t hstateen0 = SMSTATEEN0_HSENVCFG;

        if ( riscv_isa_extension_available(NULL, RISCV_ISA_EXT_ssaia) )
            /*
             * If the hypervisor extension is implemented, the same three
             * bits are defined also in hypervisor CSR hstateen0 but concern
             * only the state potentially accessible to a virtual machine
             * executing in privilege modes VS and VU:
             *      bit 60 CSRs siselect and sireg (really vsiselect and
             *             vsireg)
             *      bit 59 CSRs siph and sieh (RV32 only) and stopi (really
             *             vsiph, vsieh, and vstopi)
             *      bit 58 all state of IMSIC guest interrupt files, including
             *             CSR stopei (really vstopei)
             * If one of these bits is zero in hstateen0, and the same bit is
             * one in mstateen0, then an attempt to access the corresponding
             * state from VS or VU-mode raises a virtual instruction exception.
             */
            hstateen0 |= SMSTATEEN0_AIA | SMSTATEEN0_IMSIC | SMSTATEEN0_SVSLCT;

        v->arch.hstateen0 = (hstateen0 & csr_masks.hstateen0) |
                            csr_masks.ro_one.hstateen0;
    }

    v->arch.hie = MIP_SGEIP;
}

static void schedule_tail(struct vcpu *prev);
static void noreturn idle_loop(void);
extern void noreturn return_to_new_vcpu(void);

static void continue_new_vcpu(struct vcpu *prev)
{
    schedule_tail(prev);

    if ( is_idle_vcpu(current) )
        reset_stack_and_jump(idle_loop);
    else
    {
        vcpu_aia_init(current);

        /*
         * During a context switch to a new vCPU, interrupts must be disabled
         * to guarantee that the vCPU's CSR state can be safely restored into
         * the hart without being clobbered by an interrupt trap.
         *
         * For example, when return_to_new_vcpu() finishes, it executes sret.
         * At that point, the hart checks hstatus.SPV=1 and sstatus.SPP=1 in
         * order to return from HS-mode into VS-mode. If an interrupt were to
         * arrive before sret, the trap entry logic would implicitly clear
         * hstatus.SPV to 0. Correctly restoring it afterwards is non-trivial,
         * and if left as 0, sret would incorrectly return to HS-mode instead
         * of VS-mode.
         *
         * To avoid this, interrupts are kept disabled during the restore.
         * Additionally, setting sstatus.SPIE=1 ensures that after sret is
         * executed (as status.SIE will be loaded from SPIE), HS-mode will
         * continue to receive interrupts normally.
         */
        local_irq_disable();
        csr_set(sstatus, SSTATUS_SPIE);

        csr_write(CSR_HSTATUS, vcpu_guest_cpu_user_regs(current)->hstatus);

        reset_stack_and_jump(return_to_new_vcpu);
    }
}

int arch_vcpu_create(struct vcpu *v)
{
    int rc;
    void *stack = vzalloc(STACK_SIZE);

    if ( !stack )
        return -ENOMEM;

    v->arch.cpu_info = stack + STACK_SIZE - sizeof(*v->arch.cpu_info);

    v->arch.xen_saved_context.sp = (register_t)v->arch.cpu_info;
    v->arch.xen_saved_context.ra = (register_t)continue_new_vcpu;

    /* Idle VCPUs don't need the rest of this setup */
    if ( is_idle_vcpu(v) )
        return 0;

    v->arch.last_pcpu = NR_CPUS;

    vcpu_csr_init(v);

    if ( (rc = vcpu_vtimer_init(v)) )
        goto fail;

    if ( (rc = v->domain->arch.vintc->ops->vcpu_init(v)) )
        goto fail;

    return rc;

 fail:
    arch_vcpu_destroy(v);
    return rc;
}

void arch_vcpu_destroy(struct vcpu *v)
{
    vcpu_timer_destroy(v);

    vfree((void *)&v->arch.cpu_info[1] - STACK_SIZE);
}

int vcpu_set_interrupt(struct vcpu *v, unsigned int irq)
{
    bool kick_vcpu;

    /* We only allow VS-mode software, timer, and external interrupts */
    if ( irq != IRQ_VS_SOFT &&
         irq != IRQ_VS_TIMER &&
         irq != IRQ_VS_EXT )
        return -EINVAL;

    kick_vcpu = !test_and_set_bit(irq, v->arch.irqs_pending);

    /*
     * The counterpart of this barrier is the one encoded implicitly in xchg()
     * which is used in consumer part (vcpu_flush_interrupts()).
     */
    smp_wmb();

    kick_vcpu |= !test_and_set_bit(irq, v->arch.irqs_pending_mask);

    if ( kick_vcpu )
        vcpu_kick(v);

    return 0;
}

int vcpu_unset_interrupt(struct vcpu *v, unsigned int irq)
{
    /* We only allow VS-mode software, timer, external interrupts */
    if ( irq != IRQ_VS_SOFT &&
         irq != IRQ_VS_TIMER &&
         irq != IRQ_VS_EXT )
        return -EINVAL;

    clear_bit(irq, v->arch.irqs_pending);
    /*
     * The counterpart of this barrier is the one encoded implicitly in xchg()
     * which is used in consumer part (vcpu_flush_interrupts()).
     */
    smp_wmb();
    set_bit(irq, v->arch.irqs_pending_mask);

    return 0;
}

void vcpu_sync_interrupts(struct vcpu *curr)
{
    unsigned long hvip = csr_read(CSR_HVIP);

    ASSERT(curr == current);

    /*
     * v->arch.vsie is later used to check whether a vCPU has interrupts
     * pending. Therefore, it should be kept up to date at all times,
     * not only during context switches.
     */
    curr->arch.vsie = csr_read(CSR_VSIE);

    /* Sync-up HVIP.VSSIP bit changes done by Guest */
    if ( ((curr->arch.hvip ^ hvip) & BIT(IRQ_VS_SOFT, UL)) &&
         !test_and_set_bit(IRQ_VS_SOFT, &curr->arch.irqs_pending_mask) )
    {
        if ( hvip & BIT(IRQ_VS_SOFT, UL) )
            set_bit(IRQ_VS_SOFT, &curr->arch.irqs_pending);
        else
            clear_bit(IRQ_VS_SOFT, &curr->arch.irqs_pending);
    }

#ifdef CONFIG_RISCV_32
    /*
     * Sync-up AIA high interrupts.
     *
     * It is necessary to do only for CONFIG_RISCV_32 which isn't supported
     * now.
     */
#   error "Update v->arch.vsieh"
#endif
}

void vcpu_flush_interrupts(struct vcpu *curr)
{
    ASSERT(curr == current);

    if ( ACCESS_ONCE(curr->arch.irqs_pending_mask[0]) )
    {
        unsigned long mask = xchg(&curr->arch.irqs_pending_mask[0], 0UL);
        unsigned long val = ACCESS_ONCE(curr->arch.irqs_pending[0]) & mask;
        register_t *hvip = &curr->arch.hvip;

        *hvip &= ~mask;
        *hvip |= val;

        csr_write(CSR_HVIP, *hvip);
    }

#ifdef CONFIG_RISCV_32
    /*
     * Flush AIA high interrupts.
     *
     * It is necessary to do only for CONFIG_RISCV_32 which isn't
     * supported now.
     */
#   error "Update v->arch.hviph"
#endif
}

void vcpu_kick(struct vcpu *v)
{
    bool running = v->is_running;

    vcpu_unblock(v);
    if ( running && v != current )
    {
        perfc_incr(vcpu_kick);
        smp_send_event_check_mask(cpumask_of(v->processor));
    }
}

void sync_local_execstate(void)
{
    /* Nothing to do -- no lazy switching */
}

void sync_vcpu_execstate(struct vcpu *v)
{
    /* Nothing to do -- no lazy switching */
}

int arch_sanitise_domain_config(struct xen_domctl_createdomain *config)
{
    return 0;
}

void arch_domain_destroy(struct domain *d)
{
    printk(XENLOG_WARNING "%s: unimplemented\n", __func__);
}

int arch_domain_create(struct domain *d,
                       struct xen_domctl_createdomain *config,
                       unsigned int flags)
{
    int rc = 0;

    if ( is_idle_domain(d) )
        return 0;

    init_guest_isa(d);

    if ( (rc = p2m_init(d, config)) != 0)
        goto fail;

    if ( (rc = domain_io_init(d, MAX_IO_HANDLER)) != 0 )
        goto fail;

    if ( (rc = domain_vintc_init(d)) )
        goto fail;

    return rc;

 fail:
    arch_domain_destroy(d);
    return rc;
}

void arch_domain_creation_finished(struct domain *d)
{
    /* Nothing to do */
}

static void save_csr_regs(struct vcpu *vcpu)
{
    /* vcpu->arch.hstatus = csr_read(CSR_HSTATUS); */
    vcpu->arch.hedeleg = csr_read(CSR_HEDELEG);
    vcpu->arch.hideleg = csr_read(CSR_HIDELEG);
    vcpu->arch.hvip = csr_read(CSR_HVIP);
    vcpu->arch.hie = csr_read(CSR_HIE);
    vcpu->arch.henvcfg = csr_read(CSR_HENVCFG);
    vcpu->arch.hcounteren = csr_read(CSR_HCOUNTEREN);
    vcpu->arch.htimedelta = csr_read(CSR_HTIMEDELTA);
    vcpu->arch.htval = csr_read(CSR_HTVAL);
    vcpu->arch.htinst = csr_read(CSR_HTINST);
    vcpu->arch.hstateen0 = csr_read(CSR_HSTATEEN0);
#ifdef CONFIG_RISCV_32
    vcpu->arch.henvcfgh = csr_read(CSR_HENVCFGH);
    vcpu->arch.htimedeltah = csr_read(CSR_HTIMEDELTAH);
#endif

    vcpu->arch.vsstatus = csr_read(CSR_VSSTATUS);
    vcpu->arch.vsie = csr_read(CSR_VSIE);
    vcpu->arch.vstvec = csr_read(CSR_VSTVEC);
    vcpu->arch.vsscratch = csr_read(CSR_VSSCRATCH);
    vcpu->arch.vscause = csr_read(CSR_VSCAUSE);
    vcpu->arch.vstval = csr_read(CSR_VSTVAL);
    vcpu->arch.vsepc =  csr_read(CSR_VSEPC);
}

static void restore_csr_regs(struct vcpu *vcpu)
{
    csr_write(CSR_HEDELEG, vcpu->arch.hedeleg);
    csr_write(CSR_HIDELEG, vcpu->arch.hideleg);
    csr_write(CSR_HVIP, vcpu->arch.hvip);
    csr_write(CSR_HIE, vcpu->arch.hie);
    csr_write(CSR_HENVCFG, vcpu->arch.henvcfg);
    csr_write(CSR_HCOUNTEREN, vcpu->arch.hcounteren);
    csr_write(CSR_HTIMEDELTA, vcpu->arch.htimedelta);
    csr_write(CSR_HTVAL, vcpu->arch.htval);
    csr_write(CSR_HTINST, vcpu->arch.htinst);
    csr_write(CSR_HSTATEEN0, vcpu->arch.hstateen0);

#ifdef CONFIG_RISCV_32
    csr_write(CSR_HENVCFGH, vcpu->arch.henvcfgh);
    csr_write(CSR_HTIMEDELTAH, vcpu->arch.htimedeltah);
#endif

    csr_write(CSR_VSSTATUS, vcpu->arch.vsstatus);
    csr_write(CSR_VSIE, vcpu->arch.vsie);
    csr_write(CSR_VSTVEC, vcpu->arch.vstvec);
    csr_write(CSR_VSSCRATCH, vcpu->arch.vsscratch);
    csr_write(CSR_VSCAUSE, vcpu->arch.vscause);
    csr_write(CSR_VSTVAL, vcpu->arch.vstval);
    csr_write(CSR_VSEPC, vcpu->arch.vsepc);
}

static void ctxt_switch_from(struct vcpu *p)
{
    /*
     * When the idle VCPU is running, Xen will always stay in hypervisor
     * mode.
     * Therefore we don't need to save the context of an idle VCPU.
     */
    if ( is_idle_vcpu(p) )
    {
        printk("%s: is_idle_vcpu\n", __func__);
        return;
    }

    p2m_ctxt_switch_from(p);

    vtimer_ctxt_switch_from(p);

    save_csr_regs(p);

    vintc_state_save(p);
}

static void ctxt_switch_to(struct vcpu *n)
{
    /*
     * When the idle VCPU is running, Xen will always stay in hypervisor
     * mode.
     * Therefore we don't need to restore the context of an idle VCPU.
     */
    if ( is_idle_vcpu(n) )
    {
        printk("%s: is_idle_vcpu\n", __func__);

        return;
    }

    /*
     * If this vCPU last ran on a different pCPU, invalidate its VMID so
     * vmid_handle_vmenter() assigns a fresh one from the current pCPU's pool.
     * Without this, two pCPUs could independently assign the same
     * (generation, vmid) pair, generation counters start at the same value
     * on all pCPUs and increment independently, causing TLB contamination.
     */
    if ( n->arch.last_pcpu != smp_processor_id() )
        vmid_flush_vcpu(n);

    vtimer_ctxt_switch_to(n);

    restore_csr_regs(n);

    vintc_state_restore(n);

    p2m_ctxt_switch_to(n);
}

static void schedule_tail(struct vcpu *prev)
{
    unsigned int cpu = smp_processor_id();

    ASSERT(prev != current);

    ctxt_switch_from(prev);

    /*
     * Mark this CPU in next domain's dirty cpumasks before calling
     * ctxt_switch_to(). This avoids a race on things like p2m flushing,
     * which is synchronised on that function.
     */
    if ( prev->domain != current->domain )
        cpumask_set_cpu(cpu, current->domain->dirty_cpumask);
    write_atomic(&current->dirty_cpu, cpu);

    ctxt_switch_to(current);

    tp->guest_cpu_info = current->arch.cpu_info;

    if ( prev->domain != current->domain )
        cpumask_clear_cpu(cpu, prev->domain->dirty_cpumask);
    write_atomic(&prev->dirty_cpu, VCPU_CPU_CLEAN);

    current->arch.last_pcpu = smp_processor_id();

    /*
     * sched_context_switched() internally uses a spinlock,
     * which requires interrupts to be enabled.
     */
    local_irq_enable();

    sched_context_switched(prev, current);
}

void context_switch(struct vcpu *prev, struct vcpu *next)
{
    ASSERT(local_irq_is_enabled());
    ASSERT(prev != next);
    ASSERT(!vcpu_cpu_dirty(next));

    local_irq_disable();

    set_current(next);

    prev = __context_switch(prev, next);

    schedule_tail(prev);
}

static void do_idle(void)
{
    unsigned int cpu = smp_processor_id();

    rcu_idle_enter(cpu);
    /* rcu_idle_enter() can raise TIMER_SOFTIRQ. Process it now. */
    process_pending_softirqs();

    local_irq_disable();
    if ( cpu_is_haltable(cpu) )
    {
        wfi();
    }
    local_irq_enable();

    rcu_idle_exit(cpu);
}

static void noreturn idle_loop(void)
{
    unsigned int cpu = smp_processor_id();

    for ( ; ; )
    {
        if ( cpu_is_offline(cpu) )
            stop_cpu();

        /* Are we here for running vcpu context tasklets, or for idling? */
        if ( unlikely(tasklet_work_to_do(cpu)) )
        {
            do_tasklet();
            /* Livepatch work is always kicked off via a tasklet. */
            check_for_livepatch_work();
        }
        /*
         * Test softirqs twice --- first to see if should even try scrubbing
         * and then, after it is done, whether softirqs became pending
         * while we were scrubbing.
         */
        else if ( !softirq_pending(cpu) && !scrub_free_pages() &&
                  !softirq_pending(cpu) )
            do_idle();

        do_softirq();
    }
}

void startup_cpu_idle_loop(void)
{
    struct vcpu *v = current;

    ASSERT(is_idle_vcpu(v));

    cpumask_set_cpu(v->processor, v->domain->dirty_cpumask);
    write_atomic(&v->dirty_cpu, v->processor);

    tp->guest_cpu_info = current->arch.cpu_info;

    reset_stack_and_jump(idle_loop);
}

void continue_running(struct vcpu *same)
{
    /* Nothing to do. */
}

void arch_domain_unpause(struct domain *d)
{
    /* Nothing to do */
}

bool vcpu_has_interrupts(const struct vcpu *v, uint64_t mask)
{
    unsigned long ie;

   /*
    * This handles the asymmetric layout of RISC-V interrupt registers where
    * local interrupt bits occupy different bit positions in vsie/vsip versus
    * hvip.
    * The shift is a register-layout translation: converting interrupt enable
    * bits from guest-visible (vsip) positions into hypervisor-injection (hvip)
    * positions, so the pending-interrupt bitmap can be checked with a single
    * bitwise AND regardless of which view the bit came from. This produces a
    * mask in hvip-space, which can then be compared against irqs_pending[0]
    * which tracks pending interrupts in the hypervisor's view of the world.
    */
    ie = ((v->arch.vsie & VSIP_VALID_MASK) << VSIP_TO_HVIP_SHIFT) &
         (unsigned long)mask;
    ie |= v->arch.vsie & ~IRQ_LOCAL_MASK & (unsigned long)mask;

    if ( ACCESS_ONCE(v->arch.irqs_pending[0]) &  ie )
        return true;

    /* Check AIA high interrupts */
    return vcpu_aia_has_interrupts(v, mask);
}

static void __init __maybe_unused build_assertions(void)
{
    /*
     * Enforce the requirement documented in struct cpu_info that
     * guest_cpu_user_regs must be the first field.
     */
    BUILD_BUG_ON(offsetof(struct cpu_info, guest_cpu_user_regs));
}
