#include "kernel/calls.h"

struct futex {
    atomic_uint refcount;
    struct mem *mem;
    addr_t addr;
    cond_t cond;
    struct list chain; // locked by futex_hash_lock
};

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];

static void __attribute__((constructor)) init_futex_hash() {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

// returns the futex for the current process at the given addr, and locks it
static struct futex *futex_get(addr_t addr) {
    int hash = (addr ^ (unsigned long) current->mem) % FUTEX_HASH_SIZE;
    lock(&futex_lock);
    struct list *bucket = &futex_hash[hash];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (futex->addr == addr && futex->mem == current->mem) {
            futex->refcount++;
            return futex;
        }
    }

    futex = malloc(sizeof(struct futex));
    if (futex == NULL) {
        unlock(&futex_lock);
        return NULL;
    }
    futex->refcount = 1;
    futex->mem = current->mem;
    futex->addr = addr;
    cond_init(&futex->cond);
    list_add(bucket, &futex->chain);
    return futex;
}

// must be called on the result of futex_get when you're done with it
static void futex_put(struct futex *futex) {
    if (--futex->refcount == 0) {
        cond_destroy(&futex->cond);
        list_remove(&futex->chain);
        free(futex);
    }
    unlock(&futex_lock);
}

static int futex_load(struct futex *futex, dword_t *out) {
    assert(futex->mem == current->mem);
    dword_t *ptr = mem_ptr(current->mem, futex->addr, MEM_READ);
    if (ptr == NULL)
        return 1;
    *out = *ptr;
    return 0;
}

int futex_wait(addr_t uaddr, dword_t val) {
    struct futex *futex = futex_get(uaddr);
    int err = 0;
    dword_t tmp;
    if (futex_load(futex, &tmp))
        err = _EFAULT;
    else if (tmp != val)
        err = _EAGAIN;
    else
        wait_for(&futex->cond, &futex_lock);
    futex_put(futex);
    return err;
}

int futex_wake(addr_t uaddr, dword_t val) {
    struct futex *futex = futex_get(uaddr);
    if (val == 1)
        notify_once(&futex->cond);
    else if (val == 0x7fffffff)
        notify(&futex->cond);
    else
        TODO("invalid number of futex wakes %d", val);
    futex_put(futex);
    return val; // FIXME wrong if val is INT_MAX
}

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_PRIVATE_FLAG_ 128
#define FUTEX_CLOCK_REALTIME_ = 256
#define FUTEX_CMD_MASK_ ~(FUTEX_PRIVATE_FLAG_ | FUTEX_CLOCK_REALTIME_)

dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val) {
    if (!(op & FUTEX_PRIVATE_FLAG_)) {
        FIXME("no support for shared futexes");
    }
    if (!(op & FUTEX_PRIVATE_FLAG_)) {
        FIXME("no support for FUTEX_CLOCK_REALTIME");
        return _ENOSYS;
    }
    switch (op & FUTEX_CMD_MASK_) {
        case FUTEX_WAIT_:
            STRACE("futex_wait(0x%x, %d)", uaddr, val);
            return futex_wait(uaddr, val);
        case FUTEX_WAKE_:
            STRACE("futex_wake(0x%x, %d)", uaddr, val);
            return futex_wake(uaddr, val);
    }
    FIXME("unsupported futex operation %d", op);
    return _ENOSYS;
}
