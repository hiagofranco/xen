/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/bitmap.h>
#include <xen/cpu.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/sections.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <xen/types.h>
#include <xen/xvmalloc.h>

#include <asm/aia.h>
#include <asm/cpufeature.h>
#include <asm/csr.h>
#include <asm/current.h>
#include <asm/imsic.h>

struct vgein_ctrl {
    unsigned long bmp;
    spinlock_t lock;
    struct vcpu **owners;
    /* The least-significant bits are implemented first, apart from bit 0 */
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

void vcpu_aia_init(struct vcpu *v)
{
    unsigned int new_vsfile_id;
    int rc;

    if ( !aia_usable() )
        return;

    new_vsfile_id = vgein_assign(v);

    /*
     * vgein_assign() returns 0 when no free h/w guest interrupt file is
     * available (including GEILEN == 0); imsic_map_guest_file() maps nothing
     * in that case.
     */
    rc = imsic_map_guest_file(v, new_vsfile_id);
    if ( rc )
    {
        /* Can't continue w/o correctly mapped IMSIC interrupt file */
        domain_crash(v->domain);
        return;
    }

    vcpu_guest_cpu_user_regs(v)->hstatus |=
        MASK_INSR(new_vsfile_id, HSTATUS_VGEIN);

    imsic_update_state(v, new_vsfile_id);
}

static int vgein_init(unsigned int cpu)
{
    struct vgein_ctrl *vgein = &per_cpu(vgein, cpu);

    csr_write(CSR_HGEIE, -1UL);
    vgein->geilen = flsl(csr_read(CSR_HGEIE) >> 1);
    csr_write(CSR_HGEIE, 0);

    printk("cpu%u.geilen=%u\n", cpu, vgein->geilen);

    if ( !vgein->geilen )
        return -EOPNOTSUPP;

    /*
     * Guest interrupt file IDs run from 1 to geilen inclusive (0 means no
     * guest external interrupt source is selected), so one extra element is
     * needed to be able to index owners[] by the ID directly.
     */
    vgein->owners = xvzalloc_array(struct vcpu *, vgein->geilen + 1);
    if ( !vgein->owners )
        return -ENOMEM;

    spin_lock_init(&vgein->lock);

    return 0;
}

static void vgein_deinit(unsigned int cpu)
{
    struct vgein_ctrl *vgein = &per_cpu(vgein, cpu);

    XVFREE(vgein->owners);
    vgein->bmp = 0;
    vgein->geilen = 0;
}

static int cf_check cpu_callback(struct notifier_block *nfb, unsigned long action,
                        void *hcpu)
{
    unsigned int cpu = (unsigned long)hcpu;
    int rc = 0;

    switch ( action )
    {
    case CPU_STARTING:
        rc = vgein_init(cpu);
        if ( rc )
            printk("AIA: failed to init vgein for CPU%u\n", cpu);
        break;

    case CPU_UP_CANCELED:
    case CPU_DEAD:
        vgein_deinit(cpu);
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

    if ( (rc = vgein_init(0)) )
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
    {
        __set_bit(vgein_id, bmp);
        vgein->owners[vgein_id] = v;
    }

    spin_unlock_irqrestore(&vgein->lock, flags);

#ifdef VGEIN_DEBUG
    gprintk(XENLOG_DEBUG, "%s: %pv: vgein_id(%u), xen_cpu%u_bmp=%#lx\n",
           __func__, v, vgein_id, v->processor, *bmp);
#endif

    return vgein_id;
}

void vgein_release(struct vcpu *v, unsigned int vgein_id)
{
    unsigned long flags;
    struct vgein_ctrl *vgein = &per_cpu(vgein, v->processor);

    if ( !vgein_id )
        return;

    spin_lock_irqsave(&vgein->lock, flags);
    __clear_bit(vgein_id, &vgein->bmp);
    vgein->owners[vgein_id] = NULL;
    spin_unlock_irqrestore(&vgein->lock, flags);

#ifdef VGEIN_DEBUG
    gprintk(XENLOG_DEBUG, "%s: vgein_id(%u), xen_cpu%u_bmp=%#lx\n",
           __func__, vgein_id, v->processor, vgein->bmp);
#endif
}

void hgei_interrupt(void)
{
    unsigned long hgei_mask, flags;
    struct vgein_ctrl *vgein = &this_cpu(vgein);

    hgei_mask = csr_read(CSR_HGEIP) & csr_read(CSR_HGEIE);
    csr_clear(CSR_HGEIE, hgei_mask);

    spin_lock_irqsave(&vgein->lock, flags);

    for_each_set_bit ( guest_file_id, hgei_mask )
    {
        /*
         * guest_file_id shouldn't be zero, as it will indicate that no
         * guest external interrupt source is selected for VS-level external
         * interrupts.
         */
        ASSERT(guest_file_id);

        if ( vgein->owners[guest_file_id] )
        {
#ifdef VGEIN_DEBUG
            gprintk(XENLOG_DEBUG, "%s: kick ->%pv, hgei_mask(%#lx)\n",
                    __func__, vgein->owners[guest_file_id], hgei_mask);
#endif

            vcpu_kick(vgein->owners[guest_file_id]);
        }
    }

    spin_unlock_irqrestore(&vgein->lock, flags);
}
