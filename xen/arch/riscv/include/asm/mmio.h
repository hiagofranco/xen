/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RISCV_MMIO_H
#define RISCV_MMIO_H

#include <xen/lib.h>
#include <xen/rwlock.h>

#define MAX_IO_HANDLER  16

typedef struct {
    paddr_t gpa;
    unsigned int len;  /* access width in bytes (1, 2, 4, 8) */
    bool is_write;
    register_t data;   /* store: value to write; load: value read (set by handler) */
} mmio_info_t;

enum io_state
{
    IO_ABORT,       /* The IO was handled and led to an abort. */
    IO_HANDLED,     /* The IO was successfully handled. */
    IO_UNHANDLED,   /* No handler found for the IO. */
};

typedef enum io_state (*mmio_read_t)(struct vcpu *v, mmio_info_t *info,
                                     register_t *r);
typedef enum io_state (*mmio_write_t)(struct vcpu *v, mmio_info_t *info,
                                      register_t r);

struct mmio_handler_ops {
    mmio_read_t read;
    mmio_write_t write;
};

struct mmio_handler {
    paddr_t addr;
    paddr_t size;
    const struct mmio_handler_ops *ops;
};

struct vmmio {
    unsigned int num_entries;
    unsigned int max_num_entries;
    rwlock_t lock;
    struct mmio_handler *handlers;
};

enum io_state try_handle_mmio(mmio_info_t *info);
void register_mmio_handler(struct domain *d,
                           const struct mmio_handler_ops *ops,
                           paddr_t addr, paddr_t size);
int domain_io_init(struct domain *d, unsigned int max_count);
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
