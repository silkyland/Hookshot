/* Horizon supervisor calls that lifted SDK code still makes (the game itself has none; the SDK has ~60 wrappers).
   The HLE above nn:: should keep most of them from ever running: what reaches here is either trivial
   (debug output, ticks, info queries) or a sign that an SDK function above it wants an HLE. Unknown ones stop the
   run with the svc number and the caller, so the log says what to do next. Numbers: switchbrew.org "SVC". */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define R_OK 0
/* (mkw-recomp) the region of GetInfo 29/30: 128 GB at 192 GB, between the heap (up from 64 GB) and the address
   regions (down from 512 GB); the SDK's VAMM backs pieces of it with MapPhysicalMemory */
/* (android) the window can be smaller than the desktop's 512 GB, so the region floats to the window's top
   (hos_mem.c reserves it there) */
#define HOS_NX2_REGION_BASE (hos_nx2_region(NULL))
#define HOS_NX2_REGION_SIZE (hos_nx2_region_size())
uint64_t hos_nx2_region(uint64_t *size);
uint64_t hos_nx2_region_size(void);
/* the game's VA budget is the mapped window, not the compile-time ideal (svcGetInfo answers must stay honest) */
uint64_t hos_guest_size(void);
static uint64_t space_end(void) { return hos_guest_size() < HOS_SPACE_END ? hos_guest_size() : HOS_SPACE_END; }
#define R_INVALID_ENUM 0xF001   /* svc::ResultInvalidEnumValue */

/* (pokopia) named-port sessions and their service names: sm: needs real answers (GetServiceHandle hands out
   sessions), every other service gets the empty-success reply below. */
#define HOS_IPC_MAX 64
static struct { uint32_t h; char name[9]; int dom; } hos_ipc_sessions[HOS_IPC_MAX];
static volatile LONG hos_ipc_next;
static void ipc_session_add(uint32_t h, const char *name) {
    for (int i = 0; i < HOS_IPC_MAX; i++)
        if (!hos_ipc_sessions[i].h) {
            hos_ipc_sessions[i].h = h;
            hos_ipc_sessions[i].dom = 0;
            snprintf(hos_ipc_sessions[i].name, sizeof hos_ipc_sessions[i].name, "%.8s", name ? name : "");
            return;
        }
}
static const char *ipc_session_name(uint32_t h) {
    for (int i = 0; i < HOS_IPC_MAX; i++)
        if (hos_ipc_sessions[i].h == h) return hos_ipc_sessions[i].name;
    return NULL;
}
static int ipc_session_dom(uint32_t h) {
    for (int i = 0; i < HOS_IPC_MAX; i++)
        if (hos_ipc_sessions[i].h == h) return hos_ipc_sessions[i].dom;
    return 0;
}
static void ipc_session_set_dom(uint32_t h, int d) {
    for (int i = 0; i < HOS_IPC_MAX; i++)
        if (hos_ipc_sessions[i].h == h) { hos_ipc_sessions[i].dom = d; return; }
}
static uint32_t ipc_new_session(const char *name) {
    uint32_t h = 0x7E000000u | (uint32_t)InterlockedIncrement(&hos_ipc_next);
    ipc_session_add(h, name);
    return h;
}
/* (pokopia) CMIF domains (SDK 20 HipcServiceResolution): control methods (msg types 5/7, raw = SFCI)
   convert a session to a domain; domain requests (types 1/3/4/6 on a domain session) carry
   {kind u8, n_obj u8, in_raw u16, target u32, reserved..} then SFCI, and returned objects are domain
   ids, not move handles (pokopia-recomp runtime/src/ipc/ipc.cpp). */
typedef struct { uint32_t id; char svc[9]; } IpcDomObj;
typedef struct { int used; uint32_t owner; IpcDomObj objs[24]; uint32_t next; } IpcDomain;
static IpcDomain ipc_domains[8];
static IpcDomain *ipc_domain_new(uint32_t owner) {
    for (int i = 0; i < 8; i++)
        if (!ipc_domains[i].used) {
            memset(&ipc_domains[i], 0, sizeof ipc_domains[i]);
            ipc_domains[i].used = 1; ipc_domains[i].owner = owner; ipc_domains[i].next = 2;
            return &ipc_domains[i];
        }
    return NULL;
}
static IpcDomain *ipc_domain_get(int idx) { return idx > 0 && idx <= 8 ? &ipc_domains[idx - 1] : NULL; }
static IpcDomObj *ipc_domain_obj(IpcDomain *d, uint32_t id) {
    if (!d) return NULL;
    for (int i = 0; i < 24; i++)
        if (d->objs[i].id == id) return &d->objs[i];
    return NULL;
}
static uint32_t ipc_domain_add(IpcDomain *d, const char *svc) {
    for (int i = 0; i < 24; i++)
        if (!d->objs[i].id) {
            d->objs[i].id = d->next++;
            snprintf(d->objs[i].svc, sizeof d->objs[i].svc, "%.8s", svc);
            return d->objs[i].id;
        }
    return 0;
}

/* (pokopia) named services answered for real. The SDK's HipcServiceResolution registers a client and asks sm:
   GetServiceHandle for each service name, and aborts (svcBreak) when the reply carries no session. The SDK talks
   TIPC to sm: (message type >= 16: the command is type - 16 and the raw data is the payload, no SFCI header, no
   16-byte alignment — pokopia-recomp runtime/src/ipc/ipc.cpp); other services get CMIF requests (types 1/3/4/6)
   with {"SFCI", version, command, token} 16-byte aligned. Replies: TIPC raw = {result} + outputs, objects as move
   handles; CMIF raw = {"SFCO", version, result, 0} + outputs. sm: commands: 0 RegisterClient, 1 GetServiceHandle
   (8-byte name -> a move handle to the new session), 2/3 Register/UnregisterService, 4 DetachClient. stpl:u is
   GameShare (nn::streamplay): cmd 0 CreateUserService returns the IUserService object as a session; its commands
   answer "nothing to share" per pokopia-recomp runtime/docs/IPC_ANSWERS.md — the SDK aborts on failures of
   GetRequiredMemorySize(1), GetWorkBufferSizes(9) and GetHidSharedMemory(300), so those must succeed. */
#define HOS_IPC_TIP_BUF 0x100
/* the raw data's offset in the message: after the two header words, the special header's handles and the
   X/A/B/W descriptors (u32 words); TIPC data is not aligned, CMIF data is */
static size_t ipc_raw_off(const uint8_t *tls, int align16) {
    const uint32_t *m = (const uint32_t *)tls;
    const unsigned nx = (m[0] >> 16) & 15;
    const unsigned nabw = ((m[0] >> 20) & 15) + ((m[0] >> 24) & 15) + ((m[0] >> 28) & 15);
    const uint32_t *p = m + 2;
    if (m[1] >> 31) {
        const uint32_t sh = *p++;
        p += (sh & 1 ? 2 : 0) + (sh >> 1 & 15) + (sh >> 5 & 15);
    }
    p += nx * 2 + nabw * 3;
    const size_t off = (size_t)((const uint8_t *)p - tls);
    return align16 ? (off + 15) & ~(size_t)15 : off;
}
/* CMIF reply: SFCO at +16, outputs after it; `move` a session handle moved to the client. TIPC reply: {result}
   then outputs, objects as move handles. outn bytes of outputs (zero pad to 8). */
static void ipc_write_reply(uint8_t *tls, uint32_t ver, uint32_t res, const void *out, uint32_t outn,
                            uint32_t copy, uint32_t move, int tipc) {
    memset(tls, 0, HOS_IPC_TIP_BUF);
    uint32_t *m = (uint32_t *)tls;
    const uint32_t nwords = (24 + ((outn + 7) & ~7u)) / 4;   /* header to outputs end, in u32s from byte 8 */
    const uint32_t nh = (copy != 0) + (move != 0);
    if (tipc) {
        m[0] = 16;                                       /* reply type: TIPC base, no descriptors */
        if (nh) {
            m[1] = (1u << 31) | nh;
            m[2] = (copy ? 2u : 0) | (move ? 0x20u : 0);   /* special header: copy count << 1, move count << 5 */
            uint32_t *hp = &m[3];
            if (copy) *hp++ = copy;
            if (move) *hp = move;
            *(uint32_t *)(tls + 16) = res;
            if (outn) memcpy(tls + 20, out, outn);
        } else {
            m[1] = 1 + ((outn + 3) & ~3u) / 4;
            *(uint32_t *)(tls + 8) = res;
            if (outn) memcpy(tls + 12, out, outn);
        }
        return;
    }
    if (nh) {
        m[1] = (1u << 31) | (((HOS_IPC_TIP_BUF - 16) / 4) < 0x3ff ? (HOS_IPC_TIP_BUF - 16) / 4 : 0x3ff);
        m[2] = (copy ? 2u : 0) | (move ? 0x20u : 0);       /* special header: copy count << 1, move count << 5 */
        uint32_t *hp = &m[3];
        if (copy) *hp++ = copy;
        if (move) *hp = move;
    } else {
        m[1] = nwords < 16 ? 16 : (nwords < 0x3ff ? nwords : 0x3ff);
    }
    uint32_t *o = (uint32_t *)(tls + 16);
    o[0] = 0x4F434653u; o[1] = ver; o[2] = res; o[3] = 0;
    if (outn) memcpy(tls + 32, out, outn);
}
/* domain reply: {u32 object count, 12 reserved} + SFCO + outputs + object ids (ids stay in the domain);
   copy/move handles still travel in the HIPC special header like a flat CMIF reply */
static void ipc_write_domain(uint8_t *tls, uint32_t res, const void *out, uint32_t outn,
                             const uint32_t *ids, int nids, uint32_t copy, uint32_t move) {
    memset(tls, 0, HOS_IPC_TIP_BUF);
    uint32_t *m = (uint32_t *)tls;
    const uint32_t nh = (copy != 0) + (move != 0);
    if (nh) {
        m[1] = (1u << 31) | (((HOS_IPC_TIP_BUF - 16) / 4) < 0x3ff ? (HOS_IPC_TIP_BUF - 16) / 4 : 0x3ff);
        m[2] = (copy ? 2u : 0) | (move ? 0x20u : 0);
        uint32_t *hp = &m[3];
        if (copy) *hp++ = copy;
        if (move) *hp = move;
    } else {
        m[1] = ((HOS_IPC_TIP_BUF - 16) / 4) < 0x3ff ? (HOS_IPC_TIP_BUF - 16) / 4 : 0x3ff;
    }
    const size_t raw_at = nh ? ((8 + 4 + 4 * nh + 15) & ~(size_t)15) : 16;
    uint32_t *d = (uint32_t *)(tls + raw_at);
    d[0] = (uint32_t)nids;
    uint32_t *o = d + 4;                                     /* SFCO */
    o[0] = 0x4F434653u; o[1] = 0; o[2] = res; o[3] = 0;
    if (outn) memcpy((uint8_t *)(o + 4), out, outn);
    if (nids) memcpy((uint8_t *)(o + 4) + ((outn + 7) & ~7u), ids, nids * 4);
}
static uint32_t ipc_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t ipc_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Kernel objects IPC hands out as copy handles (Switch 2 svc ids, per pokopia-recomp svc_memory.cpp): shared
   memory blocks the client maps with svcMapSharedMemory (0x13) / unmaps with 0x14. The buffer lives host-side;
   mapping copies it into the guest-chosen address and unmapping copies back. */
typedef struct { uint32_t h; uint64_t size; uint8_t *buf; uint64_t map_addr, map_size; } IpcShm;
static IpcShm ipc_shms[16];
static IpcShm *ipc_shm(uint32_t h) {
    for (int i = 0; i < 16; i++)
        if (ipc_shms[i].h == h) return &ipc_shms[i];
    return NULL;
}
static uint32_t ipc_new_shm(uint8_t *buf, uint64_t size) {
    for (int i = 0; i < 16; i++)
        if (!ipc_shms[i].h) {
            ipc_shms[i].h = 0x7D000000u | (uint32_t)InterlockedIncrement(&hos_ipc_next);
            ipc_shms[i].size = size;
            ipc_shms[i].buf = buf;
            return ipc_shms[i].h;
        }
    return 0;
}
/* (pokopia) readable event handles: ICommonStateGetter::GetEventHandle hands out a copy handle the SDK attaches
   to a SystemEvent (nn::os::AttachReadableHandleToSystemEvent); it is "readable" while the applet-message queue
   is non-empty (pokopia-recomp am.cpp's msg_event). One applet per process: one queue. */
int hos_am_msgs = 1;                                            /* fresh_state() queues FocusStateChanged (15) */
static uint32_t ipc_new_event(void) { return 0x7C000000u | (uint32_t)InterlockedIncrement(&hos_ipc_next); }
/* nn_os_sync.c asks whether an attached handle is readable right now */
int ipc_handle_readable(uint32_t h) {
    return (h & 0xFF000000u) == 0x7C000000u && hos_am_msgs > 0;
}
static void ipc_sm_handle(uint8_t *tls, unsigned cmd, const uint8_t *args, int tipc, uint32_t ver) {
    switch (cmd) {
    case 0: case 4: ipc_write_reply(tls, ver, 0, NULL, 0, 0, 0, tipc); return;   /* RegisterClient / DetachClient */
    case 1: {                                                  /* GetServiceHandle: 8-byte name */
        char name[9] = {};
        memcpy(name, args, 8);
        if (!name[0]) { ipc_write_reply(tls, ver, 0xC15u, NULL, 0, 0, 0, tipc); return; }
        const uint32_t h = ipc_new_session(name);
        hos_log("ipc: sm: GetServiceHandle(\"%s\") -> session %#x", name, h);
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, tipc);
        return;
    }
    case 2: case 3:                                             /* Register/UnregisterService */
        hos_log("ipc: sm: command %u not served (guest services unsupported)", cmd);
        ipc_write_reply(tls, ver, 0xE15u, NULL, 0, 0, 0, tipc);    /* sm ResultNotRegistered */
        return;
    default:
        hos_log("ipc: sm: unknown command %u", cmd);
        ipc_write_reply(tls, ver, 0xF601u, NULL, 0, 0, 0, tipc);
        return;
    }
}
/* stpl:u GameShare state: what Initialize was given, answered back by 502 / 503 */
static uint8_t stpl_type, stpl_mode;
static uint32_t stpl_required_memory(uint8_t type, uint32_t guests) {
    if (guests > 3) guests = 3;
    return type == 1 ? guests * 0x1000u : 0x1000u + guests * 3 * 0x1000u;
}
static void ipc_stpl_user(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver) {
    if (cmd == 0) {                                             /* CreateUserService -> the IUserService object */
        const uint32_t h = ipc_new_session("stpl:u/iu");
        hos_log("ipc: stpl:u CreateUserService -> session %#x", h);
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, 0);
        return;
    }
    ipc_write_reply(tls, ver, 0xF601u, NULL, 0, 0, 0, 0);
}
static void ipc_stpl_iu(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver) {
    uint8_t out[0x90];
    memset(out, 0, sizeof out);
    uint32_t res = 0;
    switch (cmd) {
    case 1: {                                                  /* GetRequiredMemorySize(type, mode, guests) */
        const uint32_t v = stpl_required_memory(args[0], ipc_u32(args + 4));
        memcpy(out, &v, 4);
        ipc_write_reply(tls, ver, 0, out, 4, 0, 0, 0);
        return;
    }
    case 9:                                                     /* GetWorkBufferSizes -> 4 x u64 one page */
        for (int i = 0; i < 4; i++) { const uint64_t pg = 0x1000; memcpy(out + 8 * i, &pg, 8); }
        ipc_write_reply(tls, ver, 0, out, 32, 0, 0, 0);
        return;
    case 2:                                                     /* Initialize(type, mode, guests, pid) */
        stpl_type = args[0]; stpl_mode = args[1];
        hos_log("ipc: stpl:u Initialize(type %u, mode %u, %u guests): accepted, no guest will connect",
                stpl_type, stpl_mode, ipc_u32(args + 4));
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, 0, 0);
        return;
    case 3: case 5: case 25: case 27: case 505: case 506: case 507: case 509: case 510:
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, 0, 0);            /* Finalize / CloseSession / setters / buffers */
        return;
    case 8:                                                     /* GetConnectionState -> not connected */
        ipc_write_reply(tls, ver, 0, out, 8, 0, 0, 0);
        return;
    case 502: out[0] = stpl_type; ipc_write_reply(tls, ver, 0, out, 1, 0, 0, 0); return;
    case 503: out[0] = stpl_mode; ipc_write_reply(tls, ver, 0, out, 1, 0, 0, 0); return;
    case 504: ipc_write_reply(tls, ver, 0, out, 0x80, 0, 0, 0); return;   /* MatchmakeConfig -> 0x80 zeros */
    case 300: {                                                 /* GetHidSharedMemory -> {size, offset} + copy handle */
        static uint8_t hid_shm[0x84000];                        /* zeroed: no guest pad, empty rings */
        const uint64_t sz = sizeof hid_shm;
        memcpy(out, &sz, 8);                                    /* offset stays 0 */
        const uint32_t h = ipc_new_shm(hid_shm, sz);
        ipc_write_reply(tls, ver, h ? 0 : 0xF601u, out, 16, h, 0, 0);
        return;
    }
    case 7:                                                     /* GetConnectionStateChangedEvent: never fires */
        hos_log("ipc: stpl:u cmd 7 (event) needs a kernel object the runtime does not vend");
        ipc_write_reply(tls, ver, 0xF601u, NULL, 0, 0, 0, 0);
        return;
    default:
        hos_log("ipc: stpl:u unknown command %u", cmd);
        ipc_write_reply(tls, ver, 0xF601u, NULL, 0, 0, 0, 0);
        return;
    }
    (void)res;
}
static void am_reply(uint8_t *tls, uint32_t ver, uint32_t res, const void *out, uint32_t outn,
                     uint32_t copy, uint32_t move, int tipc, IpcDomain *dom, uint32_t domid);
/* appletOE / appletAE (nn::am, SDK 20): IApplicationProxyService opens the one application's
   IApplicationProxy; its Get* commands vend sub-interfaces as move-handle sessions. Answers follow
   pokopia-recomp runtime/src/services/am.cpp: launch parameters report "no data" (0x428 = res(128,2)),
   save-size queries answer zero (fresh save), language "en-US", display version "2.0.1", and the
   graphics-scaling functions answer level 10 (full quality). */
static void ipc_am_svc(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc, int ae,
                       IpcDomain *dom) {
    (void)args;
    if ((!ae && (cmd == 0 || cmd == 10)) || (ae && cmd == 350)) {   /* OpenApplicationProxy / 2 / OpenSystemApplicationProxy */
        if (dom) {
            const uint32_t id = ipc_domain_add(dom, "am:proxy");
            hos_log("ipc: %s cmd %u -> IApplicationProxy dom-id %u", ae ? "appletAE" : "appletOE", cmd, id);
            ipc_write_domain(tls, id ? 0 : 0xF601u, NULL, 0, id ? &id : NULL, id ? 1 : 0, 0, 0);
            return;
        }
        const uint32_t h = ipc_new_session("am:proxy");
        hos_log("ipc: %s cmd %u -> IApplicationProxy %#x", ae ? "appletAE" : "appletOE", cmd, h);
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, tipc);
        return;
    }
    am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0);
}
static void ipc_am_proxy(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                         IpcDomain *dom) {
    (void)args;
    static const struct { unsigned cmd; const char *sub; } objs[] = {
        {0, "am:csg"}, {1, "am:sub"}, {2, "am:wc"}, {3, "am:sub"}, {4, "am:sub"}, {10, "am:sub"},
        {11, "am:sub"}, {20, "am:af"}, {40, "am:sub"}, {60, "am:gsp"}, {1000, "am:sub"}};
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++)
        if (objs[i].cmd == cmd) {
            if (dom) {
                const uint32_t id = ipc_domain_add(dom, objs[i].sub);
                ipc_write_domain(tls, 0, NULL, 0, id ? &id : NULL, id ? 1 : 0, 0, 0);
                return;
            }
            const uint32_t h = ipc_new_session(objs[i].sub);
            ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, tipc);
            return;
        }
    am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0);
}
static void ipc_am_af(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                      IpcDomain *dom) {
    uint8_t out[32];
    memset(out, 0, sizeof out);
    switch (cmd) {
    case 1:                                                     /* PopLaunchParameter -> ResultNoDataInChannel */
        am_reply(tls, ver, 0x428, NULL, 0, 0, 0, tipc, dom, 0); return;
    case 20: case 25: case 26:                                  /* EnsureSaveData / ExtendSaveData / GetSaveDataSize: zeros */
        am_reply(tls, ver, 0, out, cmd == 26 ? 16 : 8, 0, 0, tipc, dom, 0); return;
    case 21: {                                                  /* GetDesiredLanguage -> "en-US" packed u64 */
        const uint64_t v = 0x53552D6E65ULL;
        memcpy(out, &v, 8);
        am_reply(tls, ver, 0, out, 8, 0, 0, tipc, dom, 0); return; }
    case 23: memcpy(out, "2.0.1", 6);                           /* GetDisplayVersion -> 16-byte string */
        am_reply(tls, ver, 0, out, 16, 0, 0, tipc, dom, 0); return;
    case 27: case 28:                                           /* CreateCacheStorage / GetSaveDataSizeMax: 16 zero bytes */
        am_reply(tls, ver, 0, out, 16, 0, 0, tipc, dom, 0); return;
    case 320: am_reply(tls, ver, 0, out, 2, 0, 0, tipc, dom, 0); return;
    case 321: am_reply(tls, ver, 0, out, 1, 0, 0, tipc, dom, 0); return;
    default:
        am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0); return;
    }
    (void)args;
}
static void ipc_am_wc(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                      IpcDomain *dom) {
    (void)args;
    if (cmd == 1) {                                             /* GetAppletResourceUserId -> 0x51 */
        uint8_t out[8]; const uint64_t v = 0x51; memcpy(out, &v, 8);
        am_reply(tls, ver, 0, out, 8, 0, 0, tipc, dom, 0); return;
    }
    am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0);
}
static void ipc_am_gsp(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                       IpcDomain *dom) {
    (void)args;
    if (cmd == 10) {                                            /* GetGraphicsScalingParameters -> level 10, rest 0 */
        uint8_t out[16] = {10, 0, 0, 0};
        am_reply(tls, ver, 0, out, 16, 0, 0, tipc, dom, 0); return;
    }
    am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0);
}
/* ICommonStateGetter (proxy cmd 0): the applet-message queue. GetEventHandle vends a readable copy handle;
   ReceiveMessage pops AppletMessage 15 (FocusStateChanged) once, then ResultNoMessages 0x680. */
static void ipc_am_csg(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                       IpcDomain *dom) {
    (void)args;
    uint8_t out[16];
    memset(out, 0, sizeof out);
    switch (cmd) {
    case 0: {                                                   /* GetEventHandle -> copy handle, readable while msgs */
        const uint32_t h = ipc_new_event();
        if (dom) { ipc_write_domain(tls, 0, NULL, 0, NULL, 0, h, 0); return; }
        ipc_write_reply(tls, ver, 0, NULL, 0, h, 0, tipc); return; }
    case 1:                                                     /* ReceiveMessage */
        if (hos_am_msgs > 0) { hos_am_msgs--; out[0] = 15;      /* kMsgFocusStateChanged */
            if (dom) { ipc_write_domain(tls, 0, out, 4, NULL, 0, 0, 0); return; }
            ipc_write_reply(tls, ver, 0, out, 4, 0, 0, tipc); return; }
        if (dom) { ipc_write_domain(tls, 0x680, NULL, 0, NULL, 0, 0, 0); return; }
        ipc_write_reply(tls, ver, 0x680, NULL, 0, 0, 0, tipc); return;
    case 5: out[0] = 1;                                         /* GetOperationMode -> Console (docked) */
        if (dom) { ipc_write_domain(tls, 0, out, 1, NULL, 0, 0, 0); return; }
        ipc_write_reply(tls, ver, 0, out, 1, 0, 0, tipc); return;
    case 6: { const uint32_t v = 1;                             /* GetPerformanceMode -> Boost */
        if (dom) { ipc_write_domain(tls, 0, &v, 4, NULL, 0, 0, 0); return; }
        ipc_write_reply(tls, ver, 0, &v, 4, 0, 0, tipc); return; }
    case 9: out[0] = 1;                                         /* GetCurrentFocusState -> InFocus */
        if (dom) { ipc_write_domain(tls, 0, out, 1, NULL, 0, 0, 0); return; }
        ipc_write_reply(tls, ver, 0, out, 1, 0, 0, tipc); return;
    case 13: {                                                  /* GetAcquiredSleepLockEvent -> copy handle */
        const uint32_t h = ipc_new_event();
        if (dom) { ipc_write_domain(tls, 0, NULL, 0, NULL, 0, h, 0); return; }
        ipc_write_reply(tls, ver, 0, NULL, 0, h, 0, tipc); return; }
    default:
        am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0); return;
    }
}
/* any other am sub-interface: empty success */
static void ipc_am_sub(uint8_t *tls, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                       IpcDomain *dom) {
    (void)cmd; (void)args;
    am_reply(tls, ver, 0, NULL, 0, 0, 0, tipc, dom, 0);
}
/* am replies in domain mode carry object ids, not move handles */
static void am_reply(uint8_t *tls, uint32_t ver, uint32_t res, const void *out, uint32_t outn,
                     uint32_t copy, uint32_t move, int tipc, IpcDomain *dom, uint32_t domid) {
    if (dom) { ipc_write_domain(tls, res, out, outn, domid ? &domid : NULL, domid ? 1 : 0, 0, 0); return; }
    ipc_write_reply(tls, ver, res, out, outn, copy, move, tipc);
}
static void am_obj_reply(uint8_t *tls, uint32_t ver, IpcDomain *dom, const char *svc, int tipc) {
    if (dom) {
        const uint32_t id = ipc_domain_add(dom, svc);
        ipc_write_domain(tls, id ? 0 : 0xF601u, NULL, 0, id ? &id : NULL, id ? 1 : 0, 0, 0);
        return;
    }
    const uint32_t h = ipc_new_session(svc);
    ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, tipc);
}
static int ipc_am_named(uint8_t *tls, const char *name, unsigned cmd, const uint8_t *args, uint32_t ver, int tipc,
                        IpcDomain *dom) {
    if (!strcmp(name, "appletOE")) { ipc_am_svc(tls, cmd, args, ver, tipc, 0, dom); return 1; }
    if (!strcmp(name, "appletAE")) { ipc_am_svc(tls, cmd, args, ver, tipc, 1, dom); return 1; }
    if (!strcmp(name, "am:proxy")) { ipc_am_proxy(tls, cmd, args, ver, tipc, dom); return 1; }
    if (!strcmp(name, "am:csg")) { ipc_am_csg(tls, cmd, args, ver, tipc, dom); return 1; }
    if (!strcmp(name, "am:af")) { ipc_am_af(tls, cmd, args, ver, tipc, dom); return 1; }
    if (!strcmp(name, "am:wc")) { ipc_am_wc(tls, cmd, args, ver, tipc, dom); return 1; }
    if (!strcmp(name, "am:gsp")) { ipc_am_gsp(tls, cmd, args, ver, tipc, dom); return 1; }
    if (!strcmp(name, "am:sub")) { ipc_am_sub(tls, cmd, args, ver, tipc, dom); return 1; }
    return 0;
}
/* a CMIF request's SFCI command and arguments, or -1 when the message is not a plain request */
static int ipc_cmif_cmd(const uint8_t *tls, uint32_t *ver, const uint8_t **args) {
    const uint32_t *m = (const uint32_t *)tls;
    const unsigned type = m[0] & 0xffff;
    if (type == 2) return -2;                                  /* HIPC Close */
    const size_t cm = ipc_raw_off(tls, 1);
    if (cm + 32 > HOS_IPC_TIP_BUF) return -1;
    if (*(const uint32_t *)(tls + cm) != 0x49434653u) return -1;
    *ver = *(const uint32_t *)(tls + cm + 4);
    *args = tls + cm + 16;
    return (int)*(const uint32_t *)(tls + cm + 8);
}
/* CMIF control messages (types 5/7): raw starts with SFCI; method = cmd@+8 (pokopia-recomp ipc.cpp
   control()): 0 ConvertCurrentObjectToDomain (service becomes object id 1), 1 CopyFromCurrentDomain
   (object id arg -> move handle), 2/4 CloneCurrentObject[Ex], 3 QueryPointerBufferSize (0x8000). */
static void ipc_control(uint8_t *tls, const char *name, uint32_t handle) {
    const size_t off = ipc_raw_off(tls, 1);
    if (off + 32 > HOS_IPC_TIP_BUF || *(const uint32_t *)(tls + off) != 0x49434653u) {
        ipc_write_reply(tls, 0, 0xE401u, NULL, 0, 0, 0, 0);
        return;
    }
    const uint32_t ver = *(const uint32_t *)(tls + off + 4);
    const unsigned method = *(const uint32_t *)(tls + off + 8);
    const uint8_t *args = tls + off + 16;
    IpcDomain *dom = ipc_domain_get(ipc_session_dom(handle));
    hos_log("ipc: %s control method %u", name, method);
    switch (method) {
    case 0: {                                                   /* ConvertCurrentObjectToDomain */
        if (!dom) {
            IpcDomain *nd = ipc_domain_new(handle);
            if (nd) {
                nd->objs[0].id = 1;
                snprintf(nd->objs[0].svc, sizeof nd->objs[0].svc, "%.8s", name);
                nd->next = 2;
                ipc_session_set_dom(handle, (int)(nd - ipc_domains) + 1);
            }
        }
        const uint32_t id = 1;
        ipc_write_reply(tls, ver, 0, &id, 4, 0, 0, 0);
        return;
    }
    case 1: {                                                   /* CopyFromCurrentDomain */
        IpcDomObj *o = dom ? ipc_domain_obj(dom, ipc_u32(args)) : NULL;
        if (!o) { ipc_write_reply(tls, ver, 0xE401u, NULL, 0, 0, 0, 0); return; }
        const uint32_t h = ipc_new_session(o->svc);
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, 0);
        return;
    }
    case 2: case 4: {                                           /* CloneCurrentObject[Ex] */
        const uint32_t h = ipc_new_session(name);
        if (dom) ipc_session_set_dom(h, (int)(dom - ipc_domains) + 1);
        ipc_write_reply(tls, ver, 0, NULL, 0, 0, h, 0);
        return;
    }
    case 3: { const uint32_t sz = 0x8000; ipc_write_reply(tls, ver, 0, &sz, 4, 0, 0, 0); return; }
    default:
        ipc_write_reply(tls, ver, 0xF601u, NULL, 0, 0, 0, 0);
        return;
    }
}
/* a domain request on a domain session: {kind,n_obj,in_raw u16,target u32,...} + SFCI@+16 + args;
   returned objects are domain ids in the reply's object list */
static void ipc_domain_request(uint8_t *tls, const char *name, uint32_t handle) {
    const size_t off = ipc_raw_off(tls, 1);
    const uint8_t *raw = tls + off;
    if (off + 48 > HOS_IPC_TIP_BUF) { ipc_write_domain(tls, 0xE401u, NULL, 0, NULL, 0, 0, 0); return; }
    const uint32_t kind = raw[0];
    if (kind == 2) { ipc_write_domain(tls, 0, NULL, 0, NULL, 0, 0, 0); return; }   /* close object */
    if (kind != 1 || *(const uint32_t *)(raw + 16) != 0x49434653u) {
        ipc_write_domain(tls, 0xE401u, NULL, 0, NULL, 0, 0, 0);
        return;
    }
    const uint32_t target = ipc_u32(raw + 4);
    const uint32_t ver = *(const uint32_t *)(raw + 20);
    const unsigned cmd = *(const uint32_t *)(raw + 24);
    const uint8_t *args = raw + 32;
    IpcDomain *dom = ipc_domain_get(ipc_session_dom(handle));
    IpcDomObj *o = ipc_domain_obj(dom, target);
    const char *svc = o ? o->svc : name;
    hos_log("ipc: %s domain req obj %u (%s) cmd %u", name, target, svc, cmd);
    if (ipc_am_named(tls, svc, cmd, args, ver, 0, dom)) return;
    ipc_write_domain(tls, 0, NULL, 0, NULL, 0, 0, 0);
}
static void ipc_answer_named(uint8_t *tls, const char *name, uint32_t handle) {
    const uint32_t *m = (const uint32_t *)tls;
    const unsigned type = m[0] & 0xffff;
    if (type == 5 || type == 7) { ipc_control(tls, name, handle); return; }
    if (type != 2 && ipc_session_dom(handle)) { ipc_domain_request(tls, name, handle); return; }
    if (type >= 16) {                                           /* TIPC: cmd = type - 16, raw payload */
        static volatile LONG tipc_seen;
        if (InterlockedIncrement(&tipc_seen) <= 200 || getenv("HOS_IPC_LOG_ALL"))
            hos_log("ipc: %s TIPC cmd %u", name, type - 16);
        if (!strcmp(name, "sm:")) {
            if (type == 18) { ipc_write_reply(tls, 0, 0xF601u, NULL, 0, 0, 0, 1); return; }
            const size_t off = ipc_raw_off(tls, 0);
            if (off + 8 > HOS_IPC_TIP_BUF) { ipc_write_reply(tls, 0, 0xE401u, NULL, 0, 0, 0, 1); return; }
            ipc_sm_handle(tls, type - 16, tls + off, 1, 0);
            return;
        }
        {   /* nn::am services may speak TIPC too */
            const size_t off = ipc_raw_off(tls, 0);
            if (off + 8 <= HOS_IPC_TIP_BUF && ipc_am_named(tls, name, type - 16, tls + off, 0, 1, NULL)) return;
        }
        ipc_write_reply(tls, 0, 0, NULL, 0, 0, 0, 1);             /* other services: TIPC success, zeroed */
        return;
    }
    uint32_t ver = 0;
    const uint8_t *args = NULL;
    const int cmd = ipc_cmif_cmd(tls, &ver, &args);
    if (cmd == -2) { ipc_write_reply(tls, ver, 0xF601u, NULL, 0, 0, 0, 0); return; }
    if (cmd < 0) { ipc_write_reply(tls, 0, 0xE401u, NULL, 0, 0, 0, 0); return; }
    static volatile LONG ipc_seen;
    if (InterlockedIncrement(&ipc_seen) <= 200 || getenv("HOS_IPC_LOG_ALL"))
        hos_log("ipc: %s cmd %d (type %u)", name, cmd, type);
    if (!strcmp(name, "sm:")) { ipc_sm_handle(tls, (unsigned)cmd, args, 0, ver); return; }
    if (!strcmp(name, "stpl:u")) { ipc_stpl_user(tls, (unsigned)cmd, args, ver); return; }
    if (!strcmp(name, "stpl:u/iu")) { ipc_stpl_iu(tls, (unsigned)cmd, args, ver); return; }
    if (ipc_am_named(tls, name, (unsigned)cmd, args, ver, 0, NULL)) return;
    ipc_write_reply(tls, ver, 0, NULL, 0, 0, 0, 0);               /* any other service: success, zeroed outputs */
}

static uint64_t get_info(uint32_t id0, uint64_t id1, int *ok) {
    *ok = 1;
    switch (id0) {
    case 0: return (1u << hos_cores()) - 1;               /* CoreMask: cores 0 to hos_cores() - 1 */
    case 1: return 0xFFFFFFFFFFFFFFFFULL;                  /* PriorityMask */
    case 2: return HOS_HEAP_BASE;                          /* AliasRegionAddress */
    case 3: return space_end() - HOS_HEAP_BASE;            /* AliasRegionSize */
    case 4: return HOS_HEAP_BASE;                          /* HeapRegionAddress */
    case 5: return space_end() - HOS_HEAP_BASE;            /* HeapRegionSize */
    case 6: return hos_total_memory;                       /* TotalMemorySize */
    case 7: return hos_program_size + hos_heap_size;       /* UsedMemorySize */
    case 8: return 0;                                      /* DebuggerAttached */
    case 11: return 0x5A3C9E1F0B7D2468ULL ^ (id1 * 0x9E3779B97F4A7C15ULL);   /* RandomEntropy */
    case 12: return 0x08000000ULL;                         /* AslrRegionAddress */
    case 13: return space_end() - 0x08000000ULL;           /* AslrRegionSize */
    case 14: return HOS_SYSTEM_BASE;                       /* StackRegionAddress */
    case 15: return HOS_SYSTEM_END - HOS_SYSTEM_BASE;      /* StackRegionSize */
    case 16: case 17: return 0;                            /* SystemResourceSize total / used */
    case 18: return hos_program_id;                        /* ProgramId (main.npdm's, gen/hos_image.c) */
    case 20: {   /* UserExceptionContextAddress: (hookshot) OoT's nn::os::SetDyingMessageRegionPrivate writes its
                    dying-message region into it; a zeroed page, made once */
        static uint64_t ctx_page;
        if (!ctx_page) ctx_page = hos_sys_alloc(0x1000, 0x1000);
        return ctx_page;
    }
    case 21: return hos_total_memory;                      /* TotalNonSystemMemorySize */
    case 22: return hos_program_size + hos_heap_size;      /* UsedNonSystemMemorySize */
    case 23: return 1;                                     /* IsApplication */
    /* the Switch 2 SDK's (ids from Eglantine's svc_types.h / svc_info.cpp) */
    case 24: return 256;                                   /* FreeThreadCount */
    case 28: return 0;                                     /* AliasRegionExtraSize */
    case 29: return HOS_NX2_REGION_BASE;                   /* the VAMM region nnosInitialize asks for */
    case 30: return HOS_NX2_REGION_SIZE;
    case 31: case 32: return 0;                            /* remote memory (the graphics stack's): size, used */
    case 33: return 0x1000;                                /* the page granule VAMM maps in */
    case 34: return 0;                                     /* TransferMemoryHint */
    case 37: case 38: return 0;
    default: *ok = 0; return 0;
    }
}

/* ---- mutex arbitration and process-wide condition variables -------------------------------------------------
   Horizon's user-mode mutex: the word holds the owner's handle, bit 30 means "has waiters". A thread that loses the
   race sets bit 30 and calls ArbitrateLock; the owner's ArbitrateUnlock hands the word straight to the
   highest-priority waiter. A condition variable wait releases the mutex and sleeps on the key; a signal moves a
   waiter back to its mutex, or gives it the mutex if free (switchbrew "SVC", libnx mutex.c / condvar.c). */
#define HANDLE_WAIT_MASK 0x40000000u
#define R_TIMED_OUT 0xEA01

typedef struct { HosThread *t; uint64_t mutex, key; uint32_t tag; int on_key; } Waiter;
static Waiter waiters[1024];
static int nwaiters;
static SRWLOCK klock = SRWLOCK_INIT;

static uint32_t word_get(uint64_t a) { return __atomic_load_n((uint32_t *)GP(a), __ATOMIC_SEQ_CST); }
static void word_set(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)GP(a), v, __ATOMIC_SEQ_CST); }
/* User mode changes a mutex word with exclusives (compare-and-swap here) while the kernel holds klock: a kernel
   read-modify-write of a word user mode may change must be a compare-and-swap too. */
static int word_cas(uint64_t a, uint32_t expect, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)GP(a), &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* The best waiter (lowest priority value, then oldest) for a mutex (on_key 0) or a key (on_key 1). */
static int best_waiter(uint64_t addr, int on_key, int *others) {
    int best = -1;
    *others = 0;
    for (int i = 0; i < nwaiters; i++) {
        Waiter *w = &waiters[i];
        if (w->on_key != on_key || (on_key ? w->key : w->mutex) != addr) continue;
        if (best < 0) best = i;
        else {
            (*others)++;
            if (hos_thread_prio(w->t) < hos_thread_prio(waiters[best].t)) best = i;
        }
    }
    return best;
}
static void remove_waiter(int i) { memmove(&waiters[i], &waiters[i + 1], (nwaiters - i - 1) * sizeof *waiters); nwaiters--; }
static int find_thread(HosThread *t) {
    for (int i = 0; i < nwaiters; i++) if (waiters[i].t == t) return i;
    return -1;
}

/* Hand a held mutex to its next waiter, or free it. Caller holds klock. */
static void release_locked(uint64_t mutex) {
    int others, i = best_waiter(mutex, 0, &others);
    if (i < 0) { word_set(mutex, 0); return; }
    Waiter w = waiters[i];
    remove_waiter(i);
    word_set(mutex, w.tag | (others ? HANDLE_WAIT_MASK : 0));
    hos_thread_wake(w.t);
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
#include <emmintrin.h>
static int spin_for_release(uint64_t mutex, uint32_t held) {
    static int64_t limit = -1;
    if (limit < 0) {
        const char *e = getenv("HOS_SPIN_US");
        limit = (e ? atoll(e) : 20) * 1000;
    }
    static int misses[256], tries[256];
    const unsigned slot = (unsigned)(mutex >> 3 ^ mutex >> 11) & 255;
    const int m = __atomic_load_n(&misses[slot], __ATOMIC_RELAXED);
    if (limit <= 0 || (m >= 4 && (__atomic_add_fetch(&tries[slot], 1, __ATOMIC_RELAXED) & 31))) return 0;
    const int64_t end = hos_now_ns() + limit;
    for (;;) {
        for (int i = 0; i < 16; i++) {
            if (word_get(mutex) != held) {
                __atomic_store_n(&misses[slot], 0, __ATOMIC_RELAXED);
                return 1;
            }
            _mm_pause();
        }
        if (hos_now_ns() >= end) {
            if (m < 4) __atomic_store_n(&misses[slot], m + 1, __ATOMIC_RELAXED);
            return 0;
        }
    }
}
#else
static int spin_for_release(uint64_t mutex, uint32_t held) { (void)mutex; (void)held; return 0; }
#endif

/* svcArbitrateLock(owner handle, mutex address, own tag) */
static uint32_t arbitrate_lock(uint32_t owner, uint64_t mutex, uint32_t tag) {
    HosThread *self = hos_self();
    if (spin_for_release(mutex, owner | HANDLE_WAIT_MASK)) return 0;
    AcquireSRWLockExclusive(&klock);
    if (word_get(mutex) != (owner | HANDLE_WAIT_MASK)) {   /* released meanwhile: let user mode retry */
        ReleaseSRWLockExclusive(&klock);
        return 0;
    }
    if (nwaiters == 1024) hos_fatal(NULL, "kernel: too many waiters");
    waiters[nwaiters++] = (Waiter){self, mutex, 0, tag, 0};
    ReleaseSRWLockExclusive(&klock);
    int to;
    hos_thread_wait(self, -1, &to);   /* woken once release_locked made us the owner */
    return 0;
}

/* svcArbitrateUnlock(mutex address) */
static uint32_t arbitrate_unlock(uint64_t mutex) {
    AcquireSRWLockExclusive(&klock);
    release_locked(mutex);
    ReleaseSRWLockExclusive(&klock);
    return 0;
}

/* svcWaitProcessWideKeyAtomic(mutex address, key address, own tag, timeout ns) */
static uint32_t wait_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    HosThread *self = hos_self();
    AcquireSRWLockExclusive(&klock);
    /* The key says "a waiter" before the mutex is released (as Horizon's kernel does): user mode signals only when it
       reads a nonzero key, so a signaller that took the mutex the moment it was free saw 0, skipped the signal, and
       this thread slept forever (the rare stall while loading: a thread on a key of 1 with its mutex free). */
    word_set(key, 1);
    if (nwaiters == 1024) hos_fatal(NULL, "kernel: too many waiters");
    waiters[nwaiters++] = (Waiter){self, mutex, key, tag, 1};
    release_locked(mutex);
    ReleaseSRWLockExclusive(&klock);
    int to;
    hos_thread_wait(self, timeout, &to);
    if (!to) return 0;
    AcquireSRWLockExclusive(&klock);
    int i = find_thread(self);
    if (i >= 0 && waiters[i].on_key) {   /* still waiting on the key: a real timeout; user mode relocks */
        remove_waiter(i);
        ReleaseSRWLockExclusive(&klock);
        return R_TIMED_OUT;
    }
    ReleaseSRWLockExclusive(&klock);
    /* signaled meanwhile: either moved to the mutex queue (wait for the hand-over) or already handed the mutex
       (the wake is pending); both end with one wake-up */
    hos_thread_wait(self, -1, &to);
    return 0;
}

/* svcSignalProcessWideKey(key address, count): count <= 0 wakes all */
static void signal_key(uint64_t key, int32_t count) {
    AcquireSRWLockExclusive(&klock);
    for (int n = 0; count <= 0 || n < count; n++) {
        int others, i = best_waiter(key, 1, &others);
        if (i < 0) break;
        Waiter *w = &waiters[i];
        /* The owner may unlock (owner -> 0) and other threads lock (0 -> them) in user mode meanwhile: a plain store
           here once left the word naming a thread that had unlocked it (its next Lock aborted: "locked by the
           current thread"), or gave a free mutex to two threads. */
        for (;;) {
            const uint32_t cur = word_get(w->mutex);
            if (cur == 0) {                           /* mutex free: it is theirs */
                if (!word_cas(w->mutex, 0, w->tag)) continue;
                HosThread *t = w->t;
                remove_waiter(i);
                hos_thread_wake(t);
            } else {                                  /* queue on the mutex */
                if (!(cur & HANDLE_WAIT_MASK) && !word_cas(w->mutex, cur, cur | HANDLE_WAIT_MASK)) continue;
                w->on_key = 0;
            }
            break;
        }
    }
    int others;
    if (best_waiter(key, 1, &others) < 0) word_set(key, 0);
    ReleaseSRWLockExclusive(&klock);
}

/* ---- (hookshot) the address arbiter: svcWaitForAddress / svcSignalToAddress -----------------------------------
   OoT's SDK (22.2.8) uses them; MKW's never did. Waiters share the table above (on_key 2, key = the address) and klock,
   so the condition check and the sleep are atomic against a signal, as in Horizon's KAddressArbiter (mesosphere).
   Wait types: 0 WaitIfLessThan, 1 DecrementAndWaitIfLessThan, 2 WaitIfEqual (32-bit), 3 WaitIfEqual64 (21.0+).
   Signal types: 0 Signal, 1 SignalAndIncrementIfEqual, 2 SignalAndModifyByWaitingCountIfEqual. */
#define R_INVALID_STATE 0xFA01   /* svc::ResultInvalidState: the value did not match */
static int addr_waiters(uint64_t addr) {
    int n = 0;
    for (int i = 0; i < nwaiters; i++) if (waiters[i].on_key == 2 && waiters[i].key == addr) n++;
    return n;
}
static void wake_address_locked(uint64_t addr, int32_t count) {
    for (int n = 0; count <= 0 || n < count; n++) {
        int others, i = best_waiter(addr, 2, &others);
        if (i < 0) break;
        HosThread *t = waiters[i].t;
        remove_waiter(i);
        hos_thread_wake(t);
    }
}
static uint32_t wait_address(uint64_t addr, uint32_t type, uint64_t value, int64_t timeout) {
    HosThread *self = hos_self();
    AcquireSRWLockExclusive(&klock);
    int wait;
    switch (type) {
    case 0: wait = (int32_t)word_get(addr) < (int32_t)value; break;
    case 1: {
        uint32_t cur;
        for (;;) {
            cur = word_get(addr);
            if ((int32_t)cur >= (int32_t)value) { wait = 0; break; }
            if (word_cas(addr, cur, cur - 1)) { wait = 1; break; }
        }
        break;
    }
    case 2: wait = (int32_t)word_get(addr) == (int32_t)value; break;
    case 3: wait = __atomic_load_n((uint64_t *)GP(addr), __ATOMIC_SEQ_CST) == value; break;
    default:
        ReleaseSRWLockExclusive(&klock);
        hos_log("svcWaitForAddress: arbitration type %u", type);
        return R_INVALID_ENUM;
    }
    if (!wait) { ReleaseSRWLockExclusive(&klock); return R_INVALID_STATE; }
    if (timeout == 0) { ReleaseSRWLockExclusive(&klock); return R_TIMED_OUT; }
    if (nwaiters == 1024) hos_fatal(NULL, "kernel: too many waiters");
    waiters[nwaiters++] = (Waiter){self, 0, addr, 0, 2};
    ReleaseSRWLockExclusive(&klock);
    int to;
    hos_thread_wait(self, timeout, &to);
    if (!to) return 0;
    AcquireSRWLockExclusive(&klock);
    int i = find_thread(self);
    if (i >= 0 && waiters[i].on_key == 2) {   /* still queued: a real timeout */
        remove_waiter(i);
        ReleaseSRWLockExclusive(&klock);
        return R_TIMED_OUT;
    }
    ReleaseSRWLockExclusive(&klock);
    hos_thread_wait(self, -1, &to);   /* signaled meanwhile: take the pending wake-up */
    return 0;
}
static uint32_t signal_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    AcquireSRWLockExclusive(&klock);
    uint32_t r = 0;
    if (type == 1) {
        if (!word_cas(addr, (uint32_t)value, (uint32_t)value + 1)) r = R_INVALID_STATE;
    } else if (type == 2) {
        /* the new value from the number of waiters, as mesosphere's KAddressArbiter computes it */
        const int n = addr_waiters(addr);
        int32_t nv;
        if (count <= 0) nv = n > 0 ? value - 2 : value + 1;
        else if (n > 0) {
            const int more = n - 1 < count + 1 ? n - 1 : count + 1;   /* the waiters after the first, capped */
            nv = more == 0 ? value + 1 : more <= count ? value - 1 : value;
        } else nv = value + 1;
        if (!word_cas(addr, (uint32_t)value, (uint32_t)nv)) r = R_INVALID_STATE;
    } else if (type != 0) {
        ReleaseSRWLockExclusive(&klock);
        hos_log("svcSignalToAddress: signal type %u", type);
        return R_INVALID_ENUM;
    }
    if (!r) wake_address_locked(addr, count);
    ReleaseSRWLockExclusive(&klock);
    return r;
}

/* Every thread waiting in the kernel's locks and condition variables, with the words they wait on (printed with the
   thread stacks at HOS_EXIT_AFTER: who holds what in a hang). */
void hos_kernel_dump(void) {
    AcquireSRWLockExclusive(&klock);
    hos_log("kernel: %d threads waiting", nwaiters);
    for (int i = 0; i < nwaiters; i++) {
        const Waiter *w = &waiters[i];
        hos_log("kernel:   %s (tag %#x) on %s %#llx: mutex %#llx = %#x, key %#llx = %#x", hos_thread_name(w->t), w->tag,
                w->on_key ? "key" : "mutex", (unsigned long long)(w->on_key ? w->key : w->mutex),
                (unsigned long long)w->mutex, w->mutex ? word_get(w->mutex) : 0, (unsigned long long)w->key,
                w->key ? word_get(w->key) : 0);
    }
    ReleaseSRWLockExclusive(&klock);
    void hos_thread_log_handles(void);
    hos_thread_log_handles();
}

/* svcQueryMemory: MemoryInfo { u64 addr, size; u32 state, attr, perm, ipc_refs, device_refs, pad } */
extern const uint64_t hos_image_end;
static void query_memory(uint64_t out, uint64_t addr) {
    uint64_t base = 0, size = 0;
    uint32_t state = 0, perm = 0;   /* Free */
    uint64_t stack, stack_size;
    hos_thread_stack(hos_self(), &stack, &stack_size);
    if (addr >= stack && addr < stack + stack_size) { base = stack; size = stack_size; state = 0x0B; perm = 3; }   /* Stack */
    else if (addr >= hos_image_base && addr < hos_image_end) { base = hos_image_base; size = hos_image_end - base; state = 0x03; perm = 5; }   /* Code */
    else if (addr >= HOS_HEAP_BASE && addr < HOS_HEAP_BASE + hos_heap_size) { base = HOS_HEAP_BASE; size = hos_heap_size; state = 0x05; perm = 3; }   /* Normal */
    else if (addr >= HOS_SYSTEM_BASE && addr < HOS_SYSTEM_END) { base = HOS_SYSTEM_BASE; size = HOS_SYSTEM_END - HOS_SYSTEM_BASE; state = 0x05; perm = 3; }
    else { base = addr & ~0xFFFULL; size = 0x1000; }   /* Free: one page, enough for callers probing an address */
    memset(GP(out), 0, 0x28);
    G64(out) = base;
    G64(out + 8) = size;
    G32(out + 16) = state;
    G32(out + 24) = perm;
}

/* A thread handle, with Horizon's pseudo-handle for the current thread. */
#define CUR_THREAD 0xFFFF8000u
static HosThread *thread_handle(uint32_t h) { return h == CUR_THREAD ? hos_self() : hos_thread_by_handle(h); }

void a64_svc(Ctx *c, uint32_t imm) {
    hos_vcore_checkpoint();
    switch (imm) {
    case 0x01:   /* SetHeapSize(-, size) -> (result, heap address) */
        hos_heap_size = c->x[1];
        c->x[0] = R_OK;
        c->x[1] = HOS_HEAP_BASE;
        return;
    case 0x02:   /* SetMemoryPermission */
    case 0x03:   /* SetMemoryAttribute */
    case 0x16:   /* CloseHandle */
    case 0x2A:   /* FlushEntireDataCache */
    case 0x2B:   /* FlushDataCache */
        c->x[0] = R_OK;
        return;
    case 0x0A:   /* ExitThread */
        hos_vcore_away();
        ExitThread(0);
        return;
    case 0x07:   /* ExitProcess */
        hos_log("svcExitProcess");
        hos_log_flush();
        ExitProcess(0);
        return;
    case 0x0C: { /* GetThreadPriority(-, handle) -> (result, priority) */
        HosThread *t = thread_handle((uint32_t)c->x[1]);
        c->x[1] = t ? (uint32_t)hos_thread_prio(t) : 44;
        c->x[0] = t ? R_OK : 0xE401;   /* svc::ResultInvalidHandle */
        return;
    }
    case 0x0D: { /* SetThreadPriority(handle, priority) */
        HosThread *t = thread_handle((uint32_t)c->x[0]);
        if (t) hos_thread_priority(t, 1, (int)c->x[1]);
        c->x[0] = t ? R_OK : 0xE401;
        return;
    }
    case 0x0E: { /* GetThreadCoreMask(-, -, handle) -> (result, ideal core, affinity mask) */
        HosThread *t = thread_handle((uint32_t)c->x[2]);
        c->x[1] = t ? (uint32_t)hos_thread_core(t) : 0;
        c->x[2] = t && hos_thread_mask(t) ? hos_thread_mask(t) : 0x7;
        c->x[0] = t ? R_OK : 0xE401;
        return;
    }
    case 0x0F: { /* SetThreadCoreMask(handle, ideal core, mask): -1 keeps the ideal core, -2/-3 defaults */
        HosThread *t = thread_handle((uint32_t)c->x[0]);
        if (t) hos_thread_affinity(t, (int)c->x[1], c->x[2]);
        c->x[0] = t ? R_OK : 0xE401;
        return;
    }
    case 0x24:   /* GetProcessId(-, handle) -> (result, id) */
        c->x[1] = 0x51;
        c->x[0] = R_OK;
        return;
    case 0x06:   /* QueryMemory(MemoryInfo* out, -, address) -> (result, page info) */
        query_memory(c->x[0], c->x[2]);
        c->x[0] = R_OK;
        c->x[1] = 0;
        return;
    case 0x1A:   /* ArbitrateLock(owner handle, mutex, tag) */
        c->x[0] = arbitrate_lock((uint32_t)c->x[0], c->x[1], (uint32_t)c->x[2]);
        return;
    case 0x1B:   /* ArbitrateUnlock(mutex) */
        c->x[0] = arbitrate_unlock(c->x[0]);
        return;
    case 0x1C:   /* WaitProcessWideKeyAtomic(mutex, key, tag, timeout) */
        c->x[0] = wait_key(c->x[0], c->x[1], (uint32_t)c->x[2], (int64_t)c->x[3]);
        return;
    case 0x34:   /* WaitForAddress(address, type, value, timeout) (hookshot) */
        c->x[0] = wait_address(c->x[0], (uint32_t)c->x[1], c->x[2], (int64_t)c->x[3]);
        return;
    case 0x35:   /* SignalToAddress(address, type, value, count) (hookshot) */
        c->x[0] = signal_address(c->x[0], (uint32_t)c->x[1], (int32_t)c->x[2], (int32_t)c->x[3]);
        return;
    case 0x1D:   /* SignalProcessWideKey(key, count) */
        signal_key(c->x[0], (int32_t)c->x[1]);
        return;
    case 0x0B: { /* SleepThread(ns) */
        void hos_sleep_logged(int64_t ns, uint64_t lr);
        hos_sleep_logged((int64_t)c->x[0], c->x[30] | 1);
        return;
    }
    case 0x10:   /* GetCurrentProcessorNumber */
        c->x[0] = (uint32_t)hos_thread_core(hos_self());
        return;
    case 0x1E:   /* GetSystemTick */
        c->x[0] = hos_ticks();
        return;
    case 0x25: { /* GetThreadId(-, handle) -> (result, id) */
        HosThread *t = thread_handle((uint32_t)c->x[1]);
        c->x[1] = hos_thread_id(t ? t : hos_self());
        c->x[0] = R_OK;
        return;
    }
    case 0x26: { /* Break(reason, info, size) */
        uint64_t n = c->x[2] > 256 ? 256 : c->x[2];
        if (n) {
            const uint8_t *ip = GP(c->x[1]);
            char hx[3 * 64 + 1];
            const uint64_t hn = n > 64 ? 64 : n;
            for (uint64_t i = 0; i < hn; i++) snprintf(hx + 3 * i, 4, "%02x ", ip[i]);
            hx[3 * hn] = 0;
            hos_log("svcBreak info (%llu bytes): %s | hex: %s", (unsigned long long)n, (const char *)ip, hx);
        }
        hos_fatal(c, "svcBreak(reason %#llx, info %#llx, size %#llx)", (unsigned long long)c->x[0],
                  (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
        return;
    }
    case 0x27: { /* OutputDebugString(str, size) */
        uint64_t n = c->x[1] > 4096 ? 4096 : c->x[1];
        char buf[4097];
        memcpy(buf, GP(c->x[0]), n);
        buf[n] = 0;
        hos_log("debug: %s", buf);
        c->x[0] = R_OK;
        return;
    }
    case 0x2C:   /* MapPhysicalMemory(address, size) */
        hos_commit(c->x[0], c->x[1]);
        if (hos_trace) hos_log("MapPhysicalMemory(%#llx, %#llx)", (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
        c->x[0] = R_OK;
        return;
    case 0x13: { /* MapSharedMemory(handle, addr, size, perm) (Switch 2 ids; pokopia-recomp svc_memory.cpp) */
        IpcShm *s = ipc_shm((uint32_t)c->x[0]);
        if (s && !(c->x[1] & 0xFFF) && c->x[2] && !(c->x[2] & 0xFFF) && c->x[2] <= s->size) {
            hos_commit(c->x[1], c->x[2]);
            memcpy(GP(c->x[1]), s->buf, (size_t)c->x[2]);
            s->map_addr = c->x[1];
            s->map_size = c->x[2];
            c->x[0] = R_OK;
        } else {
            hos_log("svcMapSharedMemory: bad handle %#llx / addr %#llx size %#llx",
                    (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
            c->x[0] = 0xE401;
        }
        return;
    }
    case 0x14: { /* UnmapSharedMemory(handle, addr, size): copy the guest's view back into the object buffer */
        IpcShm *s = ipc_shm((uint32_t)c->x[0]);
        if (s && s->map_addr == c->x[1] && s->map_size == c->x[2]) {
            memcpy(s->buf, GP(c->x[1]), (size_t)c->x[2]);
            c->x[0] = R_OK;
        } else {
            c->x[0] = 0xE401;
        }
        return;
    }
    case 0x2D:   /* UnmapPhysicalMemory(address, size): the pages read as zero if mapped again */
        {   /* (pages under a Vulkan import stay committed, zeroed: hos_mem.c FreeMemoryPages) */
            extern int rx_guest_imported(uint64_t lo, uint64_t hi);
            if (rx_guest_imported(c->x[0], c->x[0] + c->x[1])) memset(GP(c->x[0]), 0, (size_t)c->x[1]);
            else VirtualFree(GP(c->x[0]), (SIZE_T)c->x[1], MEM_DECOMMIT);
        }
        c->x[0] = R_OK;
        return;
    case 0x1F: { /* ConnectToNamedPort(-, name) -> (result, session handle): (hookshot) SDK code that resolves a
                    service through "sm:" (nn::sm::Initialize, from nn::nsd's HIPC client in OoT) gets a session
                    whose requests svc 0x21 answers empty, as for any service the runtime does not emulate. */
        const char *name = c->x[1] ? (const char *)GP(c->x[1]) : "";
        uint32_t h = ipc_new_session(name);
        hos_log("ipc: ConnectToNamedPort(\"%.11s\") -> session %#x (requests answered empty) (lr %#llx)", name, h,
                (unsigned long long)c->x[30]);
        c->x[0] = R_OK;
        c->x[1] = h;
        return;
    }
    case 0x21: { /* SendSyncRequest(session handle): (mkw-recomp) a service the runtime does not emulate. The request
                    is in the thread's IPC buffer (TLS + 0); the answer is a CMIF success with zeroed outputs, as
                    Eglantine's auto-stub does, so a lifted service client sees an empty, successful reply. */
        uint8_t *tls = (uint8_t *)GP(c->tpidrro_el0);
        static int ipc_dump = -1;
        if (ipc_dump < 0) ipc_dump = getenv("HOS_IPC_DUMP") != NULL;
        if (ipc_dump) {
            char hx[3 * 96 + 1];
            for (int i = 0; i < 96; i++) snprintf(hx + 3 * i, 4, "%02x ", tls[i]);
            hos_log("ipc: dump %s req: %s", ipc_session_name((uint32_t)c->x[0]) ? ipc_session_name((uint32_t)c->x[0]) : "?", hx);
        }
        const char *svcname = ipc_session_name((uint32_t)c->x[0]);
        if (svcname) { ipc_answer_named(tls, svcname, (uint32_t)c->x[0]); c->x[0] = R_OK; return; }
        const uint32_t w0 = *(uint32_t *)tls, w1 = *(uint32_t *)(tls + 4);
        /* the request's CMIF header ('SFCI', version, command id) after the HIPC header, 16-byte aligned */
        uint32_t cmd = 0;
        for (int off = 8; off < 0x40; off += 4)
            if (*(uint32_t *)(tls + off) == 0x49434653u) { cmd = *(uint32_t *)(tls + off + 8); break; }
        static volatile LONG logged;
        static int log_all = -1;
        if (log_all < 0) log_all = getenv("HOS_IPC_LOG_ALL") != NULL;   /* bring-up: every empty answer, no 64 cap */
        if (log_all || InterlockedIncrement(&logged) <= 64)
            hos_log("ipc: %s handle %#x command %u (hipc %08x %08x, lr %#llx): answered empty",
                    svcname ? svcname : "?", (uint32_t)c->x[0], cmd, w0, w1, (unsigned long long)c->x[30]);
        if ((w0 & 0xffff) >= 16) {                  /* TIPC: reply {result 0}, no SFCO */
            ipc_write_reply(tls, 0, 0, NULL, 0, 0, 0, 1);
            c->x[0] = R_OK;
            return;
        }
        memset(tls, 0, 0x100);
        *(uint32_t *)(tls + 0) = 0;                 /* HIPC: type 0, no descriptors */
        *(uint32_t *)(tls + 4) = 16;                /* 16 raw data words, no handle descriptor */
        *(uint32_t *)(tls + 16) = 0x4F434653u;      /* CMIF response 'SFCO', version 0, result 0 */
        c->x[0] = R_OK;
        return;
    }
    case 0x29: { /* GetInfo(id0, handle, id1) -> (result, value) */
        int ok;
        uint64_t v = get_info((uint32_t)c->x[1], c->x[3], &ok);
        if (!ok) hos_log("svcGetInfo: unknown id %#x/%#llx (lr %#llx)", (uint32_t)c->x[1],
                         (unsigned long long)c->x[3], (unsigned long long)c->x[30]);
        c->x[1] = v;
        c->x[0] = ok ? R_OK : R_INVALID_ENUM;
        return;
    }
    default:
        hos_fatal(c, "svc %#x not implemented (x0 %#llx x1 %#llx x2 %#llx)", imm, (unsigned long long)c->x[0],
                  (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    }
}
