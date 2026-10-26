// HLE kernel: the SVC interface of Horizon, implemented on the host.
//
// Every guest thread is a host thread and they run truly in parallel (guest code
// is written for a multi-core machine and its atomics are real host atomics).
// The kernel lock `K` protects kernel state only; blocking SVCs wait on `KC`.
// Synchronisation follows the Horizon kernel: user-mode mutexes with
// ArbitrateLock/Unlock, process-wide condition variables, the address arbiter
// and WaitSynchronization on events / threads.

#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t K = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  KC;
static __thread guest_thread_t *tl_current;

#define MAX_THREADS 256
static guest_thread_t *g_threads[MAX_THREADS];
static int             g_thread_count;

#define HAS_WAITERS 0x40000000u
enum { W_NONE = 0, W_LOCK = 1, W_CONDVAR = 2, W_ARBITER = 3 };

// Kernel result codes (module 1).
#define R_OK               0u
#define R_INVALID_SIZE     0xCA01u
#define R_INVALID_ADDRESS  0xCC01u
#define R_INVALID_MEMSTATE 0xD401u
#define R_INVALID_HANDLE   0xE401u
#define R_TIMED_OUT        0xEA01u
#define R_CANCELLED        0xEC01u
#define R_INVALID_ENUM     0xF001u
#define R_NOT_FOUND        0xF201u
#define R_OUT_OF_RESOURCE  0xD201u
#define R_INVALID_STATE    0xFA01u

#define PSEUDO_CURRENT_THREAD  0xFFFF8000u
#define PSEUDO_CURRENT_PROCESS 0xFFFF8001u

typedef enum { H_FREE = 0, H_THREAD, H_EVENT_W, H_EVENT_R, H_TRANSFER_MEM, H_SESSION, H_PROCESS, H_RESOURCE_LIMIT, H_OTHER } htype_t;

typedef struct { int signaled; } event_t;

typedef struct
{
    htype_t type;
    void   *obj;
    uint64_t a, b; // type specific
} handle_entry_t;

#define MAX_HANDLES 4096
#define HANDLE_BASE 0x100u
static handle_entry_t handles[MAX_HANDLES];

static uint64_t       g_program_id;
#define g_current tl_current
static uint64_t       g_heap_size;
static uint64_t       g_next_thread_id = 0x50;
static uint64_t       g_svc_count;
static uint64_t       g_svc_hist[256];
static uint64_t       g_tls_next = TLS_REGION_BASE;

uint32_t handle_alloc(htype_t t, void *obj, uint64_t a, uint64_t b)
{
    for (uint32_t i = 1; i < MAX_HANDLES; ++i)
        if (handles[i].type == H_FREE) {
            handles[i] = (handle_entry_t){ t, obj, a, b };
            return HANDLE_BASE + i;
        }
    host_fatal("out of handles");
}

handle_entry_t *handle_get(uint32_t h)
{
    if (h < HANDLE_BASE + 1 || h >= HANDLE_BASE + MAX_HANDLES)
        return NULL;
    handle_entry_t *e = &handles[h - HANDLE_BASE];
    return e->type == H_FREE ? NULL : e;
}

static void handle_free(uint32_t h)
{
    handle_entry_t *e = handle_get(h);
    if (e)
        e->type = H_FREE;
}

uint32_t kernel_new_event(int signaled, void **out_event)
{
    event_t *ev  = calloc(1, sizeof *ev);
    ev->signaled = signaled;
    if (out_event)
        *out_event = ev;
    return handle_alloc(H_EVENT_R, ev, 0, 0);
}

void kernel_event_set(void *event, int signaled) { ((event_t *)event)->signaled = signaled; }

void kernel_event_signal(void *event)
{
    pthread_mutex_lock(&K);
    ((event_t *)event)->signaled = 1;
    pthread_cond_broadcast(&KC);
    pthread_mutex_unlock(&K);
}

// ---- time ------------------------------------------------------------------

#define TICK_HZ 19200000ull
uint64_t recomp_cntfrq(void) { return TICK_HZ; }
uint64_t recomp_cntpct(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * TICK_HZ + (uint64_t)ts.tv_nsec * TICK_HZ / 1000000000ull;
}

// ---- threads -----------------------------------------------------------------

static uint64_t alloc_tls(void)
{
    // One 0x200-byte thread local region per thread, 8 per page.
    if ((g_tls_next & (PAGE_SIZE_ - 1)) == 0 && mem_map(g_tls_next, PAGE_SIZE_, MS_THREAD_LOCAL, MP_RW))
        host_fatal("cannot map TLS page");
    uint64_t a = g_tls_next;
    g_tls_next += 0x200;
    return a;
}

static guest_thread_t *new_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core)
{
    guest_thread_t *t = calloc(1, sizeof *t);
    t->entry     = entry;
    t->arg       = arg;
    t->stack_top = stack_top;
    t->priority  = prio;
    t->core      = core;
    t->id        = g_next_thread_id++;
    t->tls       = alloc_tls();
    t->handle    = handle_alloc(H_THREAD, t, 0, 0);
    t->cpu.tpidrro = t->tls;
    t->cpu.sp      = stack_top;
    t->cpu.x[0]    = arg;
    t->cpu.host    = t;
    if (g_thread_count >= MAX_THREADS)
        host_fatal("too many threads");
    g_threads[g_thread_count++] = t;
    return t;
}

guest_thread_t *kernel_current_thread(void) { return tl_current; }

static void *thread_main(void *p)
{
    guest_thread_t *t = p;
    tl_current        = t;
    jmp_buf jb;
    t->exit_jmp = &jb;
    if (!setjmp(jb))
        recomp_dispatch(&t->cpu, t->entry);
    pthread_mutex_lock(&K);
    t->state = 2;
    pthread_cond_broadcast(&KC);
    pthread_mutex_unlock(&K);
    host_log("thread 0x%x (entry %s) exited", t->handle, addr_describe(t->entry));
    return NULL;
}

static void start_host_thread(guest_thread_t *t)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20); // recompiled frames can be large and deep
    pthread_t *pt = malloc(sizeof *pt);
    if (pthread_create(pt, &a, thread_main, t) != 0)
        host_fatal("pthread_create failed");
    t->host_thread = pt;
    pthread_attr_destroy(&a);
}

_Noreturn void kernel_run_main_thread(guest_thread_t *t)
{
    start_host_thread(t);
    pthread_join(*(pthread_t *)t->host_thread, NULL);
    host_log("main thread exited");
    kernel_exit(0);
}

// ---- blocking ------------------------------------------------------------------

static clockid_t g_wait_clock = CLOCK_REALTIME; // clock used by KC's timed waits

static void deadline_after(struct timespec *d, int64_t ns)
{
    clock_gettime(g_wait_clock, d);
    d->tv_sec += ns / 1000000000;
    d->tv_nsec += ns % 1000000000;
    if (d->tv_nsec >= 1000000000) { d->tv_sec++; d->tv_nsec -= 1000000000; }
}

// Waits (K held) until t->woken, cancellation, or the timeout. Returns 1 if woken.
static int block(guest_thread_t *t, int64_t timeout_ns, int cancellable)
{
    struct timespec d;
    if (timeout_ns > 0)
        deadline_after(&d, timeout_ns);
    while (!t->woken) {
        if (cancellable && t->cancel)
            return 0;
        if (timeout_ns == 0)
            return 0;
        if (timeout_ns > 0) {
            if (pthread_cond_timedwait(&KC, &K, &d) == ETIMEDOUT)
                return t->woken;
        } else {
            pthread_cond_wait(&KC, &K);
        }
    }
    return 1;
}

// Hands the user-mode mutex at `addr` to the best waiting thread, or frees it.
static void release_lock(uint64_t addr)
{
    guest_thread_t *best = NULL;
    int             others = 0;
    for (int i = 0; i < g_thread_count; ++i) {
        guest_thread_t *w = g_threads[i];
        if (w->wait_kind != W_LOCK || w->wait_addr != addr)
            continue;
        if (!best || w->priority < best->priority) {
            if (best) ++others;
            best = w;
        } else {
            ++others;
        }
    }
    if (!best) {
        wr32(addr, 0);
        return;
    }
    wr32(addr, best->mutex_tag | (others ? HAS_WAITERS : 0));
    best->wait_kind = W_NONE;
    best->woken     = 1;
    pthread_cond_broadcast(&KC);
}

// Acquires the mutex for thread w (used when a condition variable wait ends).
// Returns 1 if acquired immediately, otherwise w becomes a lock waiter.
static int acquire_or_queue(guest_thread_t *w, uint64_t mutex, uint32_t tag)
{
    uint32_t v = rd32(mutex);
    if (v == 0) {
        wr32(mutex, tag);
        return 1;
    }
    wr32(mutex, v | HAS_WAITERS);
    w->wait_kind = W_LOCK;
    w->wait_addr = mutex;
    w->mutex_tag = tag;
    w->woken     = 0;
    return 0;
}

static int count_waiters(int kind, uint64_t addr)
{
    int n = 0;
    for (int i = 0; i < g_thread_count; ++i)
        n += g_threads[i]->wait_kind == kind && g_threads[i]->wait_addr == addr;
    return n;
}

static void wake_arbiter(uint64_t addr, int32_t count)
{
    for (int i = 0; i < g_thread_count; ++i) {
        guest_thread_t *w = g_threads[i];
        if (w->wait_kind == W_ARBITER && w->wait_addr == addr) {
            w->wait_kind = W_NONE;
            w->woken     = 1;
            if (count > 0 && --count == 0)
                break;
        }
    }
    pthread_cond_broadcast(&KC);
}

guest_thread_t *kernel_create_main_thread(uint64_t entry, uint64_t stack_top, int priority, int core)
{
    guest_thread_t *t = new_thread(entry, 0, stack_top, priority, core);
    t->state = 1;
    return t;
}

static guest_thread_t *thread_from_handle(uint32_t h)
{
    if (h == PSEUDO_CURRENT_THREAD)
        return g_current;
    handle_entry_t *e = handle_get(h);
    return (e && e->type == H_THREAD) ? e->obj : NULL;
}

void kernel_init(uint64_t program_id)
{
    g_program_id = program_id;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    if (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0)
        g_wait_clock = CLOCK_MONOTONIC; // not available everywhere (e.g. winpthreads)
    pthread_cond_init(&KC, &ca);
}

_Noreturn void kernel_exit(int code)
{
    host_log("---- exit (%d) after %llu SVCs; calls: %llu recompiled, %llu interpreted", code,
             (unsigned long long)g_svc_count, (unsigned long long)g_stats_compiled_calls,
             (unsigned long long)g_stats_interp_calls);
    fflush(stdout);
    exit(code);
}

void guest_backtrace(const cpu_t *c)
{
    host_log("  guest call stack (frame pointers):");
    uint64_t fp = c->x[29];
    host_log("    lr %s", addr_describe(c->x[30]));
    for (int depth = 0; depth < 32 && fp && (fp & 7) == 0; ++depth) {
        memory_info_t mi;
        mem_query(fp, &mi);
        if (mi.state == MS_FREE || mi.state == MS_INACCESSIBLE)
            break;
        uint64_t ret = rd64(fp + 8);
        host_log("    #%-2d %s", depth, addr_describe(ret));
        fp = rd64(fp);
    }
}

// ---- SVC helpers -----------------------------------------------------------------

static const char *svc_name(uint32_t n)
{
    static const char *names[256] = {
        [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute",
        [0x04] = "MapMemory", [0x05] = "UnmapMemory", [0x06] = "QueryMemory", [0x07] = "ExitProcess",
        [0x08] = "CreateThread", [0x09] = "StartThread", [0x0A] = "ExitThread", [0x0B] = "SleepThread",
        [0x0C] = "GetThreadPriority", [0x0D] = "SetThreadPriority", [0x0E] = "GetThreadCoreMask",
        [0x0F] = "SetThreadCoreMask", [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent",
        [0x12] = "ClearEvent", [0x13] = "MapSharedMemory", [0x14] = "UnmapSharedMemory",
        [0x15] = "CreateTransferMemory", [0x16] = "CloseHandle", [0x17] = "ResetSignal",
        [0x18] = "WaitSynchronization", [0x19] = "CancelSynchronization", [0x1A] = "ArbitrateLock",
        [0x1B] = "ArbitrateUnlock", [0x1C] = "WaitProcessWideKeyAtomic", [0x1D] = "SignalProcessWideKey",
        [0x1E] = "GetSystemTick", [0x1F] = "ConnectToNamedPort", [0x21] = "SendSyncRequest",
        [0x22] = "SendSyncRequestWithUserBuffer", [0x23] = "SendAsyncRequestWithUserBuffer",
        [0x25] = "GetThreadId", [0x26] = "Break", [0x27] = "OutputDebugString", [0x28] = "ReturnFromException",
        [0x29] = "GetInfo", [0x2C] = "MapPhysicalMemory", [0x2D] = "UnmapPhysicalMemory",
        [0x32] = "SetThreadActivity", [0x33] = "GetThreadContext3", [0x34] = "WaitForAddress",
        [0x35] = "SignalToAddress", [0x36] = "SynchronizePreemptionState", [0x40] = "CreateSession",
        [0x41] = "AcceptSession", [0x43] = "ReplyAndReceive", [0x44] = "ReplyAndReceiveWithUserBuffer",
        [0x45] = "CreateEvent", [0x4B] = "CreateCodeMemory", [0x50] = "CreateSharedMemory",
        [0x51] = "MapTransferMemory", [0x52] = "UnmapTransferMemory", [0x70] = "CreatePort",
        [0x72] = "ConnectToPort",
    };
    return (n < 256 && names[n]) ? names[n] : "?";
}

#define X(i) (c->x[i])
#define W(i) ((uint32_t)c->x[i])
#define RET(r) do { c->x[0] = (r); return; } while (0)

static void svc_get_info(cpu_t *c)
{
    uint32_t id0 = W(1), h = W(2);
    uint64_t id1 = X(3), out = 0;
    (void)h;
    switch (id0) {
    case 0: out = 0xF; break;                               // core mask
    case 1: out = 0xFFFFFFFFFFFFFFFFull; break;             // priority mask
    case 2: out = ALIAS_REGION_BASE; break;
    case 3: out = ALIAS_REGION_SIZE; break;
    case 4: out = HEAP_REGION_BASE; break;
    case 5: out = HEAP_REGION_SIZE; break;
    case 6: out = 0x1C0000000ull; break;                    // total memory available to the application
    case 7: out = mem_used_total(); break;                  // used memory
    case 8: out = 0; break;                                 // debugger attached
    case 9: out = handle_alloc(H_RESOURCE_LIMIT, NULL, 0, 0); break;
    case 10: out = 0; break;                                // idle tick count
    case 11: out = ((uint64_t)rand() << 32) ^ (uint64_t)rand() ^ id1 * 0x9E3779B97F4A7C15ull; break;
    case 12: out = ASLR_REGION_BASE; break;
    case 13: out = ASLR_REGION_SIZE; break;
    case 14: out = STACK_REGION_BASE; break;
    case 15: out = STACK_REGION_SIZE; break;
    case 16: out = 0; break;                                // system resource size
    case 17: out = 0; break;
    case 18: out = g_program_id; break;
    case 20: out = g_current->tls + 0x100; break;           // user exception context
    case 21: out = 0x1C0000000ull; break;
    case 22: out = mem_used_total(); break;                 // used non-system memory
    case 23: out = 1; break;                                // is application
    case 24: out = 64; break;                               // free thread count
    case 25: out = 0; break;                                // thread tick count
    case 26: out = 1; break;                                // svc permitted
    case 28: out = 0; break;                                // alias region extra size
    case 29: out = EXTRA_REGION_BASE; break;                // Switch 2: unknown region (address)
    case 30: out = EXTRA_REGION_SIZE; break;                // Switch 2: unknown region (size)
    case 31: out = EXTRA2_REGION_BASE; break;               // Switch 2: unknown region (address)
    case 32: out = EXTRA2_REGION_SIZE; break;               // Switch 2: unknown region (size)
    case 33: out = 0x1000; break;                           // Switch 2: page granularity (used to align RELRO protection and the first MapPhysicalMemory)
    default:
        host_log("GetInfo: unknown id 0x%x / 0x%llx", id0, (unsigned long long)id1);
        RET(R_INVALID_ENUM);
    }
    if (host_verbose)
        host_log("  GetInfo(%u, 0x%llx) = 0x%llx", id0, (unsigned long long)id1, (unsigned long long)out);
    c->x[1] = out;
    RET(R_OK);
}

static void svc_wait_sync(cpu_t *c)
{
    guest_thread_t *t = g_current;
    uint64_t list = X(1);
    int32_t  count = (int32_t)W(2);
    int64_t  timeout = (int64_t)X(3);
    struct timespec d;
    if (timeout > 0)
        deadline_after(&d, timeout);
    for (;;) {
        for (int32_t i = 0; i < count; ++i) {
            uint32_t        h = rd32(list + 4 * (uint64_t)i);
            handle_entry_t *e = handle_get(h);
            if (!e && h == PSEUDO_CURRENT_THREAD)
                continue;
            if (!e)
                RET(R_INVALID_HANDLE);
            if ((e->type == H_EVENT_R || e->type == H_EVENT_W) && ((event_t *)e->obj)->signaled) {
                c->x[1] = (uint64_t)i;
                RET(R_OK);
            }
            if (e->type == H_THREAD && ((guest_thread_t *)e->obj)->state == 2) {
                c->x[1] = (uint64_t)i;
                RET(R_OK);
            }
        }
        if (t->cancel) {
            t->cancel = 0;
            RET(R_CANCELLED);
        }
        if (timeout == 0)
            RET(R_TIMED_OUT);
        if (timeout > 0) {
            if (pthread_cond_timedwait(&KC, &K, &d) == ETIMEDOUT)
                RET(R_TIMED_OUT);
        } else {
            pthread_cond_wait(&KC, &K);
        }
    }
}

static void svc_wait_cv(cpu_t *c) // WaitProcessWideKeyAtomic(mutex, key, tag, timeout)
{
    guest_thread_t *t = g_current;
    uint64_t mutex = X(0), key = X(1);
    uint32_t tag = W(2);
    int64_t  timeout = (int64_t)X(3);
    release_lock(mutex);
    wr32(key, 1);
    t->wait_kind  = W_CONDVAR;
    t->wait_addr  = key;
    t->mutex_addr = mutex;
    t->mutex_tag  = tag;
    t->woken      = 0;
    if (block(t, timeout, 1))
        RET(R_OK); // signalled and the mutex was handed to us
    uint32_t result = t->cancel ? R_CANCELLED : R_TIMED_OUT;
    t->cancel = 0;
    if (t->wait_kind == W_CONDVAR) { // timed out while still on the condition variable
        t->wait_kind = W_NONE;
        if (acquire_or_queue(t, mutex, tag))
            RET(result);
    }
    block(t, -1, 0); // now a lock waiter: wait to be handed the mutex
    RET(result);
}

static void svc_signal_cv(cpu_t *c) // SignalProcessWideKey(key, count)
{
    uint64_t key = X(0);
    int32_t  count = (int32_t)W(1);
    for (int i = 0; i < g_thread_count && count != 0; ++i) {
        guest_thread_t *w = g_threads[i];
        if (w->wait_kind != W_CONDVAR || w->wait_addr != key)
            continue;
        w->wait_kind = W_NONE;
        if (acquire_or_queue(w, w->mutex_addr, w->mutex_tag))
            w->woken = 1;
        if (count > 0)
            --count;
    }
    if (!count_waiters(W_CONDVAR, key))
        wr32(key, 0);
    pthread_cond_broadcast(&KC);
    RET(R_OK);
}

static void kernel_svc_locked(cpu_t *c, uint32_t imm);

void kernel_svc(cpu_t *c, uint32_t imm)
{
    if (!tl_current)
        tl_current = c->host;
    pthread_mutex_lock(&K);
    kernel_svc_locked(c, imm);
    if (host_verbose && imm != 0x1E && imm != 0x10 && (uint32_t)c->x[0] != 0)
        host_log("  -> svc 0x%02x %s failed: 0x%x (thread 0x%x)", imm, svc_name(imm), (uint32_t)c->x[0],
                 tl_current ? tl_current->handle : 0);
    pthread_mutex_unlock(&K);
}

static void kernel_svc_locked(cpu_t *c, uint32_t imm)
{
    ++g_svc_count;
    ++g_svc_hist[imm & 0xFF];
    if (host_verbose)
        host_log("svc 0x%02x %-28s x0=%llx x1=%llx x2=%llx x3=%llx  from %s (lr %s)", imm, svc_name(imm),
                 (unsigned long long)X(0), (unsigned long long)X(1), (unsigned long long)X(2),
                 (unsigned long long)X(3), addr_describe(c->pc), addr_describe(c->x[30]));

    switch (imm) {
    case 0x01: { // SetHeapSize(size) -> addr
        uint64_t size = X(1);
        if (size & 0x1FFFFF)
            RET(R_INVALID_SIZE);
        if (size > g_heap_size)
            mem_map(HEAP_REGION_BASE + g_heap_size, size - g_heap_size, MS_NORMAL, MP_RW);
        else if (size < g_heap_size)
            mem_unmap(HEAP_REGION_BASE + size, g_heap_size - size);
        g_heap_size = size;
        c->x[1] = HEAP_REGION_BASE;
        RET(R_OK);
    }
    case 0x02: mem_protect(X(0), X(1), W(2)); RET(R_OK);
    case 0x03: RET(R_OK); // SetMemoryAttribute (cache attributes): nothing to do
    case 0x04: { // MapMemory(dst, src, size): stack mirror. We copy instead of aliasing.
        uint64_t dst = X(0), src = X(1), size = X(2);
        if (mem_map(dst, size, MS_STACK, MP_RW))
            RET(R_INVALID_MEMSTATE);
        g_write(dst, mem_host(src), size);
        mem_protect(src, size, MP_NONE);
        RET(R_OK);
    }
    case 0x05: { // UnmapMemory(dst, src, size)
        uint64_t dst = X(0), src = X(1), size = X(2);
        g_write(src, mem_host(dst), size);
        mem_unmap(dst, size);
        mem_protect(src, size, MP_RW);
        RET(R_OK);
    }
    case 0x06: { // QueryMemory(out_info, ?, addr)
        memory_info_t mi;
        mem_query(X(2), &mi);
        g_write(X(0), &mi, sizeof mi);
        if (host_verbose)
            host_log("  QueryMemory(0x%llx) = [0x%llx, +0x%llx) state %x perm %x", (unsigned long long)X(2),
                     (unsigned long long)mi.addr, (unsigned long long)mi.size, mi.state, mi.perm);
        c->x[1] = 0;
        RET(R_OK);
    }
    case 0x07: host_log("ExitProcess"); kernel_exit(0);
    case 0x08: { // CreateThread(entry, arg, stack_top, prio, core) -> handle
        guest_thread_t *t = new_thread(X(1), X(2), X(3), (int)W(4), (int)W(5));
        host_log("CreateThread entry %s arg 0x%llx stack 0x%llx prio %d core %d -> handle 0x%x",
                 addr_describe(t->entry), (unsigned long long)t->arg, (unsigned long long)t->stack_top,
                 t->priority, t->core, t->handle);
        c->x[1] = t->handle;
        RET(R_OK);
    }
    case 0x09: {
        guest_thread_t *t = thread_from_handle(W(0));
        if (!t)
            RET(R_INVALID_HANDLE);
        t->state = 1;
        host_log("StartThread 0x%x (entry %s)", t->handle, addr_describe(t->entry));
        start_host_thread(t);
        RET(R_OK);
    }
    case 0x0A: { // ExitThread
        guest_thread_t *t = g_current;
        pthread_mutex_unlock(&K);
        longjmp(*(jmp_buf *)t->exit_jmp, 1);
    }
    case 0x0B: { // SleepThread(ns); 0 / -1 / -2 are yields
        int64_t ns = (int64_t)X(0);
        pthread_mutex_unlock(&K);
        if (ns > 0) {
            struct timespec ts = { ns / 1000000000, ns % 1000000000 };
            nanosleep(&ts, NULL);
        } else {
            sched_yield();
        }
        pthread_mutex_lock(&K);
        return;
    }
    case 0x0C: { guest_thread_t *t = thread_from_handle(W(1)); if (!t) RET(R_INVALID_HANDLE); c->x[1] = (uint64_t)t->priority; RET(R_OK); }
    case 0x0D: { guest_thread_t *t = thread_from_handle(W(0)); if (!t) RET(R_INVALID_HANDLE); t->priority = (int)W(1); RET(R_OK); }
    case 0x0E: { guest_thread_t *t = thread_from_handle(W(2)); if (!t) RET(R_INVALID_HANDLE); c->x[1] = (uint64_t)t->core; c->x[2] = 0xF; RET(R_OK); }
    case 0x0F: RET(R_OK);
    case 0x10: c->x[0] = 0; return;
    case 0x11: case 0x12: case 0x17: { // SignalEvent / ClearEvent / ResetSignal
        handle_entry_t *e = handle_get(W(0));
        if (!e || (e->type != H_EVENT_W && e->type != H_EVENT_R))
            RET(R_INVALID_HANDLE);
        if (imm == 0x17 && e->type == H_EVENT_R && !((event_t *)e->obj)->signaled)
            RET(R_INVALID_STATE);
        ((event_t *)e->obj)->signaled = (imm == 0x11);
        pthread_cond_broadcast(&KC);
        RET(R_OK);
    }
    case 0x13: { // MapSharedMemory(handle, addr, size, perm)
        if (mem_map(X(1), X(2), MS_SHARED, W(3)))
            RET(R_INVALID_MEMSTATE);
        RET(R_OK);
    }
    case 0x14: mem_unmap(X(1), X(2)); RET(R_OK);
    case 0x15: { // CreateTransferMemory(addr, size, perm) -> handle
        c->x[1] = handle_alloc(H_TRANSFER_MEM, NULL, X(1), X(2));
        RET(R_OK);
    }
    case 0x16: {
        uint32_t h = W(0);
        if (ipc_is_session(h))
            ipc_close(h);
        handle_free(h);
        RET(R_OK);
    }
    case 0x18: svc_wait_sync(c); return;
    case 0x19: { // CancelSynchronization(thread)
        guest_thread_t *t = thread_from_handle(W(0));
        if (!t)
            RET(R_INVALID_HANDLE);
        t->cancel = 1;
        pthread_cond_broadcast(&KC);
        RET(R_OK);
    }
    case 0x1A: { // ArbitrateLock(owner, addr, tag)
        guest_thread_t *t = g_current;
        uint32_t owner = W(0), tag = W(2);
        uint64_t addr = X(1);
        if (rd32(addr) != (owner | HAS_WAITERS))
            RET(R_OK); // the lock changed hands meanwhile: user mode retries
        t->wait_kind = W_LOCK;
        t->wait_addr = addr;
        t->mutex_tag = tag;
        t->woken     = 0;
        block(t, -1, 0);
        RET(R_OK);
    }
    case 0x1B: release_lock(X(0)); RET(R_OK); // ArbitrateUnlock
    case 0x1C: svc_wait_cv(c); return;
    case 0x1D: svc_signal_cv(c); return;
    case 0x1E: c->x[0] = recomp_cntpct(); return;
    case 0x1F: { // ConnectToNamedPort(name) -> handle
        char name[16] = {0};
        g_read(X(1), name, 12);
        uint32_t h = 0;
        uint32_t r = ipc_connect_named_port(name, &h);
        c->x[1] = h;
        RET(r);
    }
    case 0x21: RET(ipc_send_sync_request(c, W(0), g_current->tls, 0x100));
    case 0x22: RET(ipc_send_sync_request(c, W(2), X(0), X(1)));
    case 0x25: { guest_thread_t *t = thread_from_handle(W(1)); if (!t) RET(R_INVALID_HANDLE); c->x[1] = t->id; RET(R_OK); }
    case 0x26: { // Break(reason, info_addr, info_size)
        host_log("Break(reason 0x%x, 0x%llx, 0x%llx) from %s", W(0), (unsigned long long)X(1),
                 (unsigned long long)X(2), addr_describe(c->pc));
        if (X(2) == 4)
            host_log("  result code 0x%x", rd32(X(1)));
        const char *watch = getenv("RECOMP_WATCH"); // comma separated module offsets, e.g. main+0x26d50f8
        while (watch && *watch) {
            char mod[16] = {0};
            unsigned long long off = 0;
            if (sscanf(watch, "%15[^+]+%llx", mod, &off) == 2)
                for (int i = 0; i < g_module_count; ++i)
                    if (!strcmp(g_modules[i].name, mod))
                        host_log("  watch %s+0x%llx = 0x%016llx", mod, off,
                                 (unsigned long long)rd64(g_modules[i].base + off));
            watch = strchr(watch, ',');
            if (watch) ++watch;
        }
        guest_backtrace(c);
        kernel_exit(3);
    }
    case 0x27: { // OutputDebugString(str, len)
        size_t n = X(1) > 4096 ? 4096 : X(1);
        char  *s = malloc(n + 1);
        g_read(X(0), s, n);
        s[n] = 0;
        host_log("[guest] %s", s);
        free(s);
        RET(R_OK);
    }
    case 0x29: svc_get_info(c); return;
    case 0x2C: // MapPhysicalMemory(addr, size)
        if (X(1) == 0)
            RET(R_OK);
        if (mem_map(X(0), X(1), MS_ALIAS, MP_RW))
            RET(R_INVALID_MEMSTATE);
        RET(R_OK);
    case 0x2D: mem_unmap(X(0), X(1)); RET(R_OK);
    case 0x34: { // WaitForAddress(addr, type, value, timeout)
        guest_thread_t *t = g_current;
        uint64_t addr = X(0);
        uint32_t type = W(1);
        int32_t  value = (int32_t)W(2), cur = (int32_t)rd32(addr);
        int64_t  timeout = (int64_t)X(3);
        if (type == 0) {        // WaitIfLessThan
            if (cur >= value) RET(R_INVALID_STATE);
        } else if (type == 1) { // DecrementAndWaitIfLessThan
            if (cur >= value) RET(R_INVALID_STATE);
            wr32(addr, (uint32_t)(cur - 1));
        } else if (type == 2) { // WaitIfEqual
            if (cur != value) RET(R_INVALID_STATE);
        } else {
            RET(R_INVALID_ENUM);
        }
        if (timeout == 0)
            RET(R_TIMED_OUT);
        t->wait_kind = W_ARBITER;
        t->wait_addr = addr;
        t->woken     = 0;
        if (block(t, timeout, 1))
            RET(R_OK);
        t->wait_kind = W_NONE;
        if (t->cancel) { t->cancel = 0; RET(R_CANCELLED); }
        RET(R_TIMED_OUT);
    }
    case 0x35: { // SignalToAddress(addr, type, value, count)
        uint64_t addr = X(0);
        uint32_t type = W(1);
        int32_t  value = (int32_t)W(2), count = (int32_t)W(3);
        if (type == 1) {        // SignalAndIncrementIfEqual
            if ((int32_t)rd32(addr) != value) RET(R_INVALID_STATE);
            wr32(addr, (uint32_t)(value + 1));
        } else if (type == 2) { // SignalAndModifyByWaitingCountIfEqual
            if ((int32_t)rd32(addr) != value) RET(R_INVALID_STATE);
            int n = count_waiters(W_ARBITER, addr);
            int32_t nv;
            if (count <= 0) nv = n > 0 ? value - 2 : value + 1;
            else if (n == 0) nv = value + 1;
            else if (n <= count) nv = value - 1;
            else nv = value;
            wr32(addr, (uint32_t)nv);
        }
        wake_arbiter(addr, count);
        RET(R_OK);
    }
    case 0x36: RET(R_OK);
    case 0x45: { // CreateEvent -> (writable, readable)
        event_t *ev = calloc(1, sizeof *ev);
        c->x[1] = handle_alloc(H_EVENT_W, ev, 0, 0);
        c->x[2] = handle_alloc(H_EVENT_R, ev, 0, 0);
        RET(R_OK);
    }
    default:
        break;
    }
    host_fatal("unimplemented SVC 0x%02x (%s) from %s, x0=%llx x1=%llx x2=%llx x3=%llx", imm, svc_name(imm),
               addr_describe(c->pc), (unsigned long long)X(0), (unsigned long long)X(1), (unsigned long long)X(2),
               (unsigned long long)X(3));
}
