[33mcommit 90eb0576a429960e44b23324837f88826da200ba[m[33m ([m[1;36mHEAD[m[33m)[m
Author: Oleksii Kurochko <oleksii.kurochko@gmail.com>
Date:   Thu Nov 13 13:45:41 2025 +0100

    xen/riscv: handle virtual instruction faults and emulate WFI for guests
    
    Implement handling of CAUSE_VIRTUAL_INST_FAULT traps for RISC-V guests
    and introduce emulation for the WFI (Wait-For-Interrupt) instruction.
    
    When a guest executes a privileged instruction such as WFI, hypervisor
    now intercepts the trap, decodes the instruction, and performs the
    appropriate emulation or redirection:
    - WFI emulation:
      The vCPU is blocked if no pending interrupts are available and will
      resume once an interrupt is injected.
    - Illegal or unsupported instructions:
      Redirected to the guest via riscv_vcpu_trap_redirect() as an illegal
      instruction exception.
    - Instruction fetch:
      If stval does not contain a valid instruction (as permitted by the
      RISC-V Privileged Spec), the instruction is fetched directly from
      guest memory.
    - Compressed instructions (RVC):
      Are treated as unprivileged and redirected to the guest.
    
    Additionally, add instruction decoding helpers (INSN_OPCODE_MASK,
    INSN_OPCODE_SYSTEM, etc.) to riscv_encoding.h.
    
    Updated vcpu_csr_init() to include HSTATUS_VTW, enabling WFI
    instruction trapping.
    
    This implementation is based on the Linux KVM change
    5a5d79acd ("RISC-V: KVM: Handle WFI exits for VCPU") with some
    minor changes:
    - Added explanatory comments.
    - Removed the "if (unlikely(INSN_IS_16BIT(insn)))" check around the condition
      that tests whether `insn` is zero, since it is unnecessary. Regardless of
      whether the instruction is 16-bit or 32-bit, we still need to read the full
      instruction from memory to verify whether it actually contains all zeros.
    - Adapted several functions/macros for Xen code.
    - Use BUG_ON() instead of an if-statement to verify that a virtual instruction
      fault actually occurred while the guest was running. This check is not very
      useful, since such a fault is expected to happen only when the guest is
      executing according to the RISC-V privilige spec.
    
    Signed-off-by: Oleksii Kurochko <oleksii.kurochko@gmail.com>

[1mdiff --git a/xen/arch/riscv/domain.c b/xen/arch/riscv/domain.c[m
[1mindex f5765f20a372..08d783162bbf 100644[m
[1m--- a/xen/arch/riscv/domain.c[m
[1m+++ b/xen/arch/riscv/domain.c[m
[36m@@ -52,6 +52,8 @@[m [mstruct csr_masks {[m
 [m
 #define HIDELEG_DEFAULT (MIP_VSSIP | MIP_VSTIP | MIP_VSEIP)[m
 [m
[32m+[m[32m#define HSTATUS_DEFAULT (HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_VTW)[m
[32m+[m
 static struct csr_masks __ro_after_init csr_masks;[m
 [m
 #define HEDELEG_VALID_MASK ULONG_MAX[m
[36m@@ -99,7 +101,7 @@[m [mstatic void vcpu_csr_init(struct vcpu *v)[m
 {[m
     v->arch.hedeleg = HEDELEG_DEFAULT & csr_masks.hedeleg;[m
 [m
[31m-    vcpu_guest_cpu_user_regs(v)->hstatus = HSTATUS_SPV | HSTATUS_SPVP;[m
[32m+[m[32m    vcpu_guest_cpu_user_regs(v)->hstatus = HSTATUS_DEFAULT;[m
 [m
     v->arch.hideleg = HIDELEG_DEFAULT & csr_masks.hideleg;[m
 [m
[1mdiff --git a/xen/arch/riscv/include/asm/riscv_encoding.h b/xen/arch/riscv/include/asm/riscv_encoding.h[m
[1mindex a0f372684ccb..cd15dcd573e4 100644[m
[1m--- a/xen/arch/riscv/include/asm/riscv_encoding.h[m
[1m+++ b/xen/arch/riscv/include/asm/riscv_encoding.h[m
[36m@@ -12,6 +12,10 @@[m
 #define _UL(X) _AC(X, UL)[m
 #define _ULL(X) _AC(X, ULL)[m
 [m
[32m+[m[32m#define INSN_OPCODE_MASK       _UL(0x007c)[m
[32m+[m[32m#define INSN_OPCODE_SHIFT      2[m
[32m+[m[32m#define INSN_OPCODE_SYSTEM     28[m
[32m+[m
 /* Status register flags */[m
 #define SR_SIE		_UL(0x00000002) /* Supervisor Interrupt Enable */[m
 #define SR_SPIE		_UL(0x00000020) /* Previous Supervisor IE */[m
[1mdiff --git a/xen/arch/riscv/traps.c b/xen/arch/riscv/traps.c[m
[1mindex 2a7e742c3803..7f26a030b9d9 100644[m
[1m--- a/xen/arch/riscv/traps.c[m
[1m+++ b/xen/arch/riscv/traps.c[m
[36m@@ -478,6 +478,94 @@[m [mstatic void handle_guest_page_fault(unsigned long cause, struct cpu_user_regs *r[m
               (cause == CAUSE_LOAD_GUEST_PAGE_FAULT) ? "load" : "store", addr);[m
 }[m
 [m
[32m+[m[32mstatic int truly_illegal_insn(struct vcpu *v, const unsigned long insn)[m
[32m+[m[32m{[m
[32m+[m[32m    struct trap_info utrap = { 0 };[m
[32m+[m
[32m+[m[32m    /* Redirect trap to Guest VCPU */[m
[32m+[m[32m    utrap.sepc = vcpu_guest_cpu_user_regs(v)->sepc;[m
[32m+[m[32m    utrap.scause = CAUSE_ILLEGAL_INSTRUCTION;[m
[32m+[m[32m    utrap.stval = insn;[m
[32m+[m[32m    utrap.htval = 0;[m
[32m+[m[32m    utrap.htinst = 0;[m
[32m+[m[32m    riscv_vcpu_trap_redirect(v, &utrap);[m
[32m+[m
[32m+[m[32m    return 0;[m
[32m+[m[32m}[m
[32m+[m
[32m+[m[32mstatic int system_opcode_insn(struct vcpu *v, unsigned long insn)[m
[32m+[m[32m{[m
[32m+[m[32m    if ( (insn & INSN_MASK_WFI) == INSN_MATCH_WFI )[m
[32m+[m[32m    {[m
[32m+[m[32m        /* Emulate wait for interrupt (WFI) behaviour. */[m
[32m+[m[32m        if ( vcpu_runnable(v) && (v->runstate.state == RUNSTATE_running) &&[m
[32m+[m[32m             !vcpu_has_interrupts(v, _UL(-1)) )[m
[32m+[m[32m        {[m
[32m+[m[32m            vcpu_block();[m
[32m+[m[32m        }[m
[32m+[m
[32m+[m[32m        vcpu_guest_cpu_user_regs(v)->sepc += INSN_LEN(insn);[m
[32m+[m
[32m+[m[32m        return 0;[m
[32m+[m[32m    }[m
[32m+[m
[32m+[m[32m    return truly_illegal_insn(v, insn);[m
[32m+[m[32m}[m
[32m+[m
[32m+[m[32mstatic int handle_virt_instruction_fault(struct vcpu *v)[m
[32m+[m[32m{[m
[32m+[m[32m    unsigned long insn = csr_read(CSR_STVAL);[m
[32m+[m
[32m+[m[32m    /*[m
[32m+[m[32m     * According to the RISC-V Privileged Architecture spec:[m
[32m+[m[32m     *  The stval register can optionally also be used to return the[m
[32m+[m[32m     *  faulting instruction bits on an illegal-instruction exception[m
[32m+[m[32m     *  (sepc points to the faulting instruction in memory). If stval is[m
[32m+[m[32m     *   written with a nonzero value when an illegal-instruction exception[m
[32m+[m[32m     *   occurs, then stval will contain the shortest of:[m
[32m+[m[32m     *    - the actual faulting instruction[m
[32m+[m[32m     *    - the first ILEN bits of the faulting instruction[m
[32m+[m[32m     *    - the first SXLEN bits of the faulting instruction[m
[32m+[m[32m     *[m
[32m+[m[32m     * So a value of zero in stval signifies either that the feature is not[m
[32m+[m[32m     * supported, or an illegal zero instruction was fetched. To verify[m
[32m+[m[32m     * that it is needed to read an instruction directly from memory.[m
[32m+[m[32m     */[m
[32m+[m[32m    if ( insn == 0 )[m
[32m+[m[32m    {[m
[32m+[m[32m        struct trap_info utrap = { 0 };[m
[32m+[m
[32m+[m[32m        insn = riscv_vcpu_unpriv_read(v, true, vcpu_guest_cpu_user_regs(v)->sepc, &utrap);[m
[32m+[m[32m        if ( utrap.scause )[m
[32m+[m[32m        {[m
[32m+[m[32m            if ( is_load_guest_page_fault(utrap.scause) )[m
[32m+[m[32m                return 0;[m
[32m+[m
[32m+[m[32m            riscv_vcpu_trap_redirect(v, &utrap);[m
[32m+[m
[32m+[m[32m            return 0;[m
[32m+[m[32m        }[m
[32m+[m[32m    }[m
[32m+[m
[32m+[m[32m    /*[m
[32m+[m[32m     * The 16-bit Compressed ISA (RVC) primarily consists of unprivileged[m
[32m+[m[32m     * operations such as basic arithmetic, register moves, and simplified[m
[32m+[m[32m     * load/store or jump instructions (e.g., C.ADDI, C.LWSP, C.J). These are[m
[32m+[m[32m     * not relevant to the hypervisor and should be handled by the guest itself.[m
[32m+[m[32m     */[m
[32m+[m[32m    if ( GET_INSN_LENGTH(insn) == 2 )[m
[32m+[m[32m        return truly_illegal_insn(v, insn);[m
[32m+[m
[32m+[m[32m    switch ( (insn & INSN_OPCODE_MASK) >> INSN_OPCODE_SHIFT )[m
[32m+[m[32m    {[m
[32m+[m[32m    case INSN_OPCODE_SYSTEM:[m
[32m+[m[32m        return system_opcode_insn(v, insn);[m
[32m+[m
[32m+[m[32m    default:[m
[32m+[m[32m        return truly_illegal_insn(v, insn);[m
[32m+[m[32m    }[m
[32m+[m[32m}[m
[32m+[m
 void do_trap(struct cpu_user_regs *cpu_regs)[m
 {[m
     register_t pc = cpu_regs->sepc;[m
[36m@@ -497,6 +585,20 @@[m [mvoid do_trap(struct cpu_user_regs *cpu_regs)[m
         handle_guest_page_fault(cause, cpu_regs);[m
         break;[m
 [m
[32m+[m[32m    case CAUSE_VIRTUAL_INST_FAULT:[m
[32m+[m[32m    {[m
[32m+[m[32m        int ret;[m
[32m+[m
[32m+[m[32m        BUG_ON(!(cpu_regs->hstatus & HSTATUS_SPV));[m
[32m+[m
[32m+[m[32m        ret = handle_virt_instruction_fault(current);[m
[32m+[m[32m        if ( ret < 0 )[m
[32m+[m[32m            /* TODO: crash only domain instead of Xen? */[m
[32m+[m[32m            /* domain_crash(current->domain); */[m
[32m+[m[32m            panic("couldn't handle CAUSE_VIRTUAL_INST_FAULT: %d\n", ret);[m
[32m+[m[32m        break;[m
[32m+[m[32m    }[m
[32m+[m
     case CAUSE_ILLEGAL_INSTRUCTION:[m
         if ( do_bug_frame(cpu_regs, pc) >= 0 )[m
         {[m
