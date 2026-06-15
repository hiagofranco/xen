/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <xen/bsearch.h>
#include <xen/lib.h>
#include <xen/rwlock.h>
#include <xen/sched.h>
#include <xen/string.h>
#include <xen/xvmalloc.h>

#include <asm/current.h>
#include <asm/mmio.h>

#define MAX_IO_HANDLER  16

struct mmio_handler {
    paddr_t addr;
    paddr_t size;
    const struct mmio_handler_ops *ops;
};

struct vmmio {
    unsigned int num_entries;
    rwlock_t lock;
    struct mmio_handler handlers[MAX_IO_HANDLER];
};

/*
 * bsearch() comparator: @key holds the address to look up in its addr field,
 * @elem is an entry of vmmio->handlers. Relies on the regions not
 * overlapping, which register_mmio_handler() enforces.
 */
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

/*
 * Return a copy of the matching handler rather than a pointer into
 * vmmio->handlers: a concurrent register_mmio_handler() shifts entries
 * up to keep the array sorted, so an escaped pointer could refer to a
 * different (or torn) entry once the lock is dropped. The copy stays
 * valid as the ops structures are never freed.
 */
static bool find_mmio_handler(const struct domain *currd, paddr_t gpa,
                              struct mmio_handler *out)
{
    struct vmmio *vmmio = currd->arch.vmmio;
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

static enum io_state try_handle_mmio(mmio_info_t *info)
{
    struct vcpu *curr = current;
    struct mmio_handler handler = {};

    if ( !find_mmio_handler(curr->domain, info->gpa, &handler) )
        return IO_UNHANDLED;

    if ( info->is_write )
        return handler.ops->write(curr, info);

    return handler.ops->read(curr, info);
}

/*
 * Check alignment and dispatch a decoded MMIO access to a registered
 * handler. On success (0), info->data holds the read value for loads.
 *
 * There is no "retry" outcome to handle: find_mmio_handler() returns a
 * copy of the matching handler taken under vmmio->lock and the ops
 * structures are never freed, so the lookup result cannot go stale
 * between finding the handler and invoking it.
 */
int do_mmio(mmio_info_t *info, paddr_t fault_addr, unsigned int len)
{
    /* Fault address should be aligned to length of MMIO */
    if ( (len & (len - 1)) || (fault_addr & (len - 1)) )
        return -EIO;

    info->gpa = fault_addr;
    info->len = len;

    switch ( try_handle_mmio(info) )
    {
    case IO_HANDLED:
        return 0;

    case IO_ABORT:
        return -EIO;

    default:
        return -EOPNOTSUPP;
    }
}

int register_mmio_handler(struct domain *d,
                          const struct mmio_handler_ops *ops,
                          paddr_t addr, paddr_t size)
{
    struct vmmio *vmmio = d->arch.vmmio;
    struct mmio_handler *handlers = vmmio->handlers;
    paddr_t end = addr + size;
    unsigned int i;
    int rc = 0;
    bool overlap;

    ASSERT(ops->read && ops->write);

    if ( end <= addr )
        return -EINVAL;

    write_lock(&vmmio->lock);

    if ( vmmio->num_entries >= ARRAY_SIZE(vmmio->handlers) )
    {
        rc = -ENOSPC;
        goto out;
    }

    /*
     * The array is kept sorted by base address, so rather than appending and
     * re-sorting, find the slot the new region belongs to and shift the tail
     * up by one.
     */
    for ( i = vmmio->num_entries;
          i > 0 && handlers[i - 1].addr > addr;
          i-- )
        /* Nothing to do */;

    /*
     * Regions are required not to overlap; check both neighbours. Their
     * addr + size cannot overflow, as such regions are rejected above when
     * they get registered.
     */
    overlap = (i > 0 && handlers[i - 1].addr + handlers[i - 1].size > addr) ||
              (i < vmmio->num_entries && end > handlers[i].addr);

    if ( overlap )
    {
        rc = -EADDRINUSE;
        goto out;
    }

    memmove(&handlers[i + 1], &handlers[i],
            (vmmio->num_entries - i) * sizeof(*handlers));

    handlers[i] = (struct mmio_handler){
        .addr = addr,
        .size = size,
        .ops = ops,
    };

    vmmio->num_entries++;

 out:
    write_unlock(&vmmio->lock);

    return rc;
}

int domain_io_init(struct domain *d)
{
    struct vmmio *vmmio = xvzalloc(struct vmmio);

    if ( !vmmio )
        return -ENOMEM;

    rwlock_init(&vmmio->lock);
    d->arch.vmmio = vmmio;

    return 0;
}

void domain_io_free(struct domain *d)
{
    XVFREE(d->arch.vmmio);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
