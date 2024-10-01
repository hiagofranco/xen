/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Vates
 *
 * RISC-V Trap handlers
 */

#include <xen/bug.h>
#include <xen/compiler.h>
#include <xen/lib.h>
#include <xen/nospec.h>
#include <xen/sched.h>
#include <xen/softirq.h>

#include <asm/aia.h>
#include <asm/cpufeature.h>
#include <asm/emulate.h>
#include <asm/extable.h>
#include <asm/intc.h>
#include <asm/ipi.h>
#include <asm/mmio.h>
#include <asm/processor.h>
#include <asm/riscv_encoding.h>
#include <asm/traps.h>
#include <asm/vsbi.h>

/*
 * Initialize the trap handling.
 *
 * The function is called after MMU is enabled.
 */
void trap_init(void)
{
    unsigned long addr = (unsigned long)&handle_trap;

    csr_write(CSR_STVEC, addr);
}

static const char *decode_trap_cause(unsigned long cause)
{
    static const char *const trap_causes[] = {
        [CAUSE_MISALIGNED_FETCH] = "Instruction Address Misaligned",
        [CAUSE_FETCH_ACCESS] = "Instruction Access Fault",
        [CAUSE_ILLEGAL_INSTRUCTION] = "Illegal Instruction",
        [CAUSE_BREAKPOINT] = "Breakpoint",
        [CAUSE_MISALIGNED_LOAD] = "Load Address Misaligned",
        [CAUSE_LOAD_ACCESS] = "Load Access Fault",
        [CAUSE_MISALIGNED_STORE] = "Store/AMO Address Misaligned",
        [CAUSE_STORE_ACCESS] = "Store/AMO Access Fault",
        [CAUSE_USER_ECALL] = "Environment Call from U-Mode",
        [CAUSE_SUPERVISOR_ECALL] = "Environment Call from S-Mode",
        [CAUSE_MACHINE_ECALL] = "Environment Call from M-Mode",
        [CAUSE_FETCH_PAGE_FAULT] = "Instruction Page Fault",
        [CAUSE_LOAD_PAGE_FAULT] = "Load Page Fault",
        [CAUSE_STORE_PAGE_FAULT] = "Store/AMO Page Fault",
        [CAUSE_FETCH_GUEST_PAGE_FAULT] = "Instruction Guest Page Fault",
        [CAUSE_LOAD_GUEST_PAGE_FAULT] = "Load Guest Page Fault",
        [CAUSE_VIRTUAL_INST_FAULT] = "Virtualized Instruction Fault",
        [CAUSE_STORE_GUEST_PAGE_FAULT] = "Guest Store/AMO Page Fault",
    };

    const char *res = cause < ARRAY_SIZE(trap_causes)
                      ? array_access_nospec(trap_causes, cause)
                      : NULL;

    return res ?: "UNKNOWN";
}

static const char *decode_reserved_interrupt_cause(unsigned long irq_cause)
{
    switch ( irq_cause )
    {
    case IRQ_M_SOFT:
        return "M-mode Software Interrupt";
    case IRQ_M_TIMER:
        return "M-mode Timer Interrupt";
    case IRQ_M_EXT:
        return "M-mode External Interrupt";
    default:
        return "UNKNOWN IRQ type";
    }
}

static const char *decode_interrupt_cause(unsigned long cause)
{
    unsigned long irq_cause = cause & ~CAUSE_IRQ_FLAG;

    switch ( irq_cause )
    {
    case IRQ_S_SOFT:
        return "Supervisor Software Interrupt";
    case IRQ_S_TIMER:
        return "Supervisor Timer Interrupt";
    case IRQ_S_EXT:
        return "Supervisor External Interrupt";
    default:
        return decode_reserved_interrupt_cause(irq_cause);
    }
}

static const char *decode_cause(unsigned long cause)
{
    if ( cause & CAUSE_IRQ_FLAG )
        return decode_interrupt_cause(cause);

    return decode_trap_cause(cause);
}

static void dump_general_regs(const struct cpu_user_regs *regs)
{
#define X(regs, name, delim) \
    printk("%-4s: %016lx" delim, #name, (regs)->name)

    X(regs, ra, " "); X(regs, sp, "\n");
    X(regs, gp, " "); X(regs, tp, "\n");
    X(regs, t0, " "); X(regs, t1, "\n");
    X(regs, t2, " "); X(regs, s0, "\n");
    X(regs, s1, " "); X(regs, a0, "\n");
    X(regs, a1, " "); X(regs, a2, "\n");
    X(regs, a3, " "); X(regs, a4, "\n");
    X(regs, a5, " "); X(regs, a6, "\n");
    X(regs, a7, " "); X(regs, s2, "\n");
    X(regs, s3, " "); X(regs, s4, "\n");
    X(regs, s5, " "); X(regs, s6, "\n");
    X(regs, s7, " "); X(regs, s8, "\n");
    X(regs, s9, " "); X(regs, s10, "\n");
    X(regs, s11, " "); X(regs, t3, "\n");
    X(regs, t4, " "); X(regs, t5, "\n");
    X(regs, t6, "\n");

#undef X
}

static void dump_csrs(const char *ctx)
{
    unsigned long v;

#define X(name, csr, fmt, ...) \
    v = csr_read(csr); \
    printk("%-10s: %016lx" fmt, #name, v, ##__VA_ARGS__)

    X(scause, CSR_SCAUSE, " %s[%s]\n", ctx, decode_cause(v));

    X(htval, CSR_HTVAL, " ");  X(htinst, CSR_HTINST, "\n");
    X(hedeleg, CSR_HEDELEG, " "); X(hideleg, CSR_HIDELEG, "\n");
    X(hstatus, CSR_HSTATUS, " [%s%s%s%s%s%s ]\n",
      (v & HSTATUS_VTSR) ? " VTSR" : "",
      (v & HSTATUS_VTVM) ? " VTVM" : "",
      (v & HSTATUS_HU)   ? " HU"   : "",
      (v & HSTATUS_SPVP) ? " SPVP" : "",
      (v & HSTATUS_SPV)  ? " SPV"  : "",
      (v & HSTATUS_GVA)  ? " GVA"  : "");
    X(hgatp, CSR_HGATP, "\n");

    if ( riscv_isa_extension_available(NULL, RISCV_ISA_EXT_smstateen) )
    {
        X(hstateen0, CSR_HSTATEEN0, "\n");
    }

    X(stvec, CSR_STVEC, " "); X(vstvec, CSR_VSTVEC, "\n");
    X(sepc, CSR_SEPC, " "); X(vsepc, CSR_VSEPC, "\n");
    X(stval, CSR_STVAL, " "); X(vstval, CSR_VSTVAL, "\n");
    X(status, CSR_SSTATUS, " "); X(vsstatus, CSR_VSSTATUS, "\n");
    X(satp, CSR_SATP, "\n");
    X(vscause, CSR_VSCAUSE, " [%s]\n", decode_cause(v));

#undef X
}

static void show_registers(const struct cpu_user_regs *regs)
{
    printk("sepc: %016lx", regs->sepc);
    if ( !(regs->hstatus & HSTATUS_SPV) )
        printk(" %pS", _p(regs->sepc));
    printk("\nsstatus: %016lx hstatus: %016lx\n", regs->sstatus, regs->hstatus);

    dump_general_regs(regs);
}

/* Return the top of the Xen stack regs->sp points into. */
static unsigned long stack_top(const struct cpu_user_regs *regs)
{
    const struct cpu_info *info = get_cpu_info();
    unsigned long sp = regs->sp, top = (unsigned long)info;

    /*
     * A vCPU's stack isn't aligned to STACK_SIZE, so its bounds have to be
     * derived from struct cpu_info, which occupies the top of it (see
     * arch_vcpu_create()):
     *
     *   stack = vzalloc(STACK_SIZE);
     *   info = stack + STACK_SIZE - sizeof(*info);
     *
     * (largest address)
     * stack + STACK_SIZE -> +-----------------+ = info + 1
     *                       | struct cpu_info |
     *               info -> +-----------------+ = top, sp of an empty stack
     *                       | used stack      |
     *                       | [...]           |
     *                       | free space      |
     *              stack -> +-----------------+ = (info + 1) - STACK_SIZE
     * (smallest address)
     */
    if ( info && sp <= top && sp > (unsigned long)(info + 1) - STACK_SIZE )
        return top;

    /*
     * Otherwise it is one of the boot stacks, used until vCPU's stack is
     * switched to. They are aligned to STACK_SIZE.
     */
    return ROUNDUP(sp, STACK_SIZE);
}

#ifdef CONFIG_FRAME_POINTER

/*
 * On entry a function saves ra and the frame pointer (s0) of its caller in a
 * frame record at the very top of its frame, and then makes s0 hold the value
 * sp had on entry. Hence the frame pointer of a function is the address right
 * above its frame, its frame record is found right below that address, and
 * the saved frame pointers chain the frame records up to the top of the stack:
 *
 * (largest address)
 * | [...]                  |
 * +------------------------+ <- fp of the caller <-,
 * | ra of the caller       |                       |
 * | fp of its caller       | -> [...]              |
 * | [...]                  |                       |
 * +------------------------+ <- fp                 |
 * | ra                     |    fp - 8             |
 * | fp of the caller       |    fp - 16 -----------'
 * | [...]                  |
 * +------------------------+ <- sp
 * (smallest address)
 *
 * There are GCC versions which don't save ra in a leaf function, in which case
 * the only member of its frame record is fp, stored where ra would otherwise
 * be.
 */
struct stackframe {
    unsigned long fp;
    unsigned long ra;
};

/*
 * Print the call trace of the context @regs were saved for. The interrupted
 * function is identified by sepc, which isn't on the stack, and its callers
 * are found by walking the frame records, starting from the one regs->s0
 * points to. @high is the top of the stack regs->sp points into.
 */
static void show_trace(const struct cpu_user_regs *regs, unsigned long high)
{
    unsigned long fp = regs->s0, low = regs->sp;
    /* Set for the frame of the function @regs were saved in. */
    bool first = true;

    printk("Xen call trace:\n");
    printk("   [<%p>] %pS (PC)\n", _p(regs->sepc), _p(regs->sepc));
    printk("   [<%p>] %pS (RA)\n", _p(regs->ra), _p(regs->ra));

    /*
     * Nothing read from the stack can be trusted, so fp is checked before
     * every use. As the frame record is read from right below fp, fp has to
     * be above low by at least the size of the record. The sum is done on
     * low, as subtracting from a bogus fp may wrap.
     */
    while ( IS_ALIGNED(fp, sizeof(fp)) && fp <= high &&
            fp >= low + sizeof(struct stackframe) )
    {
        /* fp points right above the frame record, not at it. */
        const struct stackframe *frame = (const struct stackframe *)fp - 1;
        unsigned long addr = frame->ra, next = frame->fp;

        if ( first && addr > fp && addr <= high )
            /*
             * A stack address in place of ra means the interrupted function
             * is a leaf one which hasn't saved ra: what is found there is the
             * frame pointer of its caller. The return address is still in ra
             * and has been printed already.
             */
            next = addr;
        else
            printk("   [<%p>] %pS\n", _p(addr), _p(addr));

        /*
         * Finding the return address of do_trap() means fp is the frame
         * pointer of do_trap(), i.e. the value sp had when handle_trap()
         * called it. At that point sp holds the address of the struct
         * cpu_user_regs handle_trap() has filled, so fp is the address of
         * that structure.
         *
         * For a trap taken in Xen the structure is below the top of the
         * stack. For a trap taken from a guest it is in struct cpu_info,
         * i.e. fp is equal to high: it holds guest registers, which a trace
         * of Xen must not be continued from. The guest's s0 saved by
         * do_trap() then doesn't pass the checks of the loop either, ending
         * the trace.
         */
        if ( addr == (unsigned long)ret_from_do_trap &&
             fp + sizeof(*regs) <= high )
        {
            /*
             * do_trap() interrupted Xen: its frame is followed by the
             * registers saved by handle_trap(), which is where the trace of
             * the interrupted context has to be started from.
             */
            regs = _p(fp);

            printk("   [<%p>] %pS (PC)\n", _p(regs->sepc), _p(regs->sepc));
            printk("   [<%p>] %pS (RA)\n", _p(regs->ra), _p(regs->ra));

            low = fp + sizeof(*regs);
            fp = regs->s0;
            first = true;

            continue;
        }

        /*
         * Frames are expected to be found at strictly increasing addresses,
         * which also guarantees the walk to terminate on a corrupted stack.
         */
        low = fp;
        fp = next;
        first = false;
    }

    printk("\n");
}

#else /* !CONFIG_FRAME_POINTER */

/*
 * Without frame pointers the best which can be done is to print everything
 * found on the stack which looks like a pointer into Xen's text.
 */
static void show_trace(const struct cpu_user_regs *regs, unsigned long high)
{
    const unsigned long *stack = _p(regs->sp);

    printk("Enable CONFIG_FRAME_POINTER for precise stack frame\n");

    printk("Xen call trace:\n");
    printk("   [<%p>] %pS (PC)\n", _p(regs->sepc), _p(regs->sepc));
    printk("   [<%p>] %pS (RA)\n", _p(regs->ra), _p(regs->ra));

    for ( ; (unsigned long)stack < high; stack++ )
        if ( is_active_kernel_text(*stack) )
            printk("   [<%p>] %pS\n", _p(*stack), _p(*stack));

    printk("\n");
}

#endif /* CONFIG_FRAME_POINTER */

#define STACK_DUMP_LINES     20
#define STACK_WORDS_PER_LINE 4

static void show_stack(const struct cpu_user_regs *regs)
{
    const unsigned long *stack = _p(regs->sp);
    unsigned long top;
    unsigned int i;

    /* Guest's sp and s0 have nothing to do with Xen's stack. */
    if ( regs->hstatus & HSTATUS_SPV )
        return;

    if ( !IS_ALIGNED(regs->sp, sizeof(*stack)) )
    {
        printk("No stack trace: sp=%p is misaligned\n", stack);
        return;
    }

    top = stack_top(regs);

    printk("Xen stack trace from sp=%p:\n  ", stack);

    for ( i = 0; i < STACK_DUMP_LINES * STACK_WORDS_PER_LINE; i++ )
    {
        if ( (unsigned long)stack >= top )
            break;

        if ( i && !(i % STACK_WORDS_PER_LINE) )
            printk("\n  ");

        printk(" %p", _p(*stack++));
    }

    if ( !i )
        printk("Stack empty.");
    printk("\n");

    show_trace(regs, top);
}

static void do_unexpected_trap(const struct cpu_user_regs *regs)
{
    dump_csrs("Unhandled exception");
    show_execution_state(regs);

    die();
}

static void check_for_pcpu_work(void)
{
    struct vcpu *curr = current;

    ASSERT(!local_irq_is_enabled());

    for ( ; ; )
    {
        /*
         * On every iteration: do_softirq() may switch curr out, and once
         * curr is switched in again, possibly on another pCPU, it returns
         * from do_softirq() and goes on with this loop.
         */
        vintc_enter_guest(curr);

        if ( !softirq_pending(smp_processor_id()) )
            break;

        local_irq_enable();
        do_softirq();
        local_irq_disable();
    }

    vcpu_sync_interrupts(curr);

    vcpu_flush_interrupts(curr);
}

static void timer_interrupt(void)
{
    /* Disable the timer to avoid more interrupts */
    csr_clear(CSR_SIE, BIT(IRQ_S_TIMER, UL));

    /* Signal the generic timer code to do its work */
    raise_softirq(TIMER_SOFTIRQ);
}

void do_trap(struct cpu_user_regs *cpu_regs)
{
    register_t pc = cpu_regs->sepc;
    unsigned long cause = csr_read(CSR_SCAUSE);
    bool from_guest = cpu_regs->hstatus & HSTATUS_SPV;

    /*
     * A synchronous trap taken in Xen context may come from an access done on
     * a vCPU's behalf, e.g. the hlv/hlvx sequences in read_guest(),
     * or from a probing access like csr_read_safe(). Both are covered by
     * exception table entries which record the fault details for the caller
     * and resume execution past the faulting instruction.
     *
     * Traps taken from the guest must never be fixed up: sepc is then a guest
     * address, which the guest could point at an address listed in the
     * exception table, making Xen act on an entry (and, for EX_TYPE_TRAP_INFO,
     * write through a pointer) fully under guest control.
     *
     * Interrupts must be excluded too: one taken at an address which happens
     * to be listed in the exception table would otherwise be "fixed up" as if
     * the access itself had faulted, silently skipping it.
     *
     * Returning early skips check_for_pcpu_work() below, which is correct:
     * that only runs for traps taken from the guest.
     */
    if ( !from_guest && !(cause & CAUSE_IRQ_FLAG) &&
         fixup_exception(cpu_regs, cause) )
        return;

    switch ( cause )
    {
    case CAUSE_VIRTUAL_SUPERVISOR_ECALL:
        /* CAUSE_VIRTUAL_SUPERVISOR_ECALL should come from VS-mode */
        BUG_ON(!from_guest);

        vsbi_handle_ecall(cpu_regs);
        break;

    case CAUSE_FETCH_GUEST_PAGE_FAULT:
    case CAUSE_LOAD_GUEST_PAGE_FAULT:
    case CAUSE_STORE_GUEST_PAGE_FAULT:
        /*
         * A guest page fault taken in Xen context comes from an hlv/hlvx
         * access made on a vCPU's behalf and is dealt with by the
         * fixup_exception() above, so only a guest can get here.
         */
        BUG_ON(!from_guest);

        handle_guest_page_fault(cpu_regs, cause);
        break;

    case CAUSE_VIRTUAL_INST_FAULT:
    {
        int ret;

        BUG_ON(!from_guest);

        ret = handle_virt_instruction_fault(current);
        if ( ret < 0 )
            domain_crash(current->domain,
                         "Couldn't handle CAUSE_VIRTUAL_INST_FAULT: %d\n", ret);
        break;
    }

    case CAUSE_ILLEGAL_INSTRUCTION:
        if ( do_bug_frame(cpu_regs, pc) >= 0 )
        {
            if ( !(is_kernel_text(pc) || is_kernel_inittext(pc)) )
            {
                printk("Something wrong with PC: %#lx\n", pc);
                die();
            }

            cpu_regs->sepc += INSN_LEN(*(const uint16_t *)pc);

            break;
        }

        fallthrough;
    default:
        if ( cause & CAUSE_IRQ_FLAG )
        {
            /* Handle interrupt */
            unsigned long icause = cause & ~CAUSE_IRQ_FLAG;
            bool intr_handled = true;

            switch ( icause )
            {
            case IRQ_S_SOFT:
                ipi_handler();
                break;

            case IRQ_S_EXT:
                intc_handle_external_irqs(cpu_regs);
                break;

            case IRQ_S_TIMER:
                timer_interrupt();
                break;

            case IRQ_S_GEXT:
                hgei_interrupt();
                break;

            default:
                intr_handled = false;
                break;
            }

            if ( intr_handled )
                break;
        }

        do_unexpected_trap(cpu_regs);
        break;
    }

    if ( from_guest )
        check_for_pcpu_work();
}

void vcpu_show_execution_state(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void show_execution_state(const struct cpu_user_regs *regs)
{
    show_registers(regs);
    show_stack(regs);
}

void arch_hypercall_tasklet_result(struct vcpu *v, long res)
{
    BUG_ON("unimplemented");
}

enum mc_disposition arch_do_multicall_call(struct mc_state *state)
{
    BUG_ON("unimplemented");
    return mc_continue;
}

void trap_redirect(const struct trap_info *trap)
{
    struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(current);
    unsigned long vsstatus = csr_read(CSR_VSSTATUS);

    /*
     * Redirecting a trap makes sense only if the trap was taken from
     * virtualized mode, i.e. sret is going to return to VS-mode.
     */
    ASSERT(regs->hstatus & HSTATUS_SPV);

    /*
     * Only synchronous exceptions can be redirected. Interrupts must be
     * injected via hvip instead, so that the hardware itself performs
     * VS-mode trap entry, respecting vsstatus.SIE and the vectored
     * dispatch (BASE + 4 * cause) if vstvec is configured so.
     */
    ASSERT(!(trap->scause & CAUSE_IRQ_FLAG));

    /* Change Guest SSTATUS.SPP bit */
    vsstatus &= ~SSTATUS_SPP;
    if ( regs->sstatus & SSTATUS_SPP )
        vsstatus |= SSTATUS_SPP;

    /* Change Guest SSTATUS.SPIE bit */
    vsstatus &= ~SSTATUS_SPIE;
    if ( vsstatus & SSTATUS_SIE )
        vsstatus |= SSTATUS_SPIE;

    /* Clear Guest SSTATUS.SIE bit */
    vsstatus &= ~SSTATUS_SIE;

    /* Update Guest SSTATUS */
    csr_write(CSR_VSSTATUS, vsstatus);

    /* Update Guest SCAUSE, STVAL, and SEPC */
    csr_write(CSR_VSCAUSE, trap->scause);
    csr_write(CSR_VSTVAL, trap->stval);
    csr_write(CSR_VSEPC, trap->sepc);

    /*
     * Set Guest PC to Guest exception vector.
     *
     * vstvec's MODE field is not part of the address. Exceptions always
     * target BASE regardless of MODE, so mask it off explicitly instead of
     * relying on the hardwired zero bit of sepc to drop it.
     */
    regs->sepc = csr_read(CSR_VSTVEC) & ~STVEC_MODE_MASK;

    /* Set Guest privilege mode to supervisor */
    regs->sstatus |= SSTATUS_SPP;
}
