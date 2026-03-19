/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RISCV_REGS_H
#define RISCV_REGS_H

#ifndef __ASSEMBLER__

#include <xen/bug.h>
#include <xen/nospec.h>

#include <asm/gpr-num.h>
#include <asm/processor.h>

#define hyp_mode(r)     (0)

static inline bool guest_mode(const struct cpu_user_regs *r)
{
    BUG_ON("unimplemented");
}

/*
 * Resolve an architectural register number to where its value is saved in
 * @regs. Relies on x0..x31 being laid out as an array anchored at ->zero; see
 * the comment above struct cpu_user_regs.
 */
static inline unsigned long *regs_gpr_ptr(struct cpu_user_regs *regs,
                                          unsigned int num)
{
    ASSERT(num < NR_GPRS);

    /*
     * Not a speculation concern, but a convenient way to keep the access in
     * bounds in release builds, where the assertion above is compiled out.
     */
    return &(&regs->zero)[array_index_nospec(num, NR_GPRS)];
}

#endif /* __ASSEMBLER__ */

#endif /* RISCV_REGS_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
