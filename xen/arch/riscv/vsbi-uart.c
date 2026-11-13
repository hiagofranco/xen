/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/console.h>
#include <xen/errno.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/xmalloc.h>

#include <asm/vsbi-uart.h>

int domain_vsbi_uart_init(struct domain *d, struct vsbi_uart_init_info *info)
{
    int rc;
    struct vsbi_uart *vsbi_uart = &d->arch.vsbi_uart;

    if ( vsbi_uart->backend.dom.ring_buf )
    {
        printk("%s: ring_buf != 0\n", __func__);
        return -EINVAL;
    }

    /*
     * info is NULL when the backend is in Xen.
     * info is != NULL when the backend is in a domain.
     */
    if ( info != NULL )
    {
        printk("%s: vsbi_uart backend in a domain isn't supported\n", __func__);
        rc = -EOPNOTSUPP;
        goto out;
    }
    else
    {
        vsbi_uart->backend_in_domain = false;

        vsbi_uart->backend.xen = xzalloc(struct vsbi_uart_xen_backend);
        if ( vsbi_uart->backend.xen == NULL )
        {
            rc = -ENOMEM;
            goto out;
        }
    }

    spin_lock_init(&vsbi_uart->lock);

    if ( d->console )
        d->console->input_allowed = true;

    return 0;

out:
    domain_vsbi_uart_deinit(d);

    return rc;
}

void domain_vsbi_uart_deinit(struct domain *d)
{
    struct vsbi_uart *vsbi_uart = &d->arch.vsbi_uart;

    if ( vsbi_uart->backend_in_domain )
        printk("%s: backed in a domain isn't supported\n", __func__);
    else
        XFREE(vsbi_uart->backend.xen);
}

void vsbi_print_char(char c)
{
    struct domain *currd = current->domain;
    struct domain_console *cons = currd->console;

    if ( !is_console_printable(c) )
        return;

    spin_lock(&cons->lock);
    ASSERT(cons->idx < ARRAY_SIZE(cons->buf));
    if ( c != '\n' )
        cons->buf[cons->idx++] = c;
    if ( (cons->idx == (ARRAY_SIZE(cons->buf) - 1)) || (c == '\n') )
    {
        cons->buf[cons->idx] = '\0';
        guest_printk(currd, XENLOG_G_DEBUG "%s\n", cons->buf);
        cons->idx = 0;
    }
    spin_unlock(&cons->lock);
}

int vsbi_uart_rx_char(struct domain *d, char c)
{
    struct vsbi_uart *u = &d->arch.vsbi_uart;
    struct vsbi_uart_xen_backend *b = u->backend.xen;
    unsigned long flags;
    XENCONS_RING_IDX prod, cons;

    if ( !b )
        return -ENODEV;

    spin_lock_irqsave(&u->lock, flags);
    prod = b->in_prod;
    cons = b->in_cons;

    if ( (prod - cons) >= sizeof(b->in) )
    {
        spin_unlock_irqrestore(&u->lock, flags);
        return -ENOSPC;
    }

    b->in[MASK_XENCONS_IDX(prod, b->in)] = c;
    b->in_prod = prod + 1;
    spin_unlock_irqrestore(&u->lock, flags);

    return 0;
}

int vsbi_uart_get_char(struct domain *d)
{
    struct vsbi_uart *u = &d->arch.vsbi_uart;
    struct vsbi_uart_xen_backend *b = u->backend.xen;
    unsigned long flags;
    XENCONS_RING_IDX prod, cons;
    int c = -1;

    if ( !b )
        return -1;

    spin_lock_irqsave(&u->lock, flags);
    cons = b->in_cons;
    prod = b->in_prod;

    if ( cons != prod )
    {
        c = (unsigned char)b->in[MASK_XENCONS_IDX(cons, b->in)];
        b->in_cons = cons + 1;
    }
    spin_unlock_irqrestore(&u->lock, flags);

    return c;
}
