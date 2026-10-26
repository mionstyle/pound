// IPC: HIPC message parsing, CMIF (with domains) and TIPC, the service manager
// (sm:) and a generic service layer.
//
// Services are mostly stubs for now: a request to a command without a handler
// succeeds with zeroed output and is logged once, so the boot can progress and
// show which services / commands the title needs. Commands known to return an
// interface create a new object of that interface.

#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_OK             0u
#define R_INVALID_HANDLE 0xE401u
#define R_SF_UNKNOWN_CMD 0x25A0Au // sf: unknown command (used for explicit failures)

#define APPLET_RESOURCE_USER_ID 0x63ull // any non-zero id identifies the application to services

#define CMIF_IN_MAGIC  0x49434653u // "SFCI"
#define CMIF_OUT_MAGIC 0x4F434653u // "SFCO"

// ---- objects and sessions -----------------------------------------------------------

typedef struct
{
    char iface[48];
    int  in_use;
} object_t;

#define MAX_OBJECTS 1024
static object_t objects[MAX_OBJECTS];

static int object_new(const char *iface)
{
    for (int i = 1; i < MAX_OBJECTS; ++i)
        if (!objects[i].in_use) {
            objects[i].in_use = 1;
            snprintf(objects[i].iface, sizeof objects[i].iface, "%s", iface);
            return i;
        }
    host_fatal("too many IPC objects");
}

typedef struct
{
    uint32_t handle;
    int      is_sm;
    int      is_domain;
    int      object;          // non-domain: the object this session talks to
    int      domain[64];      // domain: object ids (index = domain object id)
} session_t;

#define MAX_SESSIONS 512
static session_t sessions[MAX_SESSIONS];
static uint32_t  next_session_handle = 0xA000;

static session_t *session_get(uint32_t h)
{
    if (h == 0)
        return NULL;
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (sessions[i].handle == h)
            return &sessions[i];
    return NULL;
}

static session_t *session_new(int object, int is_sm)
{
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (sessions[i].handle == 0) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            sessions[i].handle = next_session_handle++;
            sessions[i].is_sm  = is_sm;
            sessions[i].object = object;
            return &sessions[i];
        }
    host_fatal("too many sessions");
}

bool ipc_is_session(uint32_t h) { return session_get(h) != NULL; }

void ipc_close(uint32_t h)
{
    session_t *s = session_get(h);
    if (s)
        s->handle = 0;
}

uint32_t ipc_connect_named_port(const char *name, uint32_t *out_handle)
{
    host_log("ipc: ConnectToNamedPort(\"%s\")", name);
    if (strcmp(name, "sm:") != 0)
        host_fatal("named port '%s' not implemented", name);
    *out_handle = session_new(object_new("sm:"), 1)->handle;
    return R_OK;
}

// ---- message parsing ------------------------------------------------------------

typedef struct
{
    uint32_t type;
    uint32_t num_x, num_a, num_b, num_w;
    uint32_t num_data_words;
    int      has_special, send_pid;
    uint64_t pid;
    uint32_t num_copy, num_move;
    uint32_t copy[8], move[8];
    uint64_t data;
    uint64_t x_addr[4], x_size[4];
    uint64_t a_addr[4], a_size[4];
    uint64_t b_addr[4], b_size[4];
    uint32_t num_c;
    uint64_t c_addr[4], c_size[4]; // receive list (pointer buffers)
} hipc_msg_t;

static void parse_hipc(uint64_t msg, hipc_msg_t *m)
{
    memset(m, 0, sizeof *m);
    uint32_t w0 = rd32(msg), w1 = rd32(msg + 4);
    m->type           = w0 & 0xFFFF;
    m->num_x          = (w0 >> 16) & 0xF;
    m->num_a          = (w0 >> 20) & 0xF;
    m->num_b          = (w0 >> 24) & 0xF;
    m->num_w          = (w0 >> 28) & 0xF;
    m->num_data_words = w1 & 0x3FF;
    m->has_special    = (w1 >> 31) & 1;
    uint64_t p = msg + 8;
    if (m->has_special) {
        uint32_t sh = rd32(p);
        p += 4;
        m->send_pid = sh & 1;
        m->num_copy = (sh >> 1) & 0xF;
        m->num_move = (sh >> 5) & 0xF;
        if (m->send_pid) { m->pid = rd64(p); p += 8; }
        for (uint32_t i = 0; i < m->num_copy; ++i, p += 4) if (i < 8) m->copy[i] = rd32(p);
        for (uint32_t i = 0; i < m->num_move; ++i, p += 4) if (i < 8) m->move[i] = rd32(p);
    }
    for (uint32_t i = 0; i < m->num_x; ++i, p += 8) {
        uint32_t a = rd32(p), b = rd32(p + 4);
        m->x_size[i & 3] = a >> 16;
        m->x_addr[i & 3] = b | ((uint64_t)((a >> 12) & 0xF) << 32) | ((uint64_t)((a >> 6) & 0x7) << 36);
    }
    for (uint32_t n = 0; n < m->num_a + m->num_b + m->num_w; ++n, p += 12) {
        uint32_t s = rd32(p), a = rd32(p + 4), c = rd32(p + 8);
        uint64_t addr = a | ((uint64_t)((c >> 28) & 0xF) << 32) | ((uint64_t)((c >> 2) & 0x7) << 36);
        uint64_t size = s | ((uint64_t)((c >> 24) & 0xF) << 32);
        if (n < m->num_a) { m->a_addr[n & 3] = addr; m->a_size[n & 3] = size; }
        else if (n < m->num_a + m->num_b) { m->b_addr[(n - m->num_a) & 3] = addr; m->b_size[(n - m->num_a) & 3] = size; }
    }
    m->data = p;
    // Receive list (C descriptors) follows the raw data words.
    uint32_t mode = (w1 >> 10) & 0xF;
    m->num_c = mode == 2 ? 1 : mode > 2 ? mode - 2 : 0;
    uint64_t cp = p + 4ull * m->num_data_words;
    for (uint32_t i = 0; i < m->num_c && i < 4; ++i, cp += 8) {
        uint32_t lo = rd32(cp), hi = rd32(cp + 4);
        m->c_addr[i] = lo | ((uint64_t)(hi & 0xFFFF) << 32);
        m->c_size[i] = hi >> 16;
    }
}

// Output buffer i of an "auto select" parameter: the B buffer, or the C (pointer) buffer
// when the client chose the pointer path and left B empty.
static uint64_t out_buf(const hipc_msg_t *m, int i, uint64_t *size)
{
    if (i < (int)m->num_b && m->b_addr[i]) { *size = m->b_size[i]; return m->b_addr[i]; }
    if (i < (int)m->num_c && m->c_addr[i]) { *size = m->c_size[i]; return m->c_addr[i]; }
    if (i < (int)m->num_x) { *size = m->x_size[i]; return m->x_addr[i]; }
    *size = 0;
    return 0;
}

// ---- responses ------------------------------------------------------------------

typedef struct
{
    uint32_t result;
    uint8_t  data[0x100];
    uint32_t data_size;
    uint32_t move[8];
    uint32_t num_move;
    uint32_t copy[8];
    uint32_t num_copy;
    uint32_t out_objects[8]; // domain object ids
    uint32_t num_out_objects;
} reply_t;

static void write_cmif_reply(uint64_t msg, const reply_t *r, int domain)
{
    uint64_t p = msg;
    uint32_t payload = 16 + (domain ? 16 : 0) + r->data_size + 4 * r->num_out_objects;
    uint32_t data_words = 4 + (payload + 3) / 4;
    int special = r->num_move || r->num_copy;
    wr32(p, 0);
    wr32(p + 4, data_words | (special ? (1u << 31) : 0));
    p += 8;
    if (special) {
        wr32(p, r->num_copy << 1 | r->num_move << 5);
        p += 4;
        for (uint32_t i = 0; i < r->num_copy; ++i, p += 4) wr32(p, r->copy[i]);
        for (uint32_t i = 0; i < r->num_move; ++i, p += 4) wr32(p, r->move[i]);
    }
    p = (p + 15) & ~15ull;
    if (domain) {
        wr32(p, r->num_out_objects);
        wr32(p + 4, 0); wr32(p + 8, 0); wr32(p + 12, 0);
        p += 16;
    }
    wr32(p, CMIF_OUT_MAGIC);
    wr32(p + 4, 0);
    wr32(p + 8, r->result);
    wr32(p + 12, 0);
    g_write(p + 16, r->data, r->data_size);
    for (uint32_t i = 0; i < r->num_out_objects; ++i)
        wr32(p + 16 + r->data_size + 4 * i, r->out_objects[i]);
}

static void write_tipc_reply(uint64_t msg, const reply_t *r)
{
    uint64_t p = msg;
    int special = r->num_move || r->num_copy;
    wr32(p, 0);
    wr32(p + 4, (1 + (r->data_size + 3) / 4) | (special ? (1u << 31) : 0));
    p += 8;
    if (special) {
        wr32(p, r->num_copy << 1 | r->num_move << 5);
        p += 4;
        for (uint32_t i = 0; i < r->num_copy; ++i, p += 4) wr32(p, r->copy[i]);
        for (uint32_t i = 0; i < r->num_move; ++i, p += 4) wr32(p, r->move[i]);
    }
    wr32(p, r->result);
    g_write(p + 4, r->data, r->data_size);
}

// ---- requests -----------------------------------------------------------------

typedef struct
{
    session_t  *session;
    int         object;
    const char *iface;
    uint32_t    cmd;
    uint64_t    in;      // raw input data (after the CMIF header)
    uint32_t    in_size;
    hipc_msg_t *msg;
    int         domain;  // request arrived through a domain
} request_t;

static void reply_u32(reply_t *r, uint32_t v) { memcpy(r->data + r->data_size, &v, 4); r->data_size += 4; }
static void reply_u64(reply_t *r, uint64_t v) { r->data_size = (r->data_size + 7) & ~7u; memcpy(r->data + r->data_size, &v, 8); r->data_size += 8; }

// Returns a new object of `iface` to the client (as a handle, or a domain object id).
static void reply_object(request_t *q, reply_t *r, const char *iface)
{
    int obj = object_new(iface);
    if (q->domain) {
        session_t *s = q->session;
        for (int i = 1; i < 64; ++i)
            if (!s->domain[i]) {
                s->domain[i] = obj;
                r->out_objects[r->num_out_objects++] = (uint32_t)i;
                return;
            }
        host_fatal("domain full");
    }
    r->move[r->num_move++] = session_new(obj, 0)->handle;
}

// Commands known to return an interface: (interface, command, returned interface).
static const struct { const char *iface; uint32_t cmd; const char *ret; } k_returns[] = {
    { "lm", 0, "lm:ILogger" },
    { "fsp-srv", 18, "fsp:IFileSystem(sdcard)" },
    { "fsp-srv", 51, "fsp:IFileSystem(savedata)" },
    { "fsp-srv", 200, "fsp:IStorage(romfs)" },
    { "fsp-srv", 202, "fsp:IStorage(data)" },
    { "fsp-srv", 203, "fsp:IStorage(patch)" },
    { "fsp-srv", 1, "fsp:SetCurrentProcess" },
    { "appletOE", 0, "am:IApplicationProxy" },
    { "appletOE", 10, "am:IApplicationProxy" }, // Switch 2 variant (takes pid + extra data)
    { "am:IApplicationProxy", 0, "am:ICommonStateGetter" },
    { "am:IApplicationProxy", 1, "am:ISelfController" },
    { "am:IApplicationProxy", 2, "am:IWindowController" },
    { "am:IApplicationProxy", 3, "am:IAudioController" },
    { "am:IApplicationProxy", 4, "am:IDisplayController" },
    { "am:IApplicationProxy", 10, "am:IProcessWindingController" },
    { "am:IApplicationProxy", 11, "am:ILibraryAppletCreator" },
    { "am:IApplicationProxy", 20, "am:IApplicationFunctions" },
    { "am:IApplicationProxy", 1000, "am:IDebugFunctions" },
    // Switch 2 additions to the proxy (names guessed from the SDK getters that use them).
    { "am:IApplicationProxy", 40, "am:IApplicationProxy.40" },
    { "am:IApplicationProxy", 60, "am:IApplicationProxy.60" },
    { "hid", 0, "hid:IAppletResource" },
    { "time:u", 0, "time:ISystemClock(user)" },
    { "time:u", 1, "time:ISystemClock(network)" },
    { "time:u", 2, "time:ISteadyClock" },
    { "time:u", 3, "time:ITimeZoneService" },
    { "time:u", 4, "time:ISystemClock(local)" },
    { "pctl", 0, "pctl:IParentalControlService" },
    { "pctl", 1, "pctl:IParentalControlService" },
    { "pctl:s", 0, "pctl:IParentalControlService" },
    { "pctl:s", 1, "pctl:IParentalControlService" },
    { "pctl:a", 0, "pctl:IParentalControlService" },
    { "pctl:a", 1, "pctl:IParentalControlService" },
    { "pctl:r", 0, "pctl:IParentalControlService" },
    { "pctl:r", 1, "pctl:IParentalControlService" },
    { "nifm:u", 4, "nifm:IGeneralService" },  // CreateGeneralServiceOld
    { "nifm:u", 5, "nifm:IGeneralService" },  // CreateGeneralService
    { "bcat:u", 0, "bcat:IBcatService" },     // CreateBcatService
};

// Commands that fail with a fixed result, mirroring an offline console with no content.
static const struct { const char *iface; uint32_t cmd; uint32_t result; const char *why; } k_failures[] = {
    // CreateDeliveryCacheStorageService: no BCAT delivery cache was ever downloaded.
    { "bcat:u", 1, 0x67a, "no delivery cache" },
};

// Commands known to return a (copy) handle to an event: (interface, command, initially signalled).
static const struct { const char *iface; uint32_t cmd; int signaled; } k_events[] = {
    { "am:IApplicationFunctions", 130, 0 }, // GetGpuErrorDetectedSystemEvent
    { "am:ISelfController", 91, 0 },        // GetAccumulatedSuspendedTickChangedEvent
    { "am:ISelfController", 1, 0 },         // (Switch 1) LockExit-related event
    { "am:ICommonStateGetter", 61, 0 },     // GetDefaultDisplayResolutionChangeEvent
    { "am:IApplicationFunctions", 167, 0 }, // GetHealthWarningDisappearedSystemEvent
    { "am:IApplicationFunctions", 210, 0 }, // GetUnknownEvent / friend invitation
    { "aoc:u", 8, 0 },                      // GetAddOnContentListChangedEvent
    { "aoc:u", 10, 0 },                     // GetAddOnContentListChangedEventWithProcessId
    { "time:u", 20, 0 },                    // (placeholder: shared memory handled below)
    { "hid:IAppletResource", 0, 0 },
};

static uint32_t g_unknown_logged;
static __thread uint64_t g_current_msg;

static void handle_request(request_t *q, reply_t *r)
{
    // sm: is the only service with real behaviour so far.
    if (!strcmp(q->iface, "sm:")) {
        if (q->cmd == 0) return; // RegisterClient
        if (q->cmd == 1) {       // GetServiceHandle(name)
            char name[9] = {0};
            g_read(q->in, name, 8);
            int obj = object_new(name);
            r->move[r->num_move++] = session_new(obj, 0)->handle;
            host_log("ipc: sm:GetServiceHandle(\"%s\")", name);
            return;
        }
        host_fatal("sm: command %u not implemented", q->cmd);
    }
    if (!strcmp(q->iface, "lm:ILogger") && q->cmd == 0) {
        // Log(buffer): binary log packet; print the printable runs.
        hipc_msg_t *m = q->msg;
        uint64_t a = m->num_x ? m->x_addr[0] : m->a_addr[0];
        uint64_t n = m->num_x ? m->x_size[0] : m->a_size[0];
        char     text[512];
        size_t   k = 0;
        for (uint64_t i = 0; i < n && k + 1 < sizeof text; ++i) {
            char ch = (char)rd8(a + i);
            if (ch >= 0x20 && ch < 0x7F) text[k++] = ch;
            else if (k && text[k - 1] != ' ') text[k++] = ' ';
        }
        text[k] = 0;
        host_log("[lm] %s", text);
        return;
    }
    if (!strcmp(q->iface, "am:ICommonStateGetter")) {
        // Applet message queue: the application starts with FocusStateChanged pending.
        static void *msg_event;
        static int   pending = 1;
        switch (q->cmd) {
        case 0: // GetEventHandle -> copy handle
            if (!msg_event)
                r->copy[r->num_copy++] = kernel_new_event(pending, &msg_event);
            else
                r->copy[r->num_copy++] = kernel_new_event(pending, NULL);
            return;
        case 1: // ReceiveMessage -> u32
            if (!pending) {
                r->result = 0x680; // am: no message
                return;
            }
            pending = 0;
            if (msg_event)
                kernel_event_set(msg_event, 0);
            reply_u32(r, 15); // FocusStateChanged
            host_log("ipc: am: delivering applet message FocusStateChanged");
            return;
        case 5: reply_u32(r, 0); return;  // GetOperationMode: handheld
        case 6: reply_u32(r, 0); return;  // GetPerformanceMode: normal
        case 9: reply_u32(r, 1); return;  // GetCurrentFocusState: in focus
        default: break;
        }
    }
    if (!strncmp(q->iface, "fsp:IStorage(romfs)", 19)) {
        hipc_msg_t *m = q->msg;
        switch (q->cmd) {
        case 0: { // Read(offset, size) -> buffer
            uint64_t off = rd64(q->in), size = rd64(q->in + 8), cap = 0;
            uint64_t buf = out_buf(m, 0, &cap);
            if (size > cap)
                size = cap;
            romfs_read(off, mem_host(buf), size);
            if (host_verbose)
                host_log("ipc: romfs read 0x%llx +0x%llx", (unsigned long long)off, (unsigned long long)size);
            return;
        }
        case 4: reply_u64(r, romfs_size()); return; // GetSize
        default: break;
        }
    }
    if (!strncmp(q->iface, "acc:u", 5)) {
        static const uint64_t uid[2] = { 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
        switch (q->cmd) {
        case 0: reply_u32(r, 1); return;                          // GetUserCount
        case 1: reply_u32(r, 1); return;                          // GetUserExistence(uid) -> true
        case 2: case 3: {                                         // ListAllUsers / ListOpenUsers -> buffer
            uint64_t n = 0, dst = out_buf(q->msg, 0, &n);
            if (dst && n >= 16) g_write(dst, uid, 16);
            return;
        }
        case 4: reply_u64(r, uid[0]); reply_u64(r, uid[1]); return; // GetLastOpenedUser
        case 5: reply_object(q, r, "acc:IProfile"); return;        // GetProfile(uid)
        case 100: case 140: case 160: return;                      // InitializeApplicationInfo*
        case 101: reply_object(q, r, "acc:IManagerForApplication"); return;
        case 150: reply_u32(r, 0); return;                         // IsUserAccountSwitchLocked -> false
        default: break;
        }
    }
    if (!strcmp(q->iface, "acc:IProfile")) {
        if (q->cmd == 0 || q->cmd == 1) { // Get / GetBase -> ProfileBase {uid, timestamp, nickname[0x21]}
            uint8_t base[0x38] = {0};
            static const uint64_t uid[2] = { 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
            memcpy(base, uid, 16);
            memcpy(base + 0x18, "Player", 7);
            memcpy(r->data, base, sizeof base);
            r->data_size = sizeof base;
            return;
        }
        if (q->cmd == 10) { reply_u32(r, 0); return; } // GetImageSize -> 0
    }
    if (!strcmp(q->iface, "am:IApplicationFunctions")) {
        switch (q->cmd) {
        case 1: { // PopLaunchParameter(kind) -> IStorage
            uint32_t kind = rd32(q->in);
            static int popped;
            if (kind != 2 || popped++) { // only a preselected user is provided, once
                r->result = 0x480; // am: no data in channel
                return;
            }
            host_log("ipc: am: launch parameter: preselected user");
            reply_object(q, r, "am:IStorage(preselected-user)");
            return;
        }
        case 20: reply_u64(r, 0); return; // EnsureSaveData(uid) -> required size 0 (exists)
        case 21: // GetDesiredLanguage -> u64 language code
            reply_u64(r, 0x00000053552D6E65ull); // "en-US"
            return;
        case 23: { // GetDisplayVersion -> char[0x10]
            memcpy(r->data, "1.1.0", 6);
            r->data_size = 0x10;
            return;
        }
        default: break;
        }
    }
    if (!strncmp(q->iface, "am:IStorage", 11))
        host_log("ipc: %s cmd %u (in %u bytes, X%u A%u B%u, in[0]=0x%llx)", q->iface, q->cmd, q->in_size, q->msg->num_x,
                 q->msg->num_a, q->msg->num_b, (unsigned long long)(q->in_size >= 8 ? rd64(q->in) : 0));
    if (!strncmp(q->iface, "am:IStorage(", 12)) {
        if (q->cmd == 0) { reply_object(q, r, "am:IStorageAccessor(preselected-user)"); return; } // Open
    }
    if (!strncmp(q->iface, "am:IStorageAccessor(", 20)) {
        // Preselected user launch parameter: magic, flag, user id (0x88 bytes in total).
        uint8_t blob[0x88] = {0};
        uint32_t magic = 0xC79497CA, one = 1;
        uint64_t uid[2] = { 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
        memcpy(blob, &magic, 4);
        memcpy(blob + 4, &one, 4);
        memcpy(blob + 8, uid, 16);
        if (q->cmd == 0) { reply_u64(r, sizeof blob); return; } // GetSize
        if (q->cmd == 11) { // Read(offset) -> buffer
            uint64_t off = rd64(q->in), n = 0;
            uint64_t dst = out_buf(q->msg, 0, &n);
            if (off < sizeof blob) {
                uint64_t k = sizeof blob - off < n ? sizeof blob - off : n;
                g_write(dst, blob + off, k);
            }

            return;
        }
    }
    if (!strcmp(q->iface, "am:IApplicationProxy.60") && q->cmd == 10) {
        // Read by nn::oe::GetGraphicsScalingParameters. The first word is the scaling mode:
        // nn::oe::GetCurrentGraphicsScale maps 10/20/30 to scale 0/1/2 and aborts otherwise.
        static const uint32_t params[3] = { 10, 0, 0 };
        r->data_size = sizeof params;
        memcpy(r->data, params, sizeof params);
        return;
    }
    if (!strcmp(q->iface, "am:IWindowController") && q->cmd == 1) { // GetAppletResourceUserId
        reply_u64(r, APPLET_RESOURCE_USER_ID);
        return;
    }
    if (!strcmp(q->iface, "am:ISelfController") && q->cmd == 2000) {
        // Switch 2: nn::am::GetGraphicsSupervisor(Out<NativeHandle>) - a session handle to the
        // graphics supervisor, returned as a copy handle.
        r->copy[r->num_copy++] = session_new(object_new("am:IGraphicsSupervisor"), 0)->handle;
        host_log("ipc: am:ISelfController cmd 2000 (GetGraphicsSupervisor) -> session");
        return;
    }
    for (size_t i = 0; i < sizeof k_failures / sizeof k_failures[0]; ++i)
        if (!strcmp(k_failures[i].iface, q->iface) && k_failures[i].cmd == q->cmd) {
            host_log("ipc: %s cmd %u -> 0x%x (%s)", q->iface, q->cmd, k_failures[i].result, k_failures[i].why);
            r->result = k_failures[i].result;
            return;
        }
    for (size_t i = 0; i < sizeof k_events / sizeof k_events[0]; ++i)
        if (!strcmp(k_events[i].iface, q->iface) && k_events[i].cmd == q->cmd && strcmp(q->iface, "time:u")) {
            r->copy[r->num_copy++] = kernel_new_event(k_events[i].signaled, NULL);
            host_log("ipc: %s cmd %u -> new event", q->iface, q->cmd);
            return;
        }
    for (size_t i = 0; i < sizeof k_returns / sizeof k_returns[0]; ++i)
        if (!strcmp(k_returns[i].iface, q->iface) && k_returns[i].cmd == q->cmd) {
            host_log("ipc: %s cmd %u -> new %s", q->iface, q->cmd, k_returns[i].ret);
            reply_object(q, r, k_returns[i].ret);
            return;
        }
    ++g_unknown_logged;
    char args[64] = "";
    if (q->in_size >= 4) {
        uint32_t n = q->in_size / 4 > 4 ? 4 : q->in_size / 4;
        int      k = 0;
        for (uint32_t i = 0; i < n; ++i)
            k += snprintf(args + k, sizeof args - k, " %08x", rd32(q->in + 4 * i));
    }
    host_log("ipc: STUB %s cmd %u (in %u bytes:%s, X%u A%u B%u%s) -> success, zero output", q->iface, q->cmd,
             q->in_size, args, q->msg->num_x, q->msg->num_a, q->msg->num_b, q->msg->send_pid ? ", pid" : "");
    // Generous zero output: callers read at most what they expect.
    r->data_size = 0x40;
    memset(r->data, 0, r->data_size);
}

static uint32_t handle_control(session_t *s, uint64_t msg, hipc_msg_t *m, uint32_t cmd)
{
    reply_t r;
    memset(&r, 0, sizeof r);
    switch (cmd) {
    case 0: // ConvertCurrentObjectToDomain -> object id
        s->is_domain = 1;
        s->domain[1] = s->object;
        reply_u32(&r, 1);
        break;
    case 2: // CloneCurrentObject -> handle
    case 4: // CloneCurrentObjectEx
        r.move[r.num_move++] = session_new(s->object, s->is_sm)->handle;
        if (s->is_domain) {
            session_t *n = session_get(r.move[0]);
            n->is_domain = 1;
            memcpy(n->domain, s->domain, sizeof n->domain);
        }
        break;
    case 3: // QueryPointerBufferSize -> u16
        reply_u32(&r, 0x8000);
        break;
    default:
        host_fatal("ipc: control command %u not implemented", cmd);
    }
    (void)m;
    write_cmif_reply(msg, &r, 0);
    return R_OK;
}

uint32_t ipc_send_sync_request(cpu_t *c, uint32_t handle, uint64_t msg, uint64_t msg_size)
{
    g_current_msg = msg;
    (void)c; (void)msg_size;
    session_t *s = session_get(handle);
    if (!s)
        return R_INVALID_HANDLE;
    hipc_msg_t m;
    parse_hipc(msg, &m);

    reply_t r;
    memset(&r, 0, sizeof r);
    request_t q = { .session = s, .msg = &m };

    if (m.type >= 16) { // TIPC: command = type - 16, raw data directly after the descriptors
        q.object = s->object;
        q.iface  = objects[s->object].iface;
        q.cmd    = m.type - 16;
        q.in     = m.data;
        q.in_size = m.num_data_words * 4;
        handle_request(&q, &r);
        write_tipc_reply(msg, &r);
        return R_OK;
    }
    if (m.type == 2) { // Close
        ipc_close(handle);
        return R_OK;
    }
    uint64_t h = (m.data + 15) & ~15ull;
    if (m.type == 5 || m.type == 7) {
        if (rd32(h) != CMIF_IN_MAGIC)
            host_fatal("ipc: control message without SFCI");
        return handle_control(s, msg, &m, rd32(h + 8));
    }
    if (m.type != 4 && m.type != 6)
        host_fatal("ipc: message type %u not implemented", m.type);

    if (s->is_domain) {
        uint8_t  dtype = rd8(h);
        uint16_t dsize = (uint16_t)(rd8(h + 2) | rd8(h + 3) << 8);
        uint32_t oid   = rd32(h + 4);
        if (oid >= 64 || !s->domain[oid])
            host_fatal("ipc: bad domain object id %u", oid);
        if (dtype == 2) { // close object
            s->domain[oid] = 0;
            write_cmif_reply(msg, &r, 1);
            return R_OK;
        }
        q.object = s->domain[oid];
        q.domain = 1;
        h += 16;
        q.in_size = dsize >= 16 ? dsize - 16u : 0;
    } else {
        q.object  = s->object;
        q.in_size = m.num_data_words * 4 > 32 ? m.num_data_words * 4 - 32 : 0;
    }
    if (rd32(h) != CMIF_IN_MAGIC)
        host_fatal("ipc: request without SFCI (%s)", objects[q.object].iface);
    q.iface = objects[q.object].iface;
    q.cmd   = rd32(h + 8);
    q.in    = h + 16;
    handle_request(&q, &r);
    write_cmif_reply(msg, &r, q.domain);
    return R_OK;
}
