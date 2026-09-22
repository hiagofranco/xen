#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/processor.h>
#include <asm/riscv_encoding.h>
#include <asm/traps.h>

FUNC(handle_trap)
        /*
         * swap sscratch and tp
         *
         * After the swap, if a guest trapped, tp will have a pointer to
         * pcpu_info of the guest.
         * After the swap, if Xen trapped, tp will be zero.
         */
        csrrw   tp, CSR_SSCRATCH, tp

        bnez    tp, .L_guest_trapped

/* tp == 0 => Xen trapped */
.L_xen_trapped:
        csrr    tp, CSR_SSCRATCH
        REG_S   sp, PCPU_INFO_TMP(tp)
        addi    sp, sp, -(CPU_USER_REGS_SIZE)

        j       .L_save_to_stack

/* tp != 0 => Guest trapped */
.L_guest_trapped:
        REG_S   sp, PCPU_INFO_TMP(tp)
        REG_L   sp, PCPU_INFO_CURR_CPU_INFO(tp)

.L_save_to_stack:
        /* Save registers */
        REG_S   ra, CPU_USER_REGS_RA(sp)
        REG_S   gp, CPU_USER_REGS_GP(sp)
        REG_S   t0, CPU_USER_REGS_T0(sp)
        REG_S   t1, CPU_USER_REGS_T1(sp)
        REG_S   t2, CPU_USER_REGS_T2(sp)
        REG_S   s0, CPU_USER_REGS_S0(sp)
        REG_S   s1, CPU_USER_REGS_S1(sp)
        REG_S   a0, CPU_USER_REGS_A0(sp)
        REG_S   a1, CPU_USER_REGS_A1(sp)
        REG_S   a2, CPU_USER_REGS_A2(sp)
        REG_S   a3, CPU_USER_REGS_A3(sp)
        REG_S   a4, CPU_USER_REGS_A4(sp)
        REG_S   a5, CPU_USER_REGS_A5(sp)
        REG_S   a6, CPU_USER_REGS_A6(sp)
        REG_S   a7, CPU_USER_REGS_A7(sp)
        REG_S   s2, CPU_USER_REGS_S2(sp)
        REG_S   s3, CPU_USER_REGS_S3(sp)
        REG_S   s4, CPU_USER_REGS_S4(sp)
        REG_S   s5, CPU_USER_REGS_S5(sp)
        REG_S   s6, CPU_USER_REGS_S6(sp)
        REG_S   s7, CPU_USER_REGS_S7(sp)
        REG_S   s8, CPU_USER_REGS_S8(sp)
        REG_S   s9, CPU_USER_REGS_S9(sp)
        REG_S   s10,CPU_USER_REGS_S10(sp)
        REG_S   s11,CPU_USER_REGS_S11(sp)
        REG_S   t3, CPU_USER_REGS_T3(sp)
        REG_S   t4, CPU_USER_REGS_T4(sp)
        REG_S   t5, CPU_USER_REGS_T5(sp)
        REG_S   t6, CPU_USER_REGS_T6(sp)

        csrr    t0, CSR_SEPC
        REG_S   t0, CPU_USER_REGS_SEPC(sp)

        csrr    t0, CSR_SSTATUS
        REG_S   t0, CPU_USER_REGS_SSTATUS(sp)

        csrr    t0, CSR_HSTATUS
        REG_S   t0, CPU_USER_REGS_HSTATUS(sp)

        /* Save original SP */
        REG_L   t0, PCPU_INFO_TMP(tp)
        REG_S   t0, CPU_USER_REGS_SP(sp)

        csrr    t0, CSR_SSCRATCH
        REG_S   t0, CPU_USER_REGS_TP(sp)

        /*
         * Set the scratch register to 0, so that if a recursive exception
         * occurs, the exception vector knows it came from the hypervisor.
         */
        csrw    CSR_SSCRATCH, x0

        mv      a0, sp
        call    do_trap

        REG_L   t0, CPU_USER_REGS_HSTATUS(sp)
        andi    t0, t0, HSTATUS_SPV

        beqz    t0, .L_restore_registers

        /*
         * Save TP into the scratch register, as it was swapped at the start
         * of handle_trap(), so we can locate the hypervisor data structures
         * again.
         * This is only necessary when a trap occurs from a guest, as in that
         * case the SSCRATCH register is expected to contain a pointer to
         * struct pcpu_info.
         */
        csrw    CSR_SSCRATCH, tp

.L_restore_registers:
        /* Restore stack_cpu_regs */
        REG_L	t0, CPU_USER_REGS_HSTATUS(sp)
        csrw	CSR_HSTATUS, t0

        REG_L   t0, CPU_USER_REGS_SEPC(sp)
        csrw    CSR_SEPC, t0

        REG_L   t0, CPU_USER_REGS_SSTATUS(sp)
        csrw    CSR_SSTATUS, t0

        REG_L   ra, CPU_USER_REGS_RA(sp)
        REG_L   gp, CPU_USER_REGS_GP(sp)
        REG_L   t0, CPU_USER_REGS_T0(sp)
        REG_L   t1, CPU_USER_REGS_T1(sp)
        REG_L   t2, CPU_USER_REGS_T2(sp)
        REG_L   s0, CPU_USER_REGS_S0(sp)
        REG_L   s1, CPU_USER_REGS_S1(sp)
        REG_L   a0, CPU_USER_REGS_A0(sp)
        REG_L   a1, CPU_USER_REGS_A1(sp)
        REG_L   a2, CPU_USER_REGS_A2(sp)
        REG_L   a3, CPU_USER_REGS_A3(sp)
        REG_L   a4, CPU_USER_REGS_A4(sp)
        REG_L   a5, CPU_USER_REGS_A5(sp)
        REG_L   a6, CPU_USER_REGS_A6(sp)
        REG_L   a7, CPU_USER_REGS_A7(sp)
        REG_L   s2, CPU_USER_REGS_S2(sp)
        REG_L   s3, CPU_USER_REGS_S3(sp)
        REG_L   s4, CPU_USER_REGS_S4(sp)
        REG_L   s5, CPU_USER_REGS_S5(sp)
        REG_L   s6, CPU_USER_REGS_S6(sp)
        REG_L   s7, CPU_USER_REGS_S7(sp)
        REG_L   s8, CPU_USER_REGS_S8(sp)
        REG_L   s9, CPU_USER_REGS_S9(sp)
        REG_L   s10, CPU_USER_REGS_S10(sp)
        REG_L   s11, CPU_USER_REGS_S11(sp)
        REG_L   t3, CPU_USER_REGS_T3(sp)
        REG_L   t4, CPU_USER_REGS_T4(sp)
        REG_L   t5, CPU_USER_REGS_T5(sp)
        REG_L   t6, CPU_USER_REGS_T6(sp)
        REG_L   tp, CPU_USER_REGS_TP(sp)
        REG_L   sp, CPU_USER_REGS_SP(sp)

        sret
END(handle_trap)

/*
 * struct vcpu *__context_switch(struct vcpu *prev, const struct vcpu *next)
 *
 * This is called on prev's stack, and returns on next's. As ra is
 * switched too, it doesn't return to its caller: it returns to where
 * next last called it from, i.e. into next's own context_switch(), or,
 * for a vCPU which has never run, to continue_new_vcpu() with an empty
 * stack.
 *
 * a0 - prev
 * a1 - next
 *
 * Returns prev in a0
 */
FUNC(__context_switch)
        REG_S   s0, VCPU_XEN_SAVED_CONTEXT_S0(a0)
        REG_S   s1, VCPU_XEN_SAVED_CONTEXT_S1(a0)
        REG_S   s2, VCPU_XEN_SAVED_CONTEXT_S2(a0)
        REG_S   s3, VCPU_XEN_SAVED_CONTEXT_S3(a0)
        REG_S   s4, VCPU_XEN_SAVED_CONTEXT_S4(a0)
        REG_S   s5, VCPU_XEN_SAVED_CONTEXT_S5(a0)
        REG_S   s6, VCPU_XEN_SAVED_CONTEXT_S6(a0)
        REG_S   s7, VCPU_XEN_SAVED_CONTEXT_S7(a0)
        REG_S   s8, VCPU_XEN_SAVED_CONTEXT_S8(a0)
        REG_S   s9, VCPU_XEN_SAVED_CONTEXT_S9(a0)
        REG_S   s10, VCPU_XEN_SAVED_CONTEXT_S10(a0)
        REG_S   s11, VCPU_XEN_SAVED_CONTEXT_S11(a0)
        REG_S   sp, VCPU_XEN_SAVED_CONTEXT_SP(a0)
        REG_S   ra, VCPU_XEN_SAVED_CONTEXT_RA(a0)

        REG_L   s0, VCPU_XEN_SAVED_CONTEXT_S0(a1)
        REG_L   s1, VCPU_XEN_SAVED_CONTEXT_S1(a1)
        REG_L   s2, VCPU_XEN_SAVED_CONTEXT_S2(a1)
        REG_L   s3, VCPU_XEN_SAVED_CONTEXT_S3(a1)
        REG_L   s4, VCPU_XEN_SAVED_CONTEXT_S4(a1)
        REG_L   s5, VCPU_XEN_SAVED_CONTEXT_S5(a1)
        REG_L   s6, VCPU_XEN_SAVED_CONTEXT_S6(a1)
        REG_L   s7, VCPU_XEN_SAVED_CONTEXT_S7(a1)
        REG_L   s8, VCPU_XEN_SAVED_CONTEXT_S8(a1)
        REG_L   s9, VCPU_XEN_SAVED_CONTEXT_S9(a1)
        REG_L   s10, VCPU_XEN_SAVED_CONTEXT_S10(a1)
        REG_L   s11, VCPU_XEN_SAVED_CONTEXT_S11(a1)
        REG_L   sp, VCPU_XEN_SAVED_CONTEXT_SP(a1)
        REG_L   ra, VCPU_XEN_SAVED_CONTEXT_RA(a1)

        ret
END(__context_switch)

/*
 * Enter a vCPU for the first time. Must be called with interrupts disabled,
 * see continue_new_vcpu().
 */
FUNC(return_to_new_vcpu)
        /*
         * SSCRATCH holds this hart's struct pcpu_info while a guest runs and
         * is zero while Xen runs, so that the trap path can tell the two
         * apart: tp points to pcpu_info in Xen context, but belongs to the
         * guest once sret has been executed. Establish that here; the trap
         * path has to swap the two back on a trap taken from the guest.
         */
        csrrw   tp, CSR_SSCRATCH, tp

        REG_L   t0, CPU_USER_REGS_HSTATUS(sp)
        csrw    CSR_HSTATUS, t0

        REG_L   t0, CPU_USER_REGS_SEPC(sp)
        csrw    CSR_SEPC, t0

        /* .a0 holds the hart id */
        REG_L   a0, CPU_USER_REGS_A0(sp)

        /*
         * .a1 holds the DTB address for the boot vCPU, or the opaque value
         * passed to SBI HSM hart_start() for secondary vCPUs
         */
        REG_L   a1, CPU_USER_REGS_A1(sp)

        /* Return to (V)S-mode, with interrupts re-enabled by sret */
        li      t0, SSTATUS_SPP | SSTATUS_SPIE
        csrs    CSR_SSTATUS, t0

        /*
         * sret doesn't switch stacks and leaves the GPRs alone, so every
         * register which isn't meaningful to the vCPU being started has to be
         * cleared here: otherwise the guest would see Xen's values, sp (this
         * vCPU's Xen stack) and ra among them.
         */
        .irp reg, ra, sp, gp, tp, t0, t1, t2, s0, s1, a2, a3, a4, a5, a6, a7, \
                  s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, t3, t4, t5, t6
        mv      \reg, zero
        .endr

        /* Enter guest */
        sret
END(return_to_new_vcpu)
