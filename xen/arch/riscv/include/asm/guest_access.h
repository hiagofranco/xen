/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RISCV_GUEST_ACCESS_H
#define RISCV_GUEST_ACCESS_H

#include <xen/types.h>

struct domain;
struct trap_info;

unsigned long raw_copy_to_guest(void *to, const void *from, unsigned len);
unsigned long raw_copy_from_guest(void *to, const void *from, unsigned len);
unsigned long raw_clear_guest(void *to, unsigned int len);

#define __raw_copy_to_guest raw_copy_to_guest
#define __raw_copy_from_guest raw_copy_from_guest
#define __raw_clear_guest raw_clear_guest

/*
 * Pre-validate a guest handle.
 * Allows use of faster __copy_* functions.
 */
/* All RISCV guests are paging mode external and hence safe */
#define guest_handle_okay(hnd, nr) (1)
#define guest_handle_subrange_okay(hnd, first, last) (1)

unsigned long copy_to_guest_phys(struct domain *d, paddr_t gpa, void *buf,
                                 unsigned long len);

/*
 * Read machine word from guest memory
 *
 * @guest_addr: Guest address to read
 * @read_insn: Flag representing whether we are reading instruction
 * @trap: Output pointer to trap details if something went wrong during read.
 *        It must be zero-initialised by the caller: it is written only when
 *        an access faults, so trap->scause == 0 on return is what tells the
 *        caller that the read succeeded.
 *
 * The hlv/hlvx instructions translate guest_addr through the live
 * vsatp/hgatp CSRs, so the read is only meaningful for the address
 * space of the currently running vCPU. It must therefore only be called
 * while handling a trap taken from that vCPU, never from a nested trap
 * taken in HS-mode, which may have interrupted Xen at a point where
 * hstatus, vsatp or hgatp don't belong to the vCPU.
 *
 * At most two halfwords are fetched when @read_insn is true, i.e. encodings
 * wider than 32 bits are not supported. Such an encoding cannot be completed
 * by calling this function again at @guest_addr + 4: the length check is
 * applied to the first halfword read, which would then be a continuation of
 * the instruction rather than its opcode. It is up to the caller to reject
 * anything that is neither a 16- nor a 32-bit encoding.
 */
unsigned long read_guest(unsigned long guest_addr, bool read_insn,
                         struct trap_info *trap);

#endif /* RISCV_GUEST_ACCESS_H */
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
