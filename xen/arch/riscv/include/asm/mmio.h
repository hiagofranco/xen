/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RISCV_MMIO_H
#define RISCV_MMIO_H

#include <xen/lib.h>

struct domain;
struct vcpu;

typedef struct {
    paddr_t gpa;
    unsigned int len;  /* Access width in bytes */
    bool is_write;
    /* Store: value to write; load: value read (set by handler) */
    register_t data;
} mmio_info_t;

enum io_state {
    IO_ABORT,       /* The IO was handled and led to an abort. */
    IO_HANDLED,     /* The IO was successfully handled. */
    IO_UNHANDLED,   /* No handler found for the IO. */
};

typedef enum io_state mmio_read_t(struct vcpu *v, mmio_info_t *info);
typedef enum io_state mmio_write_t(struct vcpu *v, const mmio_info_t *info);

struct mmio_handler_ops {
    mmio_read_t *read;
    mmio_write_t *write;
};

int do_mmio(mmio_info_t *info, paddr_t fault_addr, unsigned int len);
int register_mmio_handler(struct domain *d,
                          const struct mmio_handler_ops *ops,
                          paddr_t addr, paddr_t size);
int domain_io_init(struct domain *d);
void domain_io_free(struct domain *d);

#endif /* RISCV_MMIO_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
