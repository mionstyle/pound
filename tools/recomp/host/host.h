// Host runtime for statically recompiled Switch 2 titles.
//
// Pieces:
//   memory.c  - guest address space (flat host mapping) and the region map used by svcQueryMemory
//   loader.c  - places rtld / main / sdk (NSO) in memory like the Horizon loader does
//   exec.c    - dispatch: recompiled function if available, otherwise the Unicorn fallback
//   kernel.c  - SVC implementations (HLE kernel)
//   ipc.c     - IPC message decoding and the service manager
//   main.c    - command line entry point

#ifndef RECOMP_HOST_H
#define RECOMP_HOST_H

#include "recomp_cpu.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- logging --------------------------------------------------------------

void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void host_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int host_verbose;

// ---- guest address space (39-bit, like a 39-bit Horizon process) ------------

#define GUEST_ADDR_BITS 39
#define PAGE_SIZE_      0x1000ull

#define ASLR_REGION_BASE  0x0000008000000ull
#define ASLR_REGION_SIZE  (0x8000000000ull - ASLR_REGION_BASE)
#define CODE_BASE         0x0000008000000ull
#define HEAP_REGION_BASE  0x1000000000ull
#define HEAP_REGION_SIZE  0x1000000000ull
#define ALIAS_REGION_BASE 0x2000000000ull
#define ALIAS_REGION_SIZE 0x1000000000ull
#define STACK_REGION_BASE 0x3000000000ull
#define STACK_REGION_SIZE 0x0100000000ull
#define TLS_REGION_BASE   0x3100000000ull // thread local regions (one page per 8 threads)
// GetInfo 0x1D / 0x1E: a further (addr, size) region queried by the Switch 2 SDK next to
// heap / alias / ASLR. Its purpose is not known yet; we reserve an empty range for it.
#define EXTRA_REGION_BASE 0x3200000000ull
#define EXTRA_REGION_SIZE 0x0800000000ull

// Horizon memory states (svc::MemoryState, low byte) and permissions.
enum
{
    MS_FREE = 0x00, MS_IO = 0x01, MS_STATIC = 0x02, MS_CODE = 0x03, MS_CODE_DATA = 0x04,
    MS_NORMAL = 0x05, MS_SHARED = 0x06, MS_ALIAS = 0x07, MS_ALIAS_CODE = 0x08,
    MS_ALIAS_CODE_DATA = 0x09, MS_IPC = 0x0A, MS_STACK = 0x0B, MS_THREAD_LOCAL = 0x0C,
    MS_TRANSFERRED = 0x0D, MS_SHARED_TRANSFERRED = 0x0E, MS_SHARED_CODE = 0x0F,
    MS_INACCESSIBLE = 0x10,
};
enum { MP_NONE = 0, MP_R = 1, MP_W = 2, MP_X = 4, MP_RW = 3, MP_RX = 5 };

typedef struct
{
    uint64_t addr;
    uint64_t size;
    uint32_t state;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc_count;
    uint32_t device_count;
    uint32_t pad;
} memory_info_t; // layout of svc::MemoryInfo (0x28 bytes)

int      mem_init(void);
int      mem_map(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);
int      mem_unmap(uint64_t addr, uint64_t size);
int      mem_protect(uint64_t addr, uint64_t size, uint32_t perm);
void     mem_query(uint64_t addr, memory_info_t *out);
bool     mem_is_free(uint64_t addr, uint64_t size);
uint64_t mem_find_free(uint64_t base, uint64_t limit, uint64_t size);
void    *mem_host(uint64_t addr);
void     mem_dump_map(void);

static inline uint32_t g_rd32(uint64_t a) { return rd32(a); }
static inline uint64_t g_rd64(uint64_t a) { return rd64(a); }
static inline void     g_wr32(uint64_t a, uint32_t v) { wr32(a, v); }
static inline void     g_wr64(uint64_t a, uint64_t v) { wr64(a, v); }
void g_read(uint64_t a, void *dst, size_t n);
void g_write(uint64_t a, const void *src, size_t n);

// ---- modules ----------------------------------------------------------------

typedef struct { uint64_t addr; recomp_fn_t fn; } recomp_entry_t;

typedef struct
{
    char                  name[16];
    uint64_t              base;
    uint64_t              size;
    const recomp_entry_t *table; // recompiled functions (offsets relative to base)
    size_t                table_size;
    uint64_t             *base_var; // recomp_module_base_<name>
} module_t;

#define MAX_MODULES 16
extern module_t g_modules[MAX_MODULES];
extern int      g_module_count;

int  loader_load_exefs(const char *exefs_dir);
const char *addr_describe(uint64_t addr); // "sdk+0x1234"

// ---- execution --------------------------------------------------------------

void exec_init(void);
void exec_map_region(uint64_t addr, uint64_t size); // make a region visible to the interpreter
void exec_unmap_region(uint64_t addr, uint64_t size);

extern uint64_t g_stats_compiled_calls;
extern uint64_t g_stats_interp_calls;

// ---- kernel -----------------------------------------------------------------

typedef struct guest_thread
{
    cpu_t    cpu;
    uint32_t handle;
    uint64_t id;
    uint64_t entry;
    uint64_t arg;
    uint64_t stack_top;
    uint64_t tls;
    int32_t  priority;
    int32_t  core;
    int      state;     // 0 created, 1 running, 2 exited
    int      cancel;    // CancelSynchronization pending
    // blocking state (protected by the kernel lock)
    int      wait_kind; // 0 none, 1 lock, 2 condition variable, 3 address arbiter
    int      woken;
    uint64_t wait_addr;
    uint64_t mutex_addr;
    uint32_t mutex_tag;
    void    *host_thread;
    void    *exit_jmp;
} guest_thread_t;

void kernel_init(uint64_t program_id);
guest_thread_t *kernel_create_main_thread(uint64_t entry, uint64_t stack_top, int priority, int core);
_Noreturn void  kernel_run_main_thread(guest_thread_t *t);
guest_thread_t *kernel_current_thread(void);
void kernel_svc(cpu_t *c, uint32_t imm);
uint32_t kernel_new_event(int signaled, void **out_event); // returns the readable handle
void     kernel_event_set(void *event, int signaled);
_Noreturn void kernel_exit(int code);

// ---- IPC ----------------------------------------------------------------------

uint32_t ipc_connect_named_port(const char *name, uint32_t *out_handle);
uint32_t ipc_send_sync_request(cpu_t *c, uint32_t handle, uint64_t msg_addr, uint64_t msg_size);
void     ipc_close(uint32_t handle);
bool     ipc_is_session(uint32_t handle);

#endif // RECOMP_HOST_H
