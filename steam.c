// Steam: the game's libsteam_api.dylib, answered by the Steam client's own.
//
// The game loads @executable_path/../Frameworks/libsteam_api.dylib with dlopen and looks its functions up by name.
// That library is i386 and cannot be loaded. The Steam for Mac client carries the same library for arm64, and it
// is the one that talks to the running client. So dlopen of that name is answered here: every function the game
// looks up is a native function that calls the client's.
//
// Nothing is decided here. Whether Steam is running, who is logged in and whether the account owns the game are
// the client's answers, passed on as they come. With no client, or a library that cannot be found, the functions
// say so (SteamAPI_Init fails) and the game does what it does without Steam.
//
// The interfaces (ISteamUser and the rest) are C++ objects. The game is given a guest object for each, whose
// virtual table is native functions; a call arrives with the slot's number, and is passed to the same slot of the
// client's object, asked for by the version string of the game's SDK (1.32: SteamUser018 and so on), so the
// layouts agree. Each slot needs its signature to be passed on, because a pointer and an integer cross
// differently; the ones the game is seen to call are in SIGS, and a call to one that is not is printed with its
// arguments and answered with zero.
//
//   i386 Darwin: cdecl, `this` first; a 64-bit argument is two slots; bool in al.
//   Signature: return ':' arguments.  v void, i 32 bits, b bool, q 64 bits, p pointer to guest memory,
//   s returned C string (copied into guest memory).
//
// Callbacks: the game registers C++ objects (CCallbackBase: vptr, flags at +4, the callback's number at +8; Run,
// Run with a call result, GetCallbackSizeBytes). The client is given a native object of the same shape for each,
// whose Run copies the client's data into guest memory and calls the game's Run. Steam's structures are packed
// the same for 32 and 64 bits on a Mac, so the data crosses as bytes.
#include "rt.h"
#include "tables.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t aot_host_fn(void (*fn)(CPU *, uint32_t), uint32_t ctx);
uint32_t guest_call_here(uint32_t addr, int nargs, const uint32_t *args);
uint32_t aot_malloc(uint32_t n);
void aot_free(uint32_t p);

#define RET(v) (c->eax = (uint32_t)(v))
#define HANDLE 0x57EA0001u   // what dlopen gives the game for the library

static void *lib;            // the client's libsteam_api.dylib
static int tried;
static void *sym(const char *name) { return lib ? dlsym(lib, name) : NULL; }
static void load(void) {
    if (tried) return;
    tried = 1;
    const char *env = getenv("BMAA_STEAM_API");
    char p[2048];
    const char *places[] = {
        "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Steam Helper.app/Contents/MacOS/libsteam_api.dylib",
        "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/libsteam_api.dylib",
    };
    if (env) lib = dlopen(env, RTLD_NOW | RTLD_LOCAL);
    for (unsigned k = 0; !lib && k < sizeof places / sizeof *places; k++) {
        snprintf(p, sizeof p, places[k], getenv("HOME") ? getenv("HOME") : "");
        lib = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (lib) fprintf(stderr, "  [steam] the client's library: %s\n", p);
    }
    if (!lib) fprintf(stderr, "  [steam] the Steam client's libsteam_api.dylib was not found: the game runs without Steam\n");
}

// ---------- interfaces ----------
typedef struct { const char *name, *version; int user; } Iface;   // user: asked for with the logged-in user's handle
enum { I_CLIENT, I_USER, I_FRIENDS, I_UTILS, I_USERSTATS, I_APPS, I_REMOTESTORAGE, I_NETWORKING, I_MATCHMAKING,
       I_MATCHMAKINGSERVERS, NIFACE };
static const Iface IFACES[NIFACE] = {
    {"ISteamClient", "SteamClient017", 0}, {"ISteamUser", "SteamUser018", 1}, {"ISteamFriends", "SteamFriends015", 1},
    {"ISteamUtils", "SteamUtils007", 0}, {"ISteamUserStats", "STEAMUSERSTATS_INTERFACE_VERSION011", 1},
    {"ISteamApps", "STEAMAPPS_INTERFACE_VERSION007", 1}, {"ISteamRemoteStorage", "STEAMREMOTESTORAGE_INTERFACE_VERSION012", 1},
    {"ISteamNetworking", "SteamNetworking005", 1}, {"ISteamMatchmaking", "SteamMatchMaking009", 1},
    {"ISteamMatchmakingServers", "SteamMatchMakingServers002", 1},
};
#define NSLOTS 128
static void *host_obj[NIFACE];
static uint32_t guest_obj[NIFACE];

// The slots the game has been seen to call, by interface and number. (SDK 1.32.)
typedef struct { int iface, slot; const char *name, *sig; } Sig;
static const Sig SIGS[] = {
    {I_APPS, 0, "BIsSubscribed", "b:"},
    {I_APPS, 4, "GetCurrentGameLanguage", "s:"},
    {I_APPS, 6, "BIsSubscribedApp", "b:i"},
    {I_APPS, 7, "BIsDlcInstalled", "b:i"},
    {I_USER, 0, "GetHSteamUser", "i:"},
    {I_USER, 1, "BLoggedOn", "b:"},
    {I_USER, 2, "GetSteamID", "q:"},   // an 8-byte class: Darwin i386 returns it in edx:eax
    {I_UTILS, 9, "GetAppID", "i:"},
    {I_USERSTATS, 0, "RequestCurrentStats", "b:"},
    {I_USERSTATS, 10, "StoreStats", "b:"},
    {I_FRIENDS, 0, "GetPersonaName", "s:"},
    // The saved games: the game keeps them in Steam's cloud storage (profile.bin, Save0.sgd) and nowhere else, so
    // these are what makes a game continue, and what Steam then synchronises. (ISteamRemoteStorage 012.)
    {I_REMOTESTORAGE, 0, "FileWrite", "b:ppi"},
    {I_REMOTESTORAGE, 1, "FileRead", "i:ppi"},
    {I_REMOTESTORAGE, 2, "FileForget", "b:p"},
    {I_REMOTESTORAGE, 3, "FileDelete", "b:p"},
    {I_REMOTESTORAGE, 10, "FileExists", "b:p"},
    {I_REMOTESTORAGE, 11, "FilePersisted", "b:p"},
    {I_REMOTESTORAGE, 12, "GetFileSize", "i:p"},
    {I_REMOTESTORAGE, 13, "GetFileTimestamp", "q:p"},
    {I_REMOTESTORAGE, 15, "GetFileCount", "i:"},
    {I_REMOTESTORAGE, 16, "GetFileNameAndSize", "s:ip"},
    {I_REMOTESTORAGE, 17, "GetQuota", "b:pp"},
    {I_REMOTESTORAGE, 18, "IsCloudEnabledForAccount", "b:"},
    {I_REMOTESTORAGE, 19, "IsCloudEnabledForApp", "b:"},
};
static const Sig *sig_of(int iface, int slot) {
    for (unsigned k = 0; k < sizeof SIGS / sizeof *SIGS; k++) if (SIGS[k].iface == iface && SIGS[k].slot == slot) return &SIGS[k];
    return NULL;
}

static uint32_t guest_string(const char *s) {   // one per call site would leak: a small ring of them
    static uint32_t ring[16]; static int at;
    if (!s) return 0;
    uint32_t n = (uint32_t)strlen(s) + 1, g = aot_malloc(n);
    memcpy(G2H(g), s, n);
    if (ring[at]) aot_free(ring[at]);
    ring[at] = g; at = (at + 1) % 16;
    return g;
}

// A call to slot `slot` of interface `iface`: the guest's `this` is ARG(0), its own arguments follow.
static void slot_call(CPU *c, uint32_t ctx) {
    int iface = (int)(ctx >> 8), slot = (int)(ctx & 0xFF);
    void *o = host_obj[iface];
    const Sig *s = sig_of(iface, slot);
    static uint8_t said[NIFACE][NSLOTS];
    if (!s || !o) {
        if (!said[iface][slot]) {
            said[iface][slot] = 1;
            fprintf(stderr, "  [steam] %s slot %d %s: a=%08x,%08x,%08x,%08x from %s -> 0\n", IFACES[iface].name, slot,
                    o ? "has no signature here" : "but the client gave no such interface", ARG(1), ARG(2), ARG(3), ARG(4),
                    rt_func_name(rd32(c->esp)));
        }
        RET(0); c->edx = 0;
        return;
    }
    uint64_t a[8] = {0}; int n = 0, at = 1;
    for (const char *p = strchr(s->sig, ':') + 1; *p && n < 8; p++) {
        if (*p == 'i') a[n++] = ARG(at++);
        else if (*p == 'q') { a[n++] = (uint64_t)ARG(at) | (uint64_t)ARG(at + 1) << 32; at += 2; }
        else if (*p == 'p') { uint32_t g = ARG(at++); a[n++] = g ? (uint64_t)(uintptr_t)G2H(g) : 0; }
    }
    void **vt = *(void ***)o;
    uint64_t r = ((uint64_t (*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))vt[slot])(
        o, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    char rt = s->sig[0];
    if (!said[iface][slot]) {
        said[iface][slot] = 1;
        fprintf(stderr, "  [steam] %s::%s -> %s%llx (first call)\n", IFACES[iface].name, s->name, rt == 's' ? "string at " : "0x",
                rt == 'b' ? (unsigned long long)(r & 0xFF) : (unsigned long long)r);
    }
    if (rt == 'b') RET(r & 0xFF);
    else if (rt == 'q') { c->eax = (uint32_t)r; c->edx = (uint32_t)(r >> 32); }
    else if (rt == 's') RET(guest_string((const char *)(uintptr_t)r));
    else RET(r);
}

static pthread_mutex_t ilock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t iface_obj(int k) {
    pthread_mutex_lock(&ilock);
    if (!guest_obj[k]) {
        load();
        void *(*create)(const char *) = sym("SteamInternal_CreateInterface");
        void *(*find)(int, const char *) = sym("SteamInternal_FindOrCreateUserInterface");
        int (*huser)(void) = sym("SteamAPI_GetHSteamUser");
        if (k == I_CLIENT) host_obj[k] = create ? create(IFACES[k].version) : NULL;
        else host_obj[k] = find ? find(IFACES[k].user && huser ? huser() : 0, IFACES[k].version) : NULL;
        fprintf(stderr, "  [steam] %s (%s): %s\n", IFACES[k].name, IFACES[k].version, host_obj[k] ? "the client has it" : "the client did not give it");
        // the guest's object: a pointer to a table of NSLOTS native functions
        uint32_t vt = aot_malloc(4 * NSLOTS), ob = aot_malloc(16);
        for (int sl = 0; sl < NSLOTS; sl++) wr32(vt + 4 * (uint32_t)sl, aot_host_fn(slot_call, (uint32_t)k << 8 | (uint32_t)sl));
        wr32(ob, vt);
        guest_obj[k] = ob;
    }
    pthread_mutex_unlock(&ilock);
    return host_obj[k] ? guest_obj[k] : 0;
}
static void f_iface(CPU *c, uint32_t k) { RET(iface_obj((int)k)); }

// ---------- callbacks ----------
// The client's side of a callback object: what CCallbackBase is for arm64, and the guest object it stands for.
typedef struct { void **vt; uint8_t flags; int icb; uint32_t guest; } Proxy;
static uint32_t gvt(uint32_t obj, int slot) { return rd32(rd32(obj) + 4u * (uint32_t)slot); }
static void proxy_run(Proxy *p, void *data) {
    uint32_t a1[1] = { p->guest };
    uint32_t size = guest_call_here(gvt(p->guest, 2), 1, a1);
    if (size > 65536) size = 65536;
    uint32_t buf = aot_malloc(size ? size : 4);
    memcpy(G2H(buf), data, size);
    uint32_t a[2] = { p->guest, buf };
    guest_call_here(gvt(p->guest, 0), 2, a);
    aot_free(buf);
}
static void proxy_run_result(Proxy *p, void *data, bool io_failure, uint64_t call) {
    uint32_t a1[1] = { p->guest };
    uint32_t size = guest_call_here(gvt(p->guest, 2), 1, a1);
    if (size > 65536) size = 65536;
    uint32_t buf = aot_malloc(size ? size : 4);
    memcpy(G2H(buf), data, size);
    uint32_t a[5] = { p->guest, buf, io_failure, (uint32_t)call, (uint32_t)(call >> 32) };
    guest_call_here(gvt(p->guest, 1), 5, a);
    aot_free(buf);
}
static int proxy_size(Proxy *p) { uint32_t a[1] = { p->guest }; return (int)guest_call_here(gvt(p->guest, 2), 1, a); }
static void *PROXY_VT[3] = { (void *)proxy_run, (void *)proxy_run_result, (void *)proxy_size };
#define NPROXY 1024
static Proxy proxies[NPROXY];
static Proxy *proxy_for(uint32_t guest, int make) {
    for (int k = 0; k < NPROXY; k++) if (proxies[k].guest == guest) return &proxies[k];
    if (!make) return NULL;
    for (int k = 0; k < NPROXY; k++) if (!proxies[k].guest) { proxies[k].vt = PROXY_VT; proxies[k].guest = guest; proxies[k].flags = rd8(guest + 4); return &proxies[k]; }
    return NULL;
}
static void f_register_callback(CPU *c, uint32_t x) {   // (CCallbackBase *, int iCallback)
    void (*f)(void *, int) = sym("SteamAPI_RegisterCallback");
    Proxy *p = proxy_for(ARG(0), 1);
    if (f && p) { f(p, (int)ARG(1)); wr8(ARG(0) + 4, p->flags); wr32(ARG(0) + 8, (uint32_t)p->icb); }
}
static void f_unregister_callback(CPU *c, uint32_t x) {
    void (*f)(void *) = sym("SteamAPI_UnregisterCallback");
    Proxy *p = proxy_for(ARG(0), 0);
    if (f && p) { f(p); wr8(ARG(0) + 4, p->flags); p->guest = 0; }
}
static void f_register_call_result(CPU *c, uint32_t x) {   // (CCallbackBase *, SteamAPICall_t: two slots)
    void (*f)(void *, uint64_t) = sym("SteamAPI_RegisterCallResult");
    Proxy *p = proxy_for(ARG(0), 1);
    if (f && p) { p->icb = (int)rd32(ARG(0) + 8); f(p, (uint64_t)ARG(1) | (uint64_t)ARG(2) << 32); }
}
static void f_unregister_call_result(CPU *c, uint32_t x) {
    void (*f)(void *, uint64_t) = sym("SteamAPI_UnregisterCallResult");
    Proxy *p = proxy_for(ARG(0), 0);
    if (f && p) { f(p, (uint64_t)ARG(1) | (uint64_t)ARG(2) << 32); p->guest = 0; }
}

// ---------- the flat functions ----------
static void f_restart(CPU *c, uint32_t x) {
    bool (*f)(uint32_t) = sym("SteamAPI_RestartAppIfNecessary");
    bool r = f ? f(ARG(0)) : false;   // no client to restart through: the game goes on, and SteamAPI_Init will say no
    fprintf(stderr, "  [steam] SteamAPI_RestartAppIfNecessary(%u) -> %d%s\n", ARG(0), r, f ? "" : " (no client library)");
    RET(r);
}
static void f_init(CPU *c, uint32_t x) {
    bool (*f)(void) = sym("SteamAPI_InitSafe");
    bool r = f ? f() : false;
    fprintf(stderr, "  [steam] SteamAPI_Init -> %d%s\n", r, f ? " (the client's answer)" : " (no client library)");
    RET(r);
}
static void f_running(CPU *c, uint32_t x) { bool (*f)(void) = sym("SteamAPI_IsSteamRunning"); RET(f ? f() : 0); }
static void f_run_callbacks(CPU *c, uint32_t x) { void (*f)(void) = sym("SteamAPI_RunCallbacks"); if (f) f(); }
static void f_shutdown(CPU *c, uint32_t x) { void (*f)(void) = sym("SteamAPI_Shutdown"); if (f) f(); }
static void f_huser(CPU *c, uint32_t x) { int (*f)(void) = sym("SteamAPI_GetHSteamUser"); RET(f ? f() : 0); }
static void f_hpipe(CPU *c, uint32_t x) { int (*f)(void) = sym("SteamAPI_GetHSteamPipe"); RET(f ? f() : 0); }
static void f_nothing(CPU *c, uint32_t x) { RET(0); }

static const struct { const char *name; void (*fn)(CPU *, uint32_t); uint32_t ctx; } NAMES[] = {
    {"SteamAPI_RestartAppIfNecessary", f_restart, 0}, {"SteamAPI_Init", f_init, 0}, {"SteamAPI_InitSafe", f_init, 0},
    {"SteamAPI_IsSteamRunning", f_running, 0}, {"SteamAPI_RunCallbacks", f_run_callbacks, 0}, {"SteamAPI_Shutdown", f_shutdown, 0},
    {"SteamAPI_RegisterCallback", f_register_callback, 0}, {"SteamAPI_UnregisterCallback", f_unregister_callback, 0},
    {"SteamAPI_RegisterCallResult", f_register_call_result, 0}, {"SteamAPI_UnregisterCallResult", f_unregister_call_result, 0},
    {"SteamAPI_GetHSteamUser", f_huser, 0}, {"GetHSteamUser", f_huser, 0}, {"SteamAPI_GetHSteamPipe", f_hpipe, 0}, {"GetHSteamPipe", f_hpipe, 0},
    {"SteamAPI_WriteMiniDump", f_nothing, 0}, {"SteamAPI_SetMiniDumpComment", f_nothing, 0}, {"SteamAPI_SetBreakpadAppID", f_nothing, 0},
    {"SteamAPI_UseBreakpadCrashHandler", f_nothing, 0}, {"SteamAPI_SetTryCatchCallbacks", f_nothing, 0},
    {"SteamClient", f_iface, I_CLIENT}, {"SteamUser", f_iface, I_USER}, {"SteamFriends", f_iface, I_FRIENDS},
    {"SteamUtils", f_iface, I_UTILS}, {"SteamUserStats", f_iface, I_USERSTATS}, {"SteamApps", f_iface, I_APPS},
    {"SteamRemoteStorage", f_iface, I_REMOTESTORAGE}, {"SteamNetworking", f_iface, I_NETWORKING},
    {"SteamMatchmaking", f_iface, I_MATCHMAKING}, {"SteamMatchmakingServers", f_iface, I_MATCHMAKINGSERVERS},
};
static uint32_t addr_of[sizeof NAMES / sizeof *NAMES];

// ---------- dlopen, dlsym, dlclose ----------
void I__dlopen(CPU *c) {
    const char *path = ARG(0) ? (const char *)G2H(ARG(0)) : NULL;
    if (path && strstr(path, "libsteam_api")) {
        load();
        fprintf(stderr, "  [steam] the game loads %s: answered with the client's%s\n", path, lib ? "" : ", which is not there");
        RET(lib ? HANDLE : 0);
        return;
    }
    unimpl_import(c, "_dlopen");
}
void I__dlsym(CPU *c) {
    if (ARG(0) != HANDLE) { unimpl_import(c, "_dlsym"); return; }
    const char *name = (const char *)G2H(ARG(1));
    for (unsigned k = 0; k < sizeof NAMES / sizeof *NAMES; k++)
        if (!strcmp(NAMES[k].name, name)) {
            if (!addr_of[k]) addr_of[k] = aot_host_fn(NAMES[k].fn, NAMES[k].ctx);
            RET(addr_of[k]);
            return;
        }
    fprintf(stderr, "  [steam] the game looks up %s, which is not answered here -> not found\n", name);
    RET(0);
}
void I__dlclose(CPU *c) { if (ARG(0) == HANDLE) { RET(0); return; } unimpl_import(c, "_dlclose"); }
