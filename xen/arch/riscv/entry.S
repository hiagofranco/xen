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
        * After the swap, If xen trapped, tp will be empty.
        */
        csrrw   tp, CSR_SSCRATCH, tp

        /* Based on Linux kenrel arch/riscv/entry.S */
        bnez    tp, .L_guest_trapped

/* tp = NULL => XEN trapped */
.L_restore_kernel_tpsp:
        csrr    tp, CSR_SSCRATCH
        REG_S   sp, PCPU_INFO_TMP(tp)
        addi    sp, sp, -(CPUINFO_SIZE)

        j       .L_save_to_stack

/* tp != 0 => Guest trapped */
.L_guest_trapped:
        REG_S   sp, PCPU_INFO_TMP(tp)
        REG_L   sp, PCPU_INFO_GUEST_CPU_INFO(tp)

    /* Exceptions from xen */
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
 * struct vcpu *__context_switch(struct vcpu *prev, struct vcpu *next)
 *
 * This is called on prev's stack, and returns on next's.
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
