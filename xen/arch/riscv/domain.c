/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/cpumask.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/sections.h>
#include <xen/sched.h>
#include <xen/smp.h>
#include <xen/vmap.h>

#include <asm/bitops.h>
#include <asm/cpufeature.h>
#include <asm/csr.h>
#include <asm/current.h>
#include <asm/gpr-num.h>
#include <asm/intc.h>
#include <asm/mmio.h>
#include <asm/riscv_encoding.h>
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

/*
 * Return the hstatus.VSXL value encoding the guest's XLEN. Whether the
 * hardware can actually provide that width is a separate question, answered
 * by hstatus_vsxl_settable().
 */
unsigned int domain_vsxl(const struct domain *d)
{
    switch ( d->type )
    {
    case DOMAIN_32BIT:
        return XLEN_FIELD_32;

    case DOMAIN_64BIT:
        return XLEN_FIELD_64;

    /*
     * No default case: -Wswitch then fails the build should a new
     * domain_type (a 128-bit one, in particular) be added without
     * updating this mapping.
     */
    }

    ASSERT_UNREACHABLE();

    /* Never accepted by hstatus_vsxl_settable(). */
    return XLEN_FIELD_RESERVED;
}

#ifdef CONFIG_RISCV_32

/*
 * hstatus.VSXL occupies bits 33:32 and hence does not exist when HSXLEN=32.
 * VSXLEN is then always 32, so there is nothing to probe and no width other
 * than 32 to accept.
 */
void __init init_hstatus_vsxl_settable_mask(void)
{
}

bool hstatus_vsxl_settable(unsigned int vsxl)
{
    return vsxl == XLEN_FIELD_32;
}

static void vcpu_set_vsxl(struct vcpu *v)
{
    ASSERT(is_32bit_domain(v->domain));
}

#else /* CONFIG_RISCV_64 */

/*
 * Bitmap of the hstatus.VSXL values this hardware accepts, indexed by field
 * value. Not one of csr_masks: those are bit masks ANDed into a CSR write,
 * this is a set of legal values of a single WARL field.
 */
static unsigned int __ro_after_init hstatus_vsxl_settable_mask;

/*
 * hstatus.VSXL is WARL and, in particular, an implementation is permitted to
 * make it read-only with a value that always keeps VSXLEN == HSXLEN. Probe
 * once which values can really be set, so that a domain whose width the
 * hardware cannot provide is refused rather than silently run at the wrong
 * XLEN.
 *
 * Every encoding the field can hold is probed, XLEN_FIELD_128 included, so
 * that a guest width added later needs no change here.
 *
 * Like the CSR masks below this assumes all harts behave the same way.
 */
void __init init_hstatus_vsxl_settable_mask(void)
{
    register_t hstatus = csr_read(CSR_HSTATUS);

    /* Encoding 0 is reserved, hence starting at the narrowest width. */
    for ( unsigned int vsxl = XLEN_FIELD_32; vsxl <= XLEN_FIELD_128; vsxl++ )
    {
        register_t test = (hstatus & ~HSTATUS_VSXL) |
                          MASK_INSR(vsxl, HSTATUS_VSXL);

        csr_write(CSR_HSTATUS, test);

        if ( MASK_EXTR(csr_read(CSR_HSTATUS), HSTATUS_VSXL) == vsxl )
            hstatus_vsxl_settable_mask |= BIT(vsxl, U);
    }

    csr_write(CSR_HSTATUS, hstatus);
}

/*
 * Whether the hardware really accepts the given hstatus.VSXL value, i.e.
 * whether a guest of that XLEN can be run at all.
 */
bool hstatus_vsxl_settable(unsigned int vsxl)
{
    return hstatus_vsxl_settable_mask & BIT(vsxl, U);
}

/*
 * Set the guest's XLEN explicitly rather than leaving it to the WARL
 * behaviour of hstatus.VSXL.
 */
static void vcpu_set_vsxl(struct vcpu *v)
{
    struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(v);
    unsigned int vsxl = domain_vsxl(v->domain);

    ASSERT(hstatus_vsxl_settable(vsxl));

    regs->hstatus = (regs->hstatus & ~HSTATUS_VSXL) |
                    MASK_INSR(vsxl, HSTATUS_VSXL);
}

#endif /* CONFIG_RISCV_32 */

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

    /*
     * hstatus.VSXL is left zero here: vCPU0 is created before
     * kernel_probe() reads the guest kernel header, so the domain's type,
     * and hence the guest's XLEN, is not known yet.
     */
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
}

static void schedule_tail(struct vcpu *prev);
static void noreturn idle_loop(void);
void asmlinkage noreturn return_to_new_vcpu(void);

static void continue_new_vcpu(struct vcpu *prev)
{
    schedule_tail(prev);

    if ( is_idle_vcpu(current) )
        reset_stack_and_jump(idle_loop);

    vcpu_set_vsxl(current);

    /*
     * return_to_new_vcpu() sets up hstatus.SPV, sstatus.SPP and sepc so
     * that sret enters the guest in VS-mode. A trap taken in HS-mode
     * overwrites all of them (trap entry clears hstatus.SPV in particular),
     * so interrupts have to stay disabled until sret. They are re-enabled by
     * sret itself, as return_to_new_vcpu() also sets sstatus.SPIE.
     */
    local_irq_disable();

    reset_stack_and_jump(return_to_new_vcpu);
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

    v->arch.last_cpu = CPU_NONE;

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

    /* Sync-up HVIP.VSSIP bit changes done by Guest */
    if ( ((curr->arch.hvip ^ hvip) & BIT(IRQ_VS_SOFT, UL)) &&
         !test_and_set_bit(IRQ_VS_SOFT, &curr->arch.irqs_pending_mask) )
    {
        if ( hvip & BIT(IRQ_VS_SOFT, UL) )
            set_bit(IRQ_VS_SOFT, &curr->arch.irqs_pending);
        else
            clear_bit(IRQ_VS_SOFT, &curr->arch.irqs_pending);
    }
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

int arch_sanitise_domain_config(const struct xen_domctl_createdomain *config)
{
    return 0;
}

void arch_domain_destroy(struct domain *d)
{
    printk(XENLOG_WARNING "%s: not fully implemented\n", __func__);

    domain_vintc_deinit(d);

    domain_io_free(d);
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

    if ( (rc = domain_io_init(d)) )
        goto fail;

    if ( (rc = domain_vintc_init(d)) )
        goto fail;

    return rc;

 fail:
    arch_domain_destroy(d);
    return rc;
}

/*
 * vsiselect is a per-hart CSR, but a guest changes it on its own: when V=1,
 * VS-mode accesses to siselect are really accesses to vsiselect. Hence it is
 * part of the vCPU context.
 *
 * When Smstateen is implemented, hstateen0.CSRIND (SMSTATEEN0_SVSLCT) gates
 * that access, and a bit staying clear in v->arch.hstateen0 (see
 * vcpu_csr_init()) means either that the guest was never given access to the
 * CSR, and so can't have changed it, or that M-mode denied the state
 * altogether, in which case the CSR can't be accessed from HS-mode either.
 */
static bool vcpu_can_access_vsiselect(const struct vcpu *v)
{
    if ( !riscv_isa_extension_available(NULL, RISCV_ISA_EXT_ssaia) )
        return false;

    if ( !riscv_isa_extension_available(NULL, RISCV_ISA_EXT_smstateen) )
        return true;

    return v->arch.hstateen0 & SMSTATEEN0_SVSLCT;
}

static void csrs_ctxt_switch_from(struct vcpu *p)
{
    /*
     * hedeleg, hideleg, henvcfg, hcounteren, htimedelta and hstateen0 aren't
     * saved: neither the guest nor Xen changes them other than by
     * csrs_ctxt_switch_to() writing them from v->arch.*, which is left
     * unchanged after vcpu_csr_init().
     */

    p->arch.hvip = csr_read(CSR_HVIP);

    p->arch.vsstatus = csr_read(CSR_VSSTATUS);
    p->arch.vsie = csr_read64(VSIE);
    p->arch.vstvec = csr_read(CSR_VSTVEC);
    p->arch.vsscratch = csr_read(CSR_VSSCRATCH);
    p->arch.vscause = csr_read(CSR_VSCAUSE);
    p->arch.vstval = csr_read(CSR_VSTVAL);
    p->arch.vsepc = csr_read(CSR_VSEPC);

    if ( vcpu_can_access_vsiselect(p) )
        p->arch.vsiselect = csr_read(CSR_VSISELECT);
}

static void csrs_ctxt_switch_to(struct vcpu *n)
{
    csr_write(CSR_HEDELEG, n->arch.hedeleg);
    csr_write(CSR_HIDELEG, n->arch.hideleg);
    csr_write(CSR_HVIP, n->arch.hvip);
    csr_write64(HENVCFG, n->arch.henvcfg);
    csr_write(CSR_HCOUNTEREN, n->arch.hcounteren);
    csr_write64(HTIMEDELTA, n->arch.htimedelta);

    if ( riscv_isa_extension_available(NULL, RISCV_ISA_EXT_smstateen) )
        csr_write(CSR_HSTATEEN0, n->arch.hstateen0);

    csr_write(CSR_VSSTATUS, n->arch.vsstatus);
    csr_write64(VSIE, n->arch.vsie);
    csr_write(CSR_VSTVEC, n->arch.vstvec);
    csr_write(CSR_VSSCRATCH, n->arch.vsscratch);
    csr_write(CSR_VSCAUSE, n->arch.vscause);
    csr_write(CSR_VSTVAL, n->arch.vstval);
    csr_write(CSR_VSEPC, n->arch.vsepc);

    if ( vcpu_can_access_vsiselect(n) )
        csr_write(CSR_VSISELECT, n->arch.vsiselect);
}

static void ctxt_switch_from(struct vcpu *p)
{
    /*
     * When the idle VCPU is running, Xen will always stay in hypervisor
     * mode.
     * Therefore we don't need to save the context of an idle VCPU.
     */
    if ( is_idle_vcpu(p) )
        return;

    p2m_ctxt_switch_from(p);

    vtimer_ctxt_switch_from(p);

    vintc_ctxt_switch_from(p);

    csrs_ctxt_switch_from(p);
}

static void ctxt_switch_to(struct vcpu *n)
{
    /*
     * When the idle VCPU is running, Xen will always stay in hypervisor
     * mode.
     * Therefore we don't need to restore the context of an idle VCPU.
     */
    if ( is_idle_vcpu(n) )
        return;

    csrs_ctxt_switch_to(n);

    vintc_ctxt_switch_to(n);

    vtimer_ctxt_switch_to(n);

    p2m_ctxt_switch_to(n);
}

static void schedule_tail(struct vcpu *prev)
{
    unsigned int cpu = smp_processor_id();

    ASSERT(prev != current);

    ctxt_switch_from(prev);

    write_atomic(&prev->dirty_cpu, VCPU_CPU_CLEAN);

    ctxt_switch_to(current);

    write_atomic(&current->dirty_cpu, cpu);

    current->arch.last_cpu = cpu;

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

static void __init __maybe_unused build_assertions(void)
{
/*
 * GPR_LIST() puts nothing between its entries, so the separator has to come
 * from here. It is emitted ahead of each entry rather than after it, so that
 * the expansion doesn't end with one and its use below can be terminated like
 * an ordinary statement.
 */
#define CHECK_GPR_INDEX(num, name)                          \
    ; BUILD_BUG_ON(offsetof(struct cpu_user_regs, name) !=  \
                   offsetof(struct cpu_user_regs, zero) +   \
                   (num) * sizeof(unsigned long))

    /*
     * Enforce the requirement documented in struct cpu_info that
     * guest_cpu_user_regs must be the first field.
     */
    BUILD_BUG_ON(offsetof(struct cpu_info, guest_cpu_user_regs));

    /*
     * Enforce the requirement documented in struct cpu_user_regs that the
     * x0..x31 fields are contiguous and in architectural register-number
     * order, which is what lets a register number be resolved to its saved
     * value by indexing them as an array anchored at ->zero.
     */
    GPR_LIST(CHECK_GPR_INDEX);

#undef CHECK_GPR_INDEX
}

static void noreturn idle_loop(void)
{
    BUG_ON("unimplemented");
}
