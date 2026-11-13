/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/init.h>
#include <xen/sections.h>

#include <asm/cpufeature.h>
#include <asm/fpu.h>

static bool __ro_after_init has_fpu;

void __init fpu_init(void)
{
    /*
     * The save/restore helpers use fld/fsd, which require D. D implies F,
     * so the same condition also controls advertising F/D to guests.
     */
    has_fpu = IS_ENABLED(CONFIG_RISCV_64) &&
              riscv_isa_extension_available(NULL, RISCV_ISA_EXT_d);
}

bool cpu_has_fpu(void)
{
    return has_fpu;
}
