/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Vates
 */

#include <xen/bsearch.h>
#include <xen/lib.h>
#include <xen/rwlock.h>
#include <xen/sched.h>
#include <xen/sort.h>
#include <xen/xvmalloc.h>

#include <asm/current.h>
#include <asm/mmio.h>

static enum io_state handle_read(const struct mmio_handler *handler,
                                 struct vcpu *v,
                                 mmio_info_t *info)
{
    register_t r = 0;
    enum io_state rc;

    rc = handler->ops->read(v, info, &r);
    if ( rc == IO_HANDLED )
        info->data = r;

    return rc;
}

static enum io_state handle_write(const struct mmio_handler *handler,
                                  struct vcpu *v,
                                  mmio_info_t *info)
{
    return handler->ops->write(v, info, info->data);
}

/* Assumes mmio regions are not overlapping. */
static int cmp_mmio_handler(const void *key, const void *elem)
{
    const struct mmio_handler *handler0 = key;
    const struct mmio_handler *handler1 = elem;

    if ( handler0->addr < handler1->addr )
        return -1;

    if ( handler0->addr >= (handler1->addr + handler1->size) )
        return 1;

    return 0;
}

static void swap_mmio_handler(void *a, void *b)
{
    struct mmio_handler *t1 = a, *t2 = b;

    SWAP(*t1, *t2);
}

/*
 * Return a copy of the matching handler rather than a pointer into
 * vmmio->handlers: a concurrent register_mmio_handler() re-sorts the
 * array, so an escaped pointer could refer to a different (or torn)
 * entry once the lock is dropped.  The copy stays valid as the ops
 * structures are never freed.
 */
static bool find_mmio_handler(struct domain *d, paddr_t gpa,
                              struct mmio_handler *out)
{
    struct vmmio *vmmio = &d->arch.vmmio;
    struct mmio_handler key = { .addr = gpa };
    const struct mmio_handler *handler;

    read_lock(&vmmio->lock);
    handler = bsearch(&key, vmmio->handlers, vmmio->num_entries,
                      sizeof(*handler), cmp_mmio_handler);
    if ( handler )
        *out = *handler;
    read_unlock(&vmmio->lock);

    return handler != NULL;
}

enum io_state try_handle_mmio(mmio_info_t *info)
{
    struct vcpu *v = current;
    struct mmio_handler handler = {};

    if ( !find_mmio_handler(v->domain, info->gpa, &handler) )
        return IO_UNHANDLED;

    if ( info->is_write )
        return handle_write(&handler, v, info);
    else
        return handle_read(&handler, v, info);
}

void register_mmio_handler(struct domain *d,
                           const struct mmio_handler_ops *ops,
                           paddr_t addr, paddr_t size)
{
    struct vmmio *vmmio = &d->arch.vmmio;
    struct mmio_handler *handler;

    write_lock(&vmmio->lock);

    BUG_ON(vmmio->num_entries >= vmmio->max_num_entries);

    handler = &vmmio->handlers[vmmio->num_entries];
    handler->ops = ops;
    handler->addr = addr;
    handler->size = size;
    vmmio->num_entries++;

    /* Sort mmio handlers in ascending order based on base address */
    sort(vmmio->handlers, vmmio->num_entries, sizeof(struct mmio_handler),
         cmp_mmio_handler, swap_mmio_handler);

    write_unlock(&vmmio->lock);
}

int domain_io_init(struct domain *d, unsigned int max_count)
{
    rwlock_init(&d->arch.vmmio.lock);
    d->arch.vmmio.num_entries = 0;
    d->arch.vmmio.max_num_entries = max_count;
    d->arch.vmmio.handlers = xvzalloc_array(struct mmio_handler, max_count);
    if ( !d->arch.vmmio.handlers )
        return -ENOMEM;

    return 0;
}

void domain_io_free(struct domain *d)
{
    XVFREE(d->arch.vmmio.handlers);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
