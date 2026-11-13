/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/asm.h>
#include <asm/asm-offsets.h>

/*
 * The rest of Xen is built for rv64imac. Enable F/D just for this file so the
 * FPU save/restore instructions can be assembled.
 */
.option arch, +f, +d

/* void fpu_save(struct vcpu *v) */
FUNC(fpu_save)
        frcsr   t0
        sd      t0, VCPU_FPU_FCSR(a0)
        .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        fsd     f\n, VCPU_FPU_F + 8 * \n(a0)
        .endr
        ret
END(fpu_save)

/* void fpu_restore(struct vcpu *v) */
FUNC(fpu_restore)
        ld      t0, VCPU_FPU_FCSR(a0)
        fscsr   t0
        .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        fld     f\n, VCPU_FPU_F + 8 * \n(a0)
        .endr
        ret
END(fpu_restore)
