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

#include <asm/extable.h>
#include <asm/cpufeature.h>
#include <asm/guest_access.h>
#include <asm/intc.h>
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

static void do_unexpected_trap(const struct cpu_user_regs *regs)
{
    dump_csrs("Unhandled exception");
    dump_general_regs(regs);

    die();
}

static void check_for_pcpu_work(void)
{
    struct vcpu *curr = current;

    vcpu_sync_interrupts(curr);

    vcpu_flush_interrupts(curr);

    p2m_handle_vmenter();
}

static void timer_interrupt(void)
{
    /* Disable the timer to avoid more interrupts */
    csr_clear(CSR_SIE, BIT(IRQ_S_TIMER, UL));

    /* Signal the generic timer code to do its work */
    raise_softirq(TIMER_SOFTIRQ);
}

static always_inline void advance_pc(struct cpu_user_regs *regs, int step)
{
    regs->sepc += step;
}

static always_inline unsigned long get_faulting_gpa(void)
{
    /*
     * According to RISC-V spec:
     *  18.2.8. Hypervisor Trap Value Register (htval)
     *   ...
     *   A guest physical address written to htval is shifted right by 2 bits
     *   to accommodate addresses wider than the current XLEN.
     *   ...
     *   If the least-significant two bits of a faulting guest physical address
     *   are needed, these bits are ordinarily the same as the
     *   least-significant two bits of the faulting virtual address in stval.
     *   For faults due to implicit memory accesses for VS-stage address
     *   translation, the least-significant two bits are instead zeros. These
     *   cases can be distinguished using the value provided in register htinst.
     */
    return (csr_read(CSR_HTVAL) << 2) | (csr_read(CSR_STVAL) & 0x3);
}

/*
 * Determine the trapped instruction which caused a guest MMIO trap.
 *
 * Returns true if the trap was redirected to the guest, in which case
 * the caller must stop emulation and return success. Otherwise *insn
 * and *insn_len are filled in and the caller should continue decoding.
 */
static bool decode_trapped_insn(unsigned long htinst, unsigned long *insn,
                                unsigned int *insn_len)
{
    if ( htinst & 0x1 )
    {
        /*
         * Bit[0] == 1 implies trapped instruction value is
         * transformed instruction or custom instruction.
         */
        *insn = htinst | INSN_16BIT_MASK;
        *insn_len = (htinst & BIT(1, UL)) ? INSN_LEN(*insn) : 2;
    }
    else
    {
        struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(current);
        struct trap_info utrap = { 0 };

        /*
         * Bit[0] == 0 implies trapped instruction value is
         * zero or special value.
         */
        *insn = riscv_vcpu_unpriv_read(true, regs->sepc, &utrap);
        if ( utrap.scause )
        {
            /*
             * A G-stage fault here would mean the P2M mapping of the page
             * containing the trapped instruction disappeared after it was
             * fetched. Nothing removes P2M mappings of a running domain yet,
             * so this cannot happen.
             *
             * TODO: Revisit once P2M mappings can be removed at runtime.
             */
            BUG_ON(is_load_guest_page_fault(utrap.scause));

            utrap.sepc = regs->sepc;
            utrap.stval = utrap.sepc;

            riscv_vcpu_trap_redirect(&utrap);

            return true;
        }

        *insn_len = INSN_LEN(*insn);
    }

    return false;
}

/*
 * Check alignment and dispatch a decoded MMIO access to a registered
 * handler. On success (0), info->data holds the read value for loads.
 */
static int do_mmio(mmio_info_t *info, unsigned long fault_addr,
                   unsigned int len)
{
    /* Fault address should be aligned to length of MMIO */
    if ( fault_addr & (len - 1) )
        return -EIO;

    info->gpa = fault_addr;
    info->len = len;

    switch ( try_handle_mmio(info) )
    {
    case IO_HANDLED:
        return 0;
    case IO_ABORT:
        return -EIO;
    default:
        return -EOPNOTSUPP;
    }
}

static int emulate_load(unsigned long fault_addr, unsigned long htinst)
{
    struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(current);
    mmio_info_t info = { .is_write = false };
    unsigned long insn;
    unsigned int shift = 0, len, insn_len;
    bool is_unsigned = false;
    int rc;

    if ( decode_trapped_insn(htinst, &insn, &insn_len) )
        return 0;

    /* Decode length of MMIO and whether it is a sign- or zero-extending load */
    if ( (insn & INSN_MASK_LB) == INSN_MATCH_LB )
        len = 1;
    else if ( (insn & INSN_MASK_LBU) == INSN_MATCH_LBU )
    {
        len = 1;
        is_unsigned = true;
    }
    else if ( (insn & INSN_MASK_LH) == INSN_MATCH_LH )
        len = 2;
    else if ( (insn & INSN_MASK_LHU) == INSN_MATCH_LHU )
    {
        len = 2;
        is_unsigned = true;
    }
    else if ( (insn & INSN_MASK_LW) == INSN_MATCH_LW )
        len = 4;
#ifndef CONFIG_RISCV_32
    else if ( (insn & INSN_MASK_LWU) == INSN_MATCH_LWU )
    {
        len = 4;
        is_unsigned = true;
    }
#endif
    else if ( (insn & INSN_MASK_C_LW) == INSN_MATCH_C_LW )
    {
        len = 4;
        insn = RVC_RS2S(insn) << SH_RD;
    }
    else if ( (insn & INSN_MASK_C_LWSP) == INSN_MATCH_C_LWSP &&
              RV_X(insn, SH_RD, 5) )
        len = 4;
#ifndef CONFIG_RISCV_32
    else if ( (insn & INSN_MASK_LD) == INSN_MATCH_LD )
        len = 8;
    else if ( (insn & INSN_MASK_C_LD) == INSN_MATCH_C_LD )
    {
        len = 8;
        insn = RVC_RS2S(insn) << SH_RD;
    }
    else if ( (insn & INSN_MASK_C_LDSP) == INSN_MATCH_C_LDSP &&
              RV_X(insn, SH_RD, 5) )
        len = 8;
#endif
    else
        return -EOPNOTSUPP;

    if ( !is_unsigned )
        shift = BITS_PER_BYTE * (sizeof(unsigned long) - len);

#ifdef EMULATE_LOAD_DEBUG
    gdprintk(XENLOG_DEBUG, "pc=%#02lx, addr=%#02lx, len=%d, shift=%d\n",
             regs->sepc, fault_addr, len, shift);
#endif

    rc = do_mmio(&info, fault_addr, len);
    if ( rc )
        return rc;

    SET_RD(insn, regs, (long)((unsigned long)info.data << shift) >> shift);

    advance_pc(regs, insn_len);

    return 0;
}

static int emulate_store(unsigned long fault_addr, unsigned long htinst)
{
    struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(current);
    mmio_info_t info = { .is_write = true };
    register_t data;
    unsigned long insn;
    unsigned int len, insn_len;
    int rc;

    if ( decode_trapped_insn(htinst, &insn, &insn_len) )
        return 0;

    data = GET_RS2(insn, regs);

    if ( (insn & INSN_MASK_SB) == INSN_MATCH_SB )
        len = 1;
    else if ( (insn & INSN_MASK_SH) == INSN_MATCH_SH )
        len = 2;
    else if ( (insn & INSN_MASK_SW) == INSN_MATCH_SW )
        len = 4;
    else if ( (insn & INSN_MASK_C_SW) == INSN_MATCH_C_SW )
    {
        len = 4;
        data = GET_RS2S(insn, regs);
    }
    else if ( (insn & INSN_MASK_C_SWSP) == INSN_MATCH_C_SWSP )
    {
        len = 4;
        data = GET_RS2C(insn, regs);
    }
#ifndef CONFIG_RISCV_32
    else if ( (insn & INSN_MASK_SD) == INSN_MATCH_SD )
        len = 8;
    else if ( (insn & INSN_MASK_C_SD) == INSN_MATCH_C_SD )
    {
        len = 8;
        data = GET_RS2S(insn, regs);
    }
    else if ( (insn & INSN_MASK_C_SDSP) == INSN_MATCH_C_SDSP )
    {
        len = 8;
        data = GET_RS2C(insn, regs);
    }
#endif
    else
        return -EOPNOTSUPP;

    info.data = data;

    rc = do_mmio(&info, fault_addr, len);
    if ( rc )
        return rc;

    advance_pc(regs, insn_len);

    return 0;
}

static void handle_guest_page_fault(unsigned long cause,
                                    struct cpu_user_regs *regs)
{
    unsigned long addr;
    int rc;

    addr = get_faulting_gpa();

    switch ( cause )
    {
    case CAUSE_LOAD_GUEST_PAGE_FAULT:
        rc = emulate_load(addr, csr_read(CSR_HTINST));
        break;

    case CAUSE_STORE_GUEST_PAGE_FAULT:
        rc = emulate_store(addr, csr_read(CSR_HTINST));
        break;

    default:
        rc = -EOPNOTSUPP;
        ASSERT_UNREACHABLE();
        break;
    }

    if ( rc )
        domain_crash(current->domain,
                     "%s: unable to handle faulted guest %s addr %#lx\n",
                     __func__,
                     (cause == CAUSE_LOAD_GUEST_PAGE_FAULT) ? "load" : "store",
                     addr);
}

void do_trap(struct cpu_user_regs *cpu_regs)
{
    register_t pc = cpu_regs->sepc;
    unsigned long cause = csr_read(CSR_SCAUSE);

    switch ( cause )
    {
    case CAUSE_VIRTUAL_SUPERVISOR_ECALL:
        /* CAUSE_VIRTUAL_SUPERVISOR_ECALL should come from VS-mode */
        BUG_ON(!(cpu_regs->hstatus & HSTATUS_SPV));

        vsbi_handle_ecall(cpu_regs);
        break;

    case CAUSE_LOAD_GUEST_PAGE_FAULT:
    case CAUSE_STORE_GUEST_PAGE_FAULT:
        handle_guest_page_fault(cause, cpu_regs);
        break;

    case CAUSE_ILLEGAL_INSTRUCTION:
        if ( do_bug_frame(cpu_regs, pc) >= 0 )
        {
            if ( !(is_kernel_text(pc) || is_kernel_inittext(pc)) )
            {
                printk("Something wrong with PC: %#lx\n", pc);
                die();
            }

            cpu_regs->sepc += GET_INSN_LENGTH(*(uint16_t *)pc);

            break;
        }

        if ( fixup_exception(cpu_regs) )
            break;

        fallthrough;
    default:
        if ( cause & CAUSE_IRQ_FLAG )
        {
            /* Handle interrupt */
            unsigned long icause = cause & ~CAUSE_IRQ_FLAG;
            bool intr_handled = true;

            switch ( icause )
            {
            case IRQ_S_EXT:
                intc_handle_external_irqs(cpu_regs);
                break;

            case IRQ_S_TIMER:
                timer_interrupt();
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

    if ( cpu_regs->hstatus & HSTATUS_SPV )
        check_for_pcpu_work();
}

void vcpu_show_execution_state(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void show_execution_state(const struct cpu_user_regs *regs)
{
    printk("TODO: Implement show_execution_state(regs)\n");
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
