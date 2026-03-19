/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/init.h>
#include <xen/bsearch.h>
#include <xen/lib.h>
#include <xen/livepatch.h>
#include <xen/sort.h>
#include <xen/virtual_region.h>

#include <asm/csr.h>
#include <asm/extable.h>
#include <asm/processor.h>
#include <asm/traps.h>

#define EX_FIELD(ptr, field) ((unsigned long)&(ptr)->field + (ptr)->field)

static inline unsigned long ex_insn(const struct exception_table_entry *ex)
{
    return EX_FIELD(ex, insn);
}

static inline unsigned long ex_fixup(const struct exception_table_entry *ex)
{
    return EX_FIELD(ex, fixup);
}

static void __init cf_check swap_ex(void *a, void *b)
{
    struct exception_table_entry *x = a, *y = b, tmp;
    long delta = b - a;

    tmp = *x;
    x->insn = y->insn + delta;
    y->insn = tmp.insn - delta;

    x->fixup = y->fixup + delta;
    y->fixup = tmp.fixup - delta;

    x->type = y->type;
    y->type = tmp.type;

    x->data = y->data;
    y->data = tmp.data;
}

static int cf_check cmp_ex(const void *a, const void *b)
{
    const unsigned long insn_a = ex_insn(a);
    const unsigned long insn_b = ex_insn(b);

    return (insn_a > insn_b) - (insn_a < insn_b);
}

void init_or_livepatch sort_exception_table(struct exception_table_entry *start,
                                 const struct exception_table_entry *stop)
{
    sort(start, stop - start, sizeof(*start), cmp_ex, swap_ex);
}

void __init sort_exception_tables(void)
{
    sort_exception_table(__start___ex_table, __stop___ex_table);
}

static void ex_handler_fixup(const struct exception_table_entry *ex,
                             struct cpu_user_regs *regs)
{
    regs->sepc = ex_fixup(ex);
}

static inline unsigned long regs_get_gpr(struct cpu_user_regs *regs,
                                         unsigned int offset)
{
    /*
     * The GPR number -> offset arithmetic below relies on x0..x31 being
     * laid out at the start of struct cpu_user_regs in architectural
     * order.
     */
    BUILD_BUG_ON(offsetof(struct cpu_user_regs, ra) !=
                 sizeof(unsigned long));
    BUILD_BUG_ON(offsetof(struct cpu_user_regs, t6) !=
                 31 * sizeof(unsigned long));

    if ( unlikely(!offset || (offset > MAX_REG_OFFSET)) )
        return 0;

    return *(unsigned long *)((unsigned long)regs + offset);
}

static void ex_handler_trap_info(const struct exception_table_entry *ex,
                                 struct cpu_user_regs *regs)
{
    struct trap_info *trap_info =
        (struct trap_info *)regs_get_gpr(regs, ex->data * sizeof(unsigned long));

    BUG_ON(!trap_info);

    trap_info->sepc = csr_read(CSR_SEPC);
    trap_info->scause = csr_read(CSR_SCAUSE);
    trap_info->stval = csr_read(CSR_STVAL);

    regs->sepc = ex_fixup(ex);
}

bool fixup_exception(struct cpu_user_regs *regs)
{
    unsigned long pc = regs->sepc;
    const struct virtual_region *region = find_text_region(pc);
    const struct exception_table_entry *ex;
    struct exception_table_entry key;

    if ( !region || !region->ex )
        return false;

    key.insn = pc - (unsigned long)&key.insn;

    ex = bsearch(&key, region->ex, region->ex_end - region->ex, sizeof(key),
                 cmp_ex);

    if ( !ex )
        return false;

    switch ( ex->type )
    {
    case EX_TYPE_FIXUP:
        ex_handler_fixup(ex, regs);
        break;

    case EX_TYPE_TRAP_INFO:
        ex_handler_trap_info(ex, regs);
        break;

    default:
        printk(XENLOG_ERR
               "Unsupported exception table entry type %u for pc %#lx\n",
               ex->type, pc);

        return false;
    }

    return true;
}
