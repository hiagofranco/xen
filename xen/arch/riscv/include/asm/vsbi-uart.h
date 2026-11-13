/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ASM__RISCV__VSBI_UART_H
#define ASM__RISCV__VSBI_UART_H

#include <xen/spinlock.h>
#include <xen/stdbool.h>

#include <public/io/console.h>

struct domain;
struct page_info;

#define VSBI_UART_FIFO_SIZE 32
#define VSBI_UART_OUT_BUF_SIZE 128

struct vsbi_uart_xen_backend {
    char in[VSBI_UART_FIFO_SIZE];
    char out[VSBI_UART_OUT_BUF_SIZE];
    XENCONS_RING_IDX in_cons, in_prod;
    XENCONS_RING_IDX out_prod;
};

struct vsbi_uart {
    bool backend_in_domain;
    union {
        struct {
            void *ring_buf;
            struct page_info *ring_page;
        } dom;
        struct vsbi_uart_xen_backend *xen;
    } backend;

    spinlock_t lock;
};

/*
 * TODO: we don't support an option that backend is in a domain.
 *       At the moment, backend is ib Xen.
 *       This structure should be implemented in the case if backend
 *       is in a domain.
 */
struct vsbi_uart_init_info {
};

int domain_vsbi_uart_init(struct domain *d , struct vsbi_uart_init_info *info);
void domain_vsbi_uart_deinit(struct domain *d);
void vsbi_print_char(char c);

#endif /* ASM__RISCV__VSBI_UART_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
