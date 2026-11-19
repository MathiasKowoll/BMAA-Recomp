// The game's translated code as the CPU of DecompRecomp's runtime.
//
// DecompRecomp (~/Development/DecompRecomp) runs this game on an interpreter behind a small interface,
// runtime/include/dr_x86.h, and everything the game asks of macOS is answered by its bridge (Objective-C, AppKit,
// CoreFoundation, Carbon, libstdc++, OpenGL). Recomp-Framework's mac32 translator turns the same executable into
// C. This file is that interface implemented on the translated functions, in place of DecompRecomp's src/x86.c:
// the loader, the bridge and the driver are DecompRecomp's, unchanged, and guest code runs natively.
//
// How the two meet:
//   - Guest memory is the same thing on both sides: guest address a is host address base + a. M is that base.
//   - The guest's registers live in the translated code's CPU while guest code runs and in dr_cpu while the bridge
//     does; they are copied at the two crossings (an import, and a run starting or ending). XMM registers are
//     never the bridge's business and stay in CPU.
//   - An import: translated code calls I_<name>, whose default is unimpl_import(name). DecompRecomp's loader put a
//     thunk address in the lazy pointer of every imported function, so the name gives the stub, the stub its
//     pointer, and the pointer the thunk the bridge wants. A call through a pointer that holds a thunk goes the
//     same way.
//   - The bridge's tail jump (objc_msgSend finding a method of the game's) is a call of that function with the
//     stack as it is.
//   - A run is left early (an import nobody answers, exit, a fault in guest memory, a C++ throw) with a jump back
//     to dr_cpu_run, which hands DecompRecomp the same status its interpreter would.
//
// Threads. DecompRecomp has one guest thread and refuses pthread_create; this game ends its start-up by creating
// its game thread. Here every guest thread is a host thread with a CPU of its own (and a dr_cpu of its own for the
// bridge to read arguments from), and threads.c answers what creates threads and what blocks. The bridge was not
// written for two threads at once, so one lock wraps every call into it; it is let go while guest code runs
// (a callback from the bridge) and while the bridge sits in a host call that may block (dr_host_call_enter).
#include <objc/message.h>
#include <objc/runtime.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#include "dr_bridge.h"
#include "dr_x86.h"

#include "rt.h"
#include "tables.h"

uint8_t *M;
uint8_t rt_watch[0x10000000 / 8];
const char *rt_func_name(uint32_t a);
// trace.c opens a window between two bridge calls: every function entered in it is printed, with what it was given.
extern void (*trace_window_hook)(int on);
static int window_open;
static void window(int on) { window_open = on; memset(rt_watch, on ? 0xFF : 0, sizeof rt_watch); }
// DR_WATCH=addr,addr,...: those functions are printed each time they are entered (the first 40 times each), with
// what their first four arguments point at, when they point into guest memory that is there. DR_WATCH_FROM=lo-hi
// keeps only the entries called from that range of code; DR_WATCH_MAX=n raises the 40.
// What a first argument says when it is a host string or URL the game holds by handle: the text, for DR_LOG_OBJC.
static const char *said(uint32_t a) {
    static _Thread_local char buf[200];
    extern dr_objc *dr_objc_actual(void);
    dr_objc *o = dr_objc_actual();
    if (!o || a < DR_OBJC_HANDLE_BASE) return "";
    void *h = dr_objc_object_for(o, a);
    if (!h) return "";
    typedef void *(*send0)(void *, SEL);
    typedef signed char (*sendk)(void *, SEL, Class);
    Class str = objc_getClass("NSString"), url = objc_getClass("NSURL");
    SEL kind = sel_registerName("isKindOfClass:");
    if (((sendk)objc_msgSend)(h, kind, url)) h = ((send0)objc_msgSend)(h, sel_registerName("absoluteString"));
    else if (!((sendk)objc_msgSend)(h, kind, str)) return "";
    const char *t = h ? ((send0)objc_msgSend)(h, sel_registerName("UTF8String")) : NULL;
    if (!t) return "";
    snprintf(buf, sizeof buf, "  \"%.180s\"", t);
    return buf;
}

static void show_last(void);
static void on_usr1(int sig);
static void on_term(int sig) {
    (void)sig;
    extern void dr_bridge_report_unimplemented(uint32_t top);
    dr_bridge_report_unimplemented(80);
    fflush(stdout);
    _exit(143);
}
static int watching_some;
static void watch_some(void) {
    const char *w = getenv("DR_WATCH");
    for (char *e; w && *w; w = *e ? e + 1 : e) {
        uint32_t a = (uint32_t)strtoul(w, &e, 16);
        if (e == w) break;
        rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7)); watching_some = 1;
    }
}
static void show(uint32_t p) {
    if (p < 0x1000 || p >= 0xE0000000u) return;
    if (p < 0x60000000u && !(p < 0x04000000u || (p >= 0x1f000000u && p < 0x20000000u) || (p >= 0x50000000u && p < 0x51000000u))) return;
    if (p >= 0xA0000000u) return;   // past the guest heap: a float's bits, not a pointer
    fprintf(stderr, "        %08x:", p);
    for (int k = 0; k < 32; k++) fprintf(stderr, " %02x", rd8(p + k));
    fprintf(stderr, "  |");
    for (int k = 0; k < 32; k++) { uint8_t ch = rd8(p + k); fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
    fprintf(stderr, "|\n");
}
void rt_on_watch(CPU *c, uint32_t a) {
    if (watching_some && rt_watch[0] != 0xFF) {
        // DR_WATCH_FROM=lo-hi: only the entries whose caller returns into that range (one function's calls)
        static uint32_t from_lo = 1, from_hi;
        if (from_lo == 1) {
            const char *e = getenv("DR_WATCH_FROM"); char *dash = NULL;
            from_lo = e ? (uint32_t)strtoul(e, &dash, 16) : 0;
            from_hi = (dash && *dash == '-') ? (uint32_t)strtoul(dash + 1, NULL, 16) : from_lo;
        }
        if (from_lo && (rd32(c->esp) < from_lo || rd32(c->esp) > from_hi)) return;
        static struct { uint32_t a; int n; } seen[64];
        static int watch_max;
        if (!watch_max) watch_max = getenv("DR_WATCH_MAX") ? atoi(getenv("DR_WATCH_MAX")) : 40;
        for (int k = 0; k < 64; k++) {
            if (seen[k].a == a || !seen[k].a) { seen[k].a = a; if (++seen[k].n > watch_max) return; break; }
        }
        fprintf(stderr, "  [watch] %08x from %08x (%s)  a=%08x,%08x,%08x,%08x  ecx=%08x\n", a, rd32(c->esp), rt_func_name(rd32(c->esp)),
                rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), rd32(c->esp + 16), c->ecx);
        for (int k = 1; k <= 4; k++) show(rd32(c->esp + 4 * k));
        if (getenv("DR_WATCH_LAST")) show_last();   // and what this thread was last told by the bridge
        return;
    }
    fprintf(stderr, "  [fn] %08x %s  ret=%08x a=%08x,%08x,%08x  eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x esp=%08x\n",
            a, rt_func_name(a), rd32(c->esp), rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12),
            c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->esp);
}

static dr_cpu *main_cpu;                 // DecompRecomp's CPU: the main thread's
static CPU main_G;
static _Thread_local dr_cpu *Dt;         // this thread's, for the bridge
static _Thread_local CPU *Gt;            // this thread's, as the translated code sees it
static _Thread_local sigjmp_buf *leave;  // the innermost run of this thread
static _Thread_local dr_status leave_with;
#define D Dt
#define G (*Gt)
static uint64_t nimports, ncalls_ind;
CPU *cpu_new(uint32_t stack_size);
void aot_thread_begin(CPU *c);

// ---- the lock around the bridge ----
static pthread_mutex_t big;
static pthread_once_t big_once = PTHREAD_ONCE_INIT;
static void big_make(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&big, &a);
}
static _Thread_local int big_depth;
static void big_lock(void) { pthread_once(&big_once, big_make); pthread_mutex_lock(&big); big_depth++; }
static void big_unlock(void) { big_depth--; pthread_mutex_unlock(&big); }
static int big_release_all(void) { int d = big_depth; while (big_depth) big_unlock(); return d; }
static void big_reacquire(int d) { while (big_depth < d) big_lock(); }
// The bridge is about to make a host call that may block, or has come back from it.
static _Thread_local int held_before[32], nheld_before;
static void host_call_enter(void) { if (nheld_before < 32) held_before[nheld_before++] = big_release_all(); }
static void host_call_leave(void) { if (nheld_before > 0) big_reacquire(held_before[--nheld_before]); }

static void in(void) {
    G.eax = D->regs[DR_EAX]; G.ecx = D->regs[DR_ECX]; G.edx = D->regs[DR_EDX]; G.ebx = D->regs[DR_EBX];
    G.esp = D->regs[DR_ESP]; G.ebp = D->regs[DR_EBP]; G.esi = D->regs[DR_ESI]; G.edi = D->regs[DR_EDI];
    memcpy(G.st, D->st, sizeof G.st); G.top = D->fpu_top; G.fcw = D->fpu_cw;
}
static void out(void) {
    D->regs[DR_EAX] = G.eax; D->regs[DR_ECX] = G.ecx; D->regs[DR_EDX] = G.edx; D->regs[DR_EBX] = G.ebx;
    D->regs[DR_ESP] = G.esp; D->regs[DR_EBP] = G.ebp; D->regs[DR_ESI] = G.esi; D->regs[DR_EDI] = G.edi;
    memcpy(D->st, G.st, sizeof G.st); D->fpu_top = G.top; D->fpu_cw = G.fcw; D->mxcsr = G.mxcsr;
}

static const GFunc *func_at(uint32_t a) {
    int lo = 0, hi = ngfuncs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gfuncs[m].addr == a) return &gfuncs[m];
        if (gfuncs[m].addr < a) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}
static const GFunc *func_holding(uint32_t a) {
    int lo = 0, hi = ngfuncs - 1, best = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gfuncs[m].addr <= a) { best = m; lo = m + 1; } else hi = m - 1;
    }
    return best < 0 ? NULL : &gfuncs[best];
}
const char *rt_func_name(uint32_t a) { const GFunc *f = func_holding(a); return f ? f->name : "?"; }

static void stop(dr_status s) __attribute__((noreturn));
static void stop(dr_status s) {
    out();
    D->eip = G.fn;
    leave_with = s;
    if (!leave) { fprintf(stderr, "  [aot] %s outside a run\n", dr_status_str(s)); _exit(1); }
    siglongjmp(*leave, 1);
}

static void enter(CPU *c, uint32_t t);

// An import, with the return address on the guest stack and the arguments above it. The caller pops the address.
static void import(CPU *c, uint32_t thunk) {
    nimports++;
    out();
    D->eip = thunk;
    D->tail_jump = false;
    big_lock();
    bool ok = D->on_import && D->on_import(D, thunk, D->user);
    big_unlock();
    if (!ok) stop(D->exited ? DR_HALT_EXIT : D->threw ? DR_ERR_THROW : DR_ERR_IMPORT);
    in();
    if (D->tail_jump) {   // the bridge found guest code to run in its place: same stack, same return address
        D->tail_jump = false;
        enter(c, D->tail_target);
        c->esp -= 4;      // that function popped the address; the caller of an import pops it too
    }
}

// Control goes to guest address t with the return address on the stack; when this returns, the address is popped.
// Native functions the guest can call through a pointer (what dlsym hands it for a library answered natively):
// addresses in a range of their own, each with a word of context.
#define HOSTFN_BASE 0xFD000000u
#define NHOSTFN 8192
static struct { void (*fn)(CPU *, uint32_t); uint32_t ctx; } hostfns[NHOSTFN];
static int nhostfns;
uint32_t aot_host_fn(void (*fn)(CPU *, uint32_t), uint32_t ctx) {
    int k = __atomic_fetch_add(&nhostfns, 1, __ATOMIC_SEQ_CST);
    if (k >= NHOSTFN) { fprintf(stderr, "  [aot] out of native function addresses\n"); abort(); }
    hostfns[k].fn = fn; hostfns[k].ctx = ctx;
    return HOSTFN_BASE + 16u * (uint32_t)k;
}

// A guest function answered natively when it is reached through a pointer (a virtual method, a stage of a table):
// the project lists the few it needs (game.c). hle.txt does the same for direct calls.
static struct { uint32_t addr; void (*fn)(CPU *); } overrides[16];
static int noverrides;
void aot_override(uint32_t addr, void (*fn)(CPU *)) { if (noverrides < 16) { overrides[noverrides].addr = addr; overrides[noverrides++].fn = fn; } }
void game_init(void) __attribute__((weak));

static int stub_of_thunk(uint32_t t);
static void enter(CPU *c, uint32_t t) {
    for (int k = 0; k < noverrides; k++)
        if (overrides[k].addr == t) { overrides[k].fn(c); c->esp += 4; return; }
    if (t >= HOSTFN_BASE && t < HOSTFN_BASE + 16u * NHOSTFN) {
        uint32_t k = (t - HOSTFN_BASE) / 16;
        if (hostfns[k].fn) { hostfns[k].fn(c, hostfns[k].ctx); c->esp += 4; return; }
    }
    if (t >= DR_THUNK_BASE) {
        // By its stub's function when it has one: that is this file's unimpl_import for most, and for the few
        // that threads.c answers itself (what creates threads and what blocks) it is that answer.
        int k = stub_of_thunk(t);
        if (k >= 0) gstubs[k].fn(c); else import(c, t);
        c->esp += 4; return;
    }
    const GFunc *f = func_at(t);
    if (f) { f->fn(c); return; }
    for (int k = 0; k < ngstubs; k++)   // the address of a stub, taken as a function pointer
        if (gstubs[k].addr == t) { gstubs[k].fn(c); c->esp += 4; return; }
    if (t < D->null_floor) stop(DR_ERR_NULLCALL);
    fprintf(stderr, "  [aot] call to 0x%08x, which is no function of the translation (from %s)\n", t, rt_func_name(c->fn));
    if (t >= DR_UNRESOLVED_BASE) fprintf(stderr, "  [aot]   that is the marker of a data symbol nobody resolved\n");
    stop(DR_ERR_OPCODE);
}
// DR_CALLSITE=addr: an indirect call that returns to that address is printed, with where it went, what it was
// given and what it answered.
static uint32_t callsite = 1, callsite_hi;   // or DR_CALLSITE=lo-hi: every indirect call that returns into that range
void call_ind(CPU *c, uint32_t t) {
    ncalls_ind++;
    if (callsite == 1) {
        const char *e = getenv("DR_CALLSITE"); char *dash = NULL;
        callsite = e ? (uint32_t)strtoul(e, &dash, 16) : 0;
        callsite_hi = (dash && *dash == '-') ? (uint32_t)strtoul(dash + 1, NULL, 16) : callsite;
    }
    uint32_t ra = callsite ? rd32(c->esp) : 0;
    if (callsite && ra >= callsite && ra <= callsite_hi) {
        fprintf(stderr, "  [site] %08x calls %08x (%s)  a=%08x,%08x,%08x  ecx=%08x\n", ra, t, rt_func_name(t),
                rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), c->ecx);
        enter(c, t);
        fprintf(stderr, "  [site] %08x: it answered eax=%08x edx=%08x\n", ra, c->eax, c->edx);
        return;
    }
    enter(c, t);
}

// ---- names of imports -> thunks ----
// A stub is `jmp *[pointer]` (ff 25, then the pointer's address), and DecompRecomp's loader wrote the thunk there.
static struct { const char *name; uint32_t thunk; } *by_name;
static uint32_t nby_name;
static uint32_t hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static int32_t *stub_by_thunk;   // (thunk - base) / stride -> stub index + 1
static pthread_once_t names_once = PTHREAD_ONCE_INIT;
static void names_make(void) {
    nby_name = 1; while (nby_name < (uint32_t)ngstubs * 2 + 16) nby_name <<= 1;
    by_name = calloc(nby_name, sizeof *by_name);
    stub_by_thunk = calloc(DR_THUNK_MAX, sizeof *stub_by_thunk);
    for (int k = 0; k < ngstubs; k++) {
        uint32_t a = gstubs[k].addr, t = 0;
        if (rd8(a) == 0xFF && rd8(a + 1) == 0x25) t = rd32(rd32(a + 2));
        uint32_t i = hash(gstubs[k].name) & (nby_name - 1);
        while (by_name[i].name) i = (i + 1) & (nby_name - 1);
        by_name[i].name = gstubs[k].name; by_name[i].thunk = t;
        if (t >= DR_THUNK_BASE && (t - DR_THUNK_BASE) / DR_THUNK_STRIDE < DR_THUNK_MAX)
            stub_by_thunk[(t - DR_THUNK_BASE) / DR_THUNK_STRIDE] = k + 1;
    }
}
static uint32_t thunk_of(const char *name) {
    pthread_once(&names_once, names_make);
    for (uint32_t i = hash(name) & (nby_name - 1); by_name[i].name; i = (i + 1) & (nby_name - 1))
        if (!strcmp(by_name[i].name, name)) return by_name[i].thunk;
    return 0;
}
static int stub_of_thunk(uint32_t t) {
    pthread_once(&names_once, names_make);
    uint32_t i = (t - DR_THUNK_BASE) / DR_THUNK_STRIDE;
    return i < DR_THUNK_MAX ? stub_by_thunk[i] - 1 : -1;
}

// setjmp is compiled into the function that calls it (the host's setjmp, in that frame); longjmp is its other half
// and cannot be the bridge's, which would move an interpreter's instruction pointer.
#define NJMP 256
static GJmp jmps[NJMP];
GJmp *gjmp_slot(uint32_t buf) {
    for (int i = 0; i < NJMP; i++) if (jmps[i].guest_buf == buf) return &jmps[i];
    for (int i = 0; i < NJMP; i++) if (!jmps[i].guest_buf) { jmps[i].guest_buf = buf; return &jmps[i]; }
    fprintf(stderr, "  [aot] out of jmp slots\n"); abort();
}

// The last calls this thread made into the bridge, kept to be shown when it faults: what the game was last told
// is usually why.
#define NLAST 48
static _Thread_local struct { const char *name; uint32_t a[3], from, eax, sel; } last[NLAST];
static _Thread_local unsigned nlast;
static void show_last(void) {
    for (unsigned k = nlast > NLAST ? nlast - NLAST : 0; k < nlast; k++) {
        char b[300];
        uint32_t sel = last[k % NLAST].sel;
        const char *what = (sel > 0x1000 && sel < 0x04000000u) ? (const char *)G2H(sel) : last[k % NLAST].name;
        int n = snprintf(b, sizeof b, "      %s%-44.60s a=%08x,%08x,%08x -> %08x  from %s\n", sel ? "@ " : "", what, last[k % NLAST].a[0],
                         last[k % NLAST].a[1], last[k % NLAST].a[2], last[k % NLAST].eax, rt_func_name(last[k % NLAST].from));
        write(2, b, (size_t)n);
    }
}

static void on_usr1(int sig) {
    (void)sig;
    char b[160]; int n = snprintf(b, sizeof b, "  [aot] SIGUSR1: this thread is in %s; its last calls into the bridge:\n", Gt ? rt_func_name(G.fn) : "no guest code");
    write(2, b, (size_t)n);
    show_last();
}

void unimpl_import(CPU *c, const char *name) {
    if (!strcmp(name, "_longjmp") || !strcmp(name, "__longjmp") || !strcmp(name, "_siglongjmp")) {
        GJmp *j = gjmp_slot(ARG(0));
        j->val = ARG(1) ? ARG(1) : 1;
        longjmp(j->host, 1);
    }
    uint32_t thunk = thunk_of(name);
    if (window_open) fprintf(stderr, "  [imp] %s  a=%08x,%08x,%08x,%08x  from %s\n", name, ARG(0), ARG(1), ARG(2), ARG(3), rt_func_name(rd32(c->esp)));
    if (!strcmp(name, "_exit") || !strcmp(name, "__Exit")) {   // what it was told before it chose to leave
        fprintf(stderr, "  [aot] the game calls %s(%d); this thread's last calls into the bridge:\n", name, (int)ARG(0));
        show_last();
    }
    unsigned slot = nlast++ % NLAST;
    last[slot].name = name; last[slot].a[0] = ARG(0); last[slot].a[1] = ARG(1); last[slot].a[2] = ARG(2);
    last[slot].sel = !strncmp(name, "_objc_msgSend", 13) ? ARG(strstr(name, "_stret") ? 2 : 1) : 0;
    {   // DR_LOG_OBJC: every Objective-C message the game sends, by name, with its receiver and who sent it
        static int log_objc = -1;
        if (log_objc < 0) log_objc = getenv("DR_LOG_OBJC") != NULL;
        uint32_t sel = last[slot].sel;
        if (log_objc && sel > 0x1000 && sel < 0x04000000u) {
            uint32_t a0 = ARG(strstr(name, "_stret") ? 3 : 2);
            fprintf(stderr, "  [msg] %s  to %08x  a=%08x,%08x  from %s%s\n", (const char *)G2H(sel),
                    ARG(strstr(name, "_stret") ? 1 : 0), a0, ARG(strstr(name, "_stret") ? 4 : 3), rt_func_name(rd32(c->esp)),
                    strcasestr((const char *)G2H(sel), "url") || strcasestr((const char *)G2H(sel), "string") ||
                    strcasestr((const char *)G2H(sel), "path") || strcasestr((const char *)G2H(sel), "title") ||
                    strcasestr((const char *)G2H(sel), "file") || strcasestr((const char *)G2H(sel), "named") ? said(a0) : "");
        }
    }
    last[slot].from = rd32(c->esp); last[slot].eax = 0xBADBAD00u;
    if (thunk < DR_THUNK_BASE) {
        fprintf(stderr, "  [aot] import %s has no thunk (its pointer holds 0x%08x), called from %s\n", name, thunk, rt_func_name(c->fn));
        stop(DR_ERR_IMPORT);
    }
    import(c, thunk);
    last[slot].eax = c->eax;
}

// ---- what translated code calls for itself ----
void missing_func(CPU *c, uint32_t addr, uint32_t from) {
    fprintf(stderr, "  [aot] call to untranslated 0x%08x from 0x%08x (%s)\n", addr, from, rt_func_name(from));
    stop(DR_ERR_OPCODE);
}
void bad_jump(CPU *c, uint32_t t, uint32_t from) {
    fprintf(stderr, "  [aot] indirect jump to 0x%08x at 0x%08x (%s), which is no case of its table\n", t, from, rt_func_name(from));
    stop(DR_ERR_OPCODE);
}
void bad_insn(CPU *c, uint32_t a) {
    fprintf(stderr, "  [aot] instruction at 0x%08x (%s) was not translated\n", a, rt_func_name(a));
    stop(DR_ERR_OPCODE);
}
int32_t cvtt_i32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int32_t)v : INT32_MIN; }
double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
void cpu_set_mxcsr(CPU *c, uint32_t v) {
    static const int mode[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    c->mxcsr = v;
    fesetround(mode[(v >> 13) & 3]);
}
void cpu_cpuid(CPU *c) { c->eax = 1; c->ebx = c->ecx = c->edx = 0; }

// ---- dr_x86.h ----
#define MAGIC_RET 0xDEADC0DEu

void dr_cpu_init(dr_cpu *cpu, dr_guest *g) {
    memset(cpu, 0, sizeof *cpu);
    cpu->g = g;
    cpu->magic_ret = MAGIC_RET;
    cpu->ilimit = 50ull * 1000 * 1000;
    cpu->fpu_cw = 0x037F;
    cpu->mxcsr = 0x1F80;
    cpu->flags = 0x202;
    M = g->base;
    main_cpu = cpu;
    Dt = cpu;
    Gt = &main_G;
    memset(&G, 0, sizeof G);
    G.fcw = 0x37F; G.mxcsr = 0x1F80;
    G.stack_hi = DR_STACK_TOP; G.stack_lo = DR_STACK_TOP - DR_STACK_SIZE;
    dr_host_call_enter = host_call_enter;
    dr_host_call_leave = host_call_leave;
    trace_window_hook = window;
    watch_some();
    if (game_init) game_init();
    { extern void dbg_init(void); dbg_init(); }   // kill -USR2: the windows and their views
    signal(SIGTERM, on_term);   // a run stopped from outside still says which imports nobody answered
    signal(SIGUSR1, on_usr1);   // kill -USR1: what the thread that takes the signal was last told by the bridge
    printf("cpu       : codigo traducido (%d funciones), no el interprete\n", ngfuncs);
}

void dr_cpu_fpush(dr_cpu *cpu, double v) {
    cpu->fpu_top = (cpu->fpu_top - 1) & 7;
    cpu->st[cpu->fpu_top] = v;
}

void dr_cpu_setup_call(dr_cpu *cpu, uint32_t func, const uint32_t *args, int argc) {
    cpu->regs[DR_ESP] = DR_STACK_TOP - 256;
    for (int i = argc - 1; i >= 0; i--) dr_push(cpu, args[i]);
    dr_push(cpu, cpu->magic_ret);
    cpu->eip = func;
}

// A fault inside the guest's 4 GB is the guest's (a wild pointer of the game's, or of ours handed to it): the run
// ends with DR_ERR_FAULT, as the interpreter's does. Anywhere else it is the host's, and fatal.
static void on_fault(int sig, siginfo_t *info, void *uctx) {
    (void)sig; (void)uctx;
    uint8_t *a = (uint8_t *)info->si_addr;
    if (!D || !Gt || !leave || a < M || a >= M + DR_GUEST_SIZE) {
        char b[256];
        int n = snprintf(b, sizeof b, "  [aot] host fault at %p (guest memory is %p..%p), in or under guest %s\n",
                         (void *)a, (void *)M, (void *)(M + DR_GUEST_SIZE), Gt ? rt_func_name(G.fn) : "no guest thread");
        write(2, b, (size_t)n);
        _exit(139);
    }
    D->fault_addr = (uint32_t)(a - M);
    D->faulted = true;
    {   // where: the function, and who called it, by the chain of frames
        char b[700]; int n = snprintf(b, sizeof b, "  [aot] guest fault at 0x%08x, last function entered %s; eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x; frames:",
                                      D->fault_addr, rt_func_name(G.fn), G.eax, G.ecx, G.edx, G.ebx, G.esi, G.edi);
        uint32_t ebp = G.ebp;
        // the thread's own stack, or one of the stacks the host's calls into the game run on
        for (int k = 0; k < 12 && n < 600 && ((ebp >= G.stack_lo && ebp + 8 <= G.stack_hi) ||
                                              (ebp >= 0x50000000u && ebp < 0x58800000u)); k++) {
            n += snprintf(b + n, sizeof b - (size_t)n, " %s", rt_func_name(rd32(ebp + 4)));
            uint32_t up = rd32(ebp); if (up <= ebp) break; ebp = up;
        }
        b[n++] = '\n'; write(2, b, (size_t)n);
        show_last();
    }
    stop(DR_ERR_FAULT);
}
void dr_cpu_install_fault_handler(dr_cpu *cpu) {
    (void)cpu;
    static char alt[SIGSTKSZ * 4];
    stack_t ss = {0}; ss.ss_sp = alt; ss.ss_size = sizeof alt;
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

// Guest code runs from here until the function at D->eip returns. The lock around the bridge is let go for that
// long, whoever holds it, and taken back as it was.
static dr_status run(void) {
    sigjmp_buf here, *outer = leave;
    volatile int held = big_release_all();
    if (sigsetjmp(here, 1)) { leave = outer; big_release_all(); big_reacquire(held); return leave_with; }
    leave = &here;
    in();
    G.fn = D->eip;
    enter(&G, D->eip);       // returns when that function does, its return address (the sentinel) popped
    out();
    leave = outer;
    big_reacquire(held);
    return DR_HALT_RET;
}

dr_status dr_cpu_run(dr_cpu *cpu) {
    if (!Dt) aot_thread_begin(NULL);
    if (cpu != Dt) {   // the driver's CPU, on a thread that is not the main one: its call, on this thread's stack
        Dt->eip = cpu->eip;
    }
    dr_status st = run();
    if (st == DR_HALT_RET) Dt->eip = Dt->magic_ret;
    return st;
}

// The bridge calls back into the guest: a method of one of the game's classes, a callback it registered. It names
// the CPU it was given at start-up, which is the main thread's; the call runs on the thread it arrives on, with
// that thread's CPU. On the main thread the stack is the one the bridge chose; on any other, the thread's own
// stack below where it stands, which is free while the thread is inside the bridge or the host.
dr_status dr_cpu_call_nested(dr_cpu *given, uint32_t func, const uint32_t *args, int argc, uint32_t stack_top,
                             uint32_t *out_eax) {
    return dr_cpu_call_nested2(given, func, args, argc, stack_top, out_eax, NULL, NULL);
}

// The same, handing back edx too and, when asked, popping st(0): how a guest method returns a float or a double.
dr_status dr_cpu_call_nested2(dr_cpu *given, uint32_t func, const uint32_t *args, int argc, uint32_t stack_top,
                              uint32_t *out_eax, uint32_t *out_edx, double *out_st0) {
    if (!Dt) aot_thread_begin(NULL);
    dr_cpu *cpu = Dt;
    if (cpu != given) stack_top = (cpu->regs[DR_ESP] ? cpu->regs[DR_ESP] : G.esp) - 1024;
    if (cpu->nest_depth >= DR_MAX_NEST) return DR_ERR_LIMIT;
    uint32_t saved_regs[8]; memcpy(saved_regs, cpu->regs, sizeof saved_regs);
    uint32_t saved_eip = cpu->eip, saved_flags = cpu->flags, saved_fn = G.fn;
    bool saved_exited = cpu->exited, saved_tail = cpu->tail_jump;
    uint32_t saved_tail_target = cpu->tail_target, saved_magic = cpu->magic_ret;

    cpu->nest_depth++;
    cpu->magic_ret = 0xDEAD0000u + (uint32_t)cpu->nest_depth;
    cpu->regs[DR_ESP] = stack_top & ~15u;
    for (int i = argc - 1; i >= 0; i--) dr_push(cpu, args[i]);
    dr_push(cpu, cpu->magic_ret);
    cpu->eip = func;
    cpu->exited = false;

    dr_status st = run();
    if (out_eax) *out_eax = cpu->regs[DR_EAX];
    if (out_edx) *out_edx = cpu->regs[DR_EDX];
    if (out_st0 && st == DR_HALT_RET) { *out_st0 = cpu->st[cpu->fpu_top]; cpu->fpu_top = (cpu->fpu_top + 1) & 7; }
    cpu->fail_eip = (st == DR_OK || st == DR_HALT_RET) ? 0 : cpu->eip;
    given->fail_eip = cpu->fail_eip;
    if (cpu != given && st == DR_ERR_FAULT) given->fault_addr = cpu->fault_addr;

    cpu->magic_ret = saved_magic;
    cpu->nest_depth--;
    memcpy(cpu->regs, saved_regs, sizeof saved_regs);
    cpu->eip = saved_eip; cpu->flags = saved_flags; cpu->exited = saved_exited;
    cpu->tail_jump = saved_tail; cpu->tail_target = saved_tail_target;
    G.fn = saved_fn;
    in();   // the thread goes on from where it was
    return st;
}

// ---- for threads.c ----
// A guest stack and a CPU that starts at its top.
CPU *cpu_new(uint32_t stack_size) {
    CPU *c = calloc(1, sizeof *c);
    stack_size = (stack_size + 0xFFFFu) & ~0xFFFFu;
    big_lock();
    uint32_t lo = dr_guest_alloc(main_cpu->g, stack_size, PROT_READ | PROT_WRITE);
    big_unlock();
    if (!lo) { fprintf(stderr, "  [aot] no room for a guest stack of %u bytes\n", stack_size); abort(); }
    c->stack_lo = lo; c->stack_hi = lo + stack_size;
    c->esp = c->stack_hi - 64;
    c->fcw = 0x37F; c->mxcsr = 0x1F80;
    return c;
}
// This host thread becomes a guest thread with CPU c (or a new one): it gets a dr_cpu of its own, which is the
// main one's settings and no state.
void aot_thread_begin(CPU *c) {
    if (!c) c = cpu_new(1u << 20);
    dr_cpu *d = malloc(sizeof *d);
    *d = *main_cpu;
    memset(d->regs, 0, sizeof d->regs);
    d->regs[DR_ESP] = c->esp;
    d->nest_depth = 0; d->tail_jump = false; d->exited = false; d->threw = false; d->faulted = false;
    d->magic_ret = MAGIC_RET;
    Gt = c; Dt = d;
}
// Call a guest function on this thread, which stands in the host (a new thread's start routine, a block handed
// to a queue). cdecl; returns eax.
uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args) {
    if (Gt != c) aot_thread_begin(c);
    uint32_t sp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) PUSH(args[i]);
    PUSH(0xDEAD0000u);
    out();
    D->eip = addr;
    dr_status st = run();
    if (st != DR_HALT_RET)
        fprintf(stderr, "  [aot] a guest call to 0x%08x (%s) on a thread of its own ended: %s\n", addr, rt_func_name(addr), dr_status_str(st));
    uint32_t r = c->eax;
    c->esp = sp;
    return r;
}
// The same on a thread that is in the middle of something (the main thread in its run loop, called by a queue):
// below where it stands, and nothing it had is disturbed.
uint32_t guest_call_here(uint32_t addr, int nargs, const uint32_t *args) {
    if (!Dt) aot_thread_begin(NULL);
    uint32_t eax = 0;
    // The stack to go below is where the translated code stands NOW. A shim of ours is called straight from
    // translated code, with no hand-over to the bridge, so the bridge's copy of esp is from the last time there was
    // one, further up: a helper run "below" that one ran over the frames of the functions still in progress (the
    // copy helper of a block given to dispatch_async wrote over its caller's locals).
    out();
    dr_cpu fake; fake.fail_eip = 0; fake.fault_addr = 0;   // "not this thread's": takes the stack below
    dr_cpu_call_nested(&fake, addr, args, nargs, 0, &eax);
    return eax;
}
int aot_is_main_thread(void) { return Gt == &main_G; }
uint32_t aot_stack_lo(void) { return Gt ? G.stack_lo : 0; }
uint32_t aot_stack_hi(void) { return Gt ? G.stack_hi : 0; }
// For threads.c: memory of the guest's, taken under the bridge's lock.
uint32_t aot_malloc(uint32_t n) { big_lock(); uint32_t p = dr_guest_malloc(main_cpu->g, n); big_unlock(); return p; }
void aot_free(uint32_t p) { big_lock(); dr_guest_free(main_cpu->g, p); big_unlock(); }

// The interpreter skips a function by address as it is entered. Translated callers call it by name, so a function
// to neutralise is listed in hle.txt instead and answered by an H_ function of the project (see game.c).
void dr_cpu_skip_function(dr_cpu *cpu, uint32_t addr) {
    (void)cpu;
    fprintf(stderr, "  [aot] --skip-func 0x%08x: with translated code this is hle.txt's job, not the driver's\n", addr);
}

const char *dr_status_str(dr_status s) {
    switch (s) {
        case DR_OK: return "ok";
        case DR_HALT_RET: return "retorno limpio";
        case DR_ERR_OPCODE: return "codigo no traducido";
        case DR_ERR_MEM: return "acceso a memoria invalido";
        case DR_ERR_LIMIT: return "limite superado";
        case DR_ERR_IMPORT: return "import sin implementacion";
        case DR_ERR_FAULT: return "el invitado toco memoria sin mapear";
        case DR_HALT_EXIT: return "el invitado termino (exit)";
        case DR_ERR_NULLCALL: return "el invitado llamo a un puntero nulo";
        case DR_ERR_THROW: return "el invitado lanzo una excepcion de C++ (no se propaga)";
    }
    return "?";
}

void dr_cpu_backtrace(const dr_cpu *cpu, int max) {
    uint32_t ebp = cpu->regs[DR_EBP], esp = cpu->regs[DR_ESP];
    fprintf(stderr, "  [pila] rastro del invitado (cadena de ebp, aproximado; esp=0x%08x ebp=0x%08x), en %s\n", esp, ebp, rt_func_name(G.fn));
    fprintf(stderr, "  [pila]   0x%08x  <- [esp], quien llamo (%s)\n", dr_r32(cpu->g, esp), rt_func_name(dr_r32(cpu->g, esp)));
    for (int i = 0; i < max && ebp > 0x1000 && ebp < 0xFE000000u; i++) {
        uint32_t ret = dr_r32(cpu->g, ebp + 4), next = dr_r32(cpu->g, ebp);
        fprintf(stderr, "  [pila]   0x%08x  %s\n", ret, rt_func_name(ret));
        if (next <= ebp) break;
        ebp = next;
    }
}
void dr_cpu_dump(const dr_cpu *cpu) {
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x  en %s\n",
            cpu->regs[0], cpu->regs[1], cpu->regs[2], cpu->regs[3], cpu->regs[4], cpu->regs[5], cpu->regs[6], cpu->regs[7],
            rt_func_name(G.fn));
}
void dr_cpu_profile_report(const dr_cpu *cpu, uint32_t top) {
    (void)cpu; (void)top;
    printf("aot       : %llu imports atendidos por el puente, %llu llamadas indirectas\n",
           (unsigned long long)nimports, (unsigned long long)ncalls_ind);
}
void dr_cpu_ring_dump(const dr_cpu *cpu, uint32_t n) { (void)cpu; (void)n; }
