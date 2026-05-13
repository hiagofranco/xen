/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RISCV_IPI_H
#define RISCV_IPI_H

#include <xen/cpumask.h>

enum ipis {
    IPI_EVENT_CHECK,
    IPI_CALL_FUNCTION,
    IPI_MAX
};

typedef int (*ipi_send_t)(const cpumask_t *mask);

void ipi_handler(void);
void ipi_send_mask(enum ipis op, const cpumask_t *mask);
void ipi_init(ipi_send_t send_func);

#endif /* RISCV_IPI_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
