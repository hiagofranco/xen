/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/bitops.h>
#include <xen/cpu.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/sched.h>
#include <xen/sections.h>
#include <xen/spinlock.h>
#include <xen/types.h>

#include <asm/aia.h>
#include <asm/cpufeature.h>
#include <asm/csr.h>
#include <asm/current.h>

struct vgein_ctrl {
    /* The least-significant bits are implemented first, apart from bit 0 */
    unsigned long bmp;
    spinlock_t lock;
    unsigned int geilen;
};

/*
 * VGEIN control structure for each physical CPU to track which VS (guest)
 * interrupt file IDs are in use.
 */
static DEFINE_PER_CPU(struct vgein_ctrl, vgein);

static bool __ro_after_init _aia_usable;

bool aia_usable(void)
{
    return _aia_usable;
}

/* HGEIE is a per-hart CSR, so this has to run on the CPU being initialized. */
static int vgein_init(void)
{
    struct vgein_ctrl *vgein = &this_cpu(vgein);

    spin_lock_init(&vgein->lock);

    csr_write(CSR_HGEIE, ~0UL);
    vgein->geilen = flsl(csr_read(CSR_HGEIE) >> 1);
    csr_write(CSR_HGEIE, 0);

    vgein->bmp = 0;

    if ( !vgein->geilen )
        return -EOPNOTSUPP;

    return 0;
}

static void vgein_deinit(void)
{
    csr_write(CSR_HGEIE, 0);
}

static int cf_check cpu_callback(struct notifier_block *nfb,
                                 unsigned long action, void *hcpu)
{
    unsigned int cpu = (unsigned long)hcpu;
    int rc = 0;

    switch ( action )
    {
    case CPU_STARTING:
        rc = vgein_init();
        if ( rc )
            printk(XENLOG_ERR "AIA: failed to init vgein for CPU%u: %d\n",
                   cpu, rc);
        break;

    case CPU_DYING:
        vgein_deinit();
        break;
    }

    return notifier_from_errno(rc);
}

static struct notifier_block cpu_nfb = {
    .notifier_call = cpu_callback,
};

void __init aia_init(void)
{
    int rc;

    if ( !riscv_isa_extension_available(NULL, RISCV_ISA_EXT_ssaia) )
    {
        dprintk(XENLOG_WARNING, "SSAIA isn't present in riscv,isa\n");
        return;
    }

    if ( (rc = vgein_init()) )
    {
        dprintk(XENLOG_ERR, "vgein_init() failed: %d\n", rc);
        return;
    }

    _aia_usable = true;

    register_cpu_notifier(&cpu_nfb);
}

unsigned int vgein_assign(struct vcpu *v)
{
    unsigned int vgein_id;
    struct vgein_ctrl *vgein = &per_cpu(vgein, v->processor);
    unsigned long *bmp = &vgein->bmp;
    unsigned long flags;

    if ( !vgein->geilen )
        return 0;

    spin_lock_irqsave(&vgein->lock, flags);
    /*
     * The vgein_id shouldn't be zero, as it will indicate that no guest
     * external interrupt source is selected for VS-level external interrupts
     * according to RISC-V privileged spec:
     *   Hypervisor Status Register (hstatus) in RISC-V privileged spec:
     *
     *   The VGEIN (Virtual Guest External Interrupt Number) field selects
     *   a guest external interrupt source for VS-level external interrupts.
     *   VGEIN is a WLRL field that must be able to hold values between zero
     *   and the maximum guest external interrupt number (known as GEILEN),
     *   inclusive.
     *   When VGEIN=0, no guest external interrupt source is selected for
     *   VS-level external interrupts.
     *
     * So start to search from bit number 1.
     */
    vgein_id = find_next_zero_bit(bmp, vgein->geilen + 1, 1);

    if ( vgein_id > vgein->geilen )
        vgein_id = 0;
    else
        __set_bit(vgein_id, bmp);

    spin_unlock_irqrestore(&vgein->lock, flags);

#ifdef VGEIN_DEBUG
    gprintk(XENLOG_DEBUG, "%s: %pv: vgein_id(%u), xen_cpu%u_bmp=%#lx\n",
            __func__, v, vgein_id, v->processor, *bmp);
#endif

    return vgein_id;
}

void vgein_release(struct vcpu *v, unsigned int vgein_id, unsigned int cpu)
{
    unsigned long flags;
    struct vgein_ctrl *vgein = &per_cpu(vgein, cpu);

    if ( !vgein_id )
        return;

    spin_lock_irqsave(&vgein->lock, flags);
    if ( !__test_and_clear_bit(vgein_id, &vgein->bmp) )
        ASSERT_UNREACHABLE();
    spin_unlock_irqrestore(&vgein->lock, flags);

#ifdef VGEIN_DEBUG
    gprintk(XENLOG_DEBUG, "%s: %pv: vgein_id(%u), xen_cpu%u_bmp=%#lx\n",
            __func__, v, vgein_id, cpu, vgein->bmp);
#endif
}
