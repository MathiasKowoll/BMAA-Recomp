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
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
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
static void show_more(int count);
static void big_lock_report(void);
static void prof_register(CPU *c);
static int prof_index(CPU *c);
static const char *volatile prof_in[64];
static void import_stat(const char *name, uint64_t ns);
static void on_usr1(int sig);
static void on_term(int sig) {
    (void)sig;
    extern void dr_bridge_report_unimplemented(uint32_t top);
    signal(SIGALRM, SIG_DFL); alarm(2);   // the report prints from a signal handler: if another thread holds stdout, die anyway
    dr_bridge_report_unimplemented(80);
    big_lock_report();
    fflush(stdout);
    _exit(143);
}
static int watching_some;
static uint32_t *ip_calls;
// A process that ends without the game having asked (no "_exit" line): say from where, on the host's side.
static void skip_setup(void);
static void nap_setup(void);
static void scale_setup(void);
static void at_exit_note(void) {
    void *bt[24]; int n = backtrace(bt, 24);
    fprintf(stderr, "  [aot] the process is exiting; the host's stack:\n");
    backtrace_symbols_fd(bt, n, 2);
}
static void ip_setup(void);
static void watch_some(void) {
    ip_setup();
    const char *w = getenv("DR_WATCH");
    for (char *e; w && *w; w = *e ? e + 1 : e) {
        uint32_t a = (uint32_t)strtoul(w, &e, 16);
        if (e == w) break;
        rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7)); watching_some = 1;
    }
}
static void show(uint32_t p) {
    if (p < 0x1000 || p >= 0xE0000000u) return;
    if (p < 0x60000000u && !(p < 0x04000000u || (p >= 0x1f000000u && p < 0x20000000u) || (p >= 0x40000000u && p < 0x51000000u))) return;
    if (p >= 0xA0000000u) return;   // past the guest heap: a float's bits, not a pointer
    fprintf(stderr, "        %08x:", p);
    for (int k = 0; k < 32; k++) fprintf(stderr, " %02x", rd8(p + k));
    fprintf(stderr, "  |");
    for (int k = 0; k < 32; k++) { uint8_t ch = rd8(p + k); fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
    fprintf(stderr, "|\n");
    // an engine string object {data, count, capacity}: the text its data points at
    {
        uint32_t d = rd32(p), n = rd32(p + 4), cap = rd32(p + 8);
        if (d >= 0x1000 && d < 0xA0000000u && n > 0 && n <= cap && cap < 4096 && rd16(d) >= 32 && rd16(d) < 127) {
            fprintf(stderr, "        -> utf16[%u]: \"", n);
            for (uint32_t k = 0; k < n && k < 300; k++) { uint32_t ch = rd16(d + 2u * k); if (!ch) break; fputc(ch >= 32 && ch < 127 ? (int)ch : '.', stderr); }
            fprintf(stderr, "\"\n");
        }
    }
    // text in 16-bit characters (an engine's TCHAR): the whole string
    if (rd16(p) >= 32 && rd16(p) < 127 && rd16(p + 2) >= 32 && rd16(p + 2) < 127 && rd16(p + 4) >= 32 && rd16(p + 4) < 127) {
        fprintf(stderr, "        utf16: \"");
        for (int k = 0; k < 500; k++) { uint32_t ch = rd16(p + 2u * k); if (!ch) break; fputc(ch >= 32 && ch < 127 ? (int)ch : '.', stderr); }
        fprintf(stderr, "\"\n");
    }
    // text in 32-bit characters: the whole string
    if (rd32(p) >= 32 && rd32(p) < 127 && rd32(p + 4) >= 32 && rd32(p + 4) < 127) {
        fprintf(stderr, "        wide: \"");
        for (int k = 0; k < 300; k++) { uint32_t ch = rd32(p + 4u * k); if (!ch) break; fputc(ch >= 32 && ch < 127 ? (int)ch : '.', stderr); }
        fprintf(stderr, "\"\n");
    }
}
// ---- BMAA_INTERP=lo-hi[,lo-hi...]: run the game's functions that start in those ranges with DecompRecomp's
// interpreter instead of their translation (and whatever they call, until they return). Everything else stays
// translated. It is for finding a fault of the translation: if a thing goes right with a range interpreted, the
// translation of something in that range is wrong, and halving the range finds the function.
// The interpreter is x86.c built a second time under other names (interp.o); its imports come back here and are
// answered by the same shims the translated code calls.
dr_status ip_cpu_run(dr_cpu *cpu); void ip_cpu_init(dr_cpu *cpu, dr_guest *g);
static dr_cpu *main_cpu; static _Thread_local CPU *Gt; static void stop(dr_status s) __attribute__((noreturn));   // defined below
static const GFunc *func_at(uint32_t a);
static uint32_t ip_lo[16], ip_hi[16]; static int ip_n;
static _Thread_local dr_cpu *ip_cpu[8]; static _Thread_local int ip_depth;
static unsigned long ip_runs;
static void ip_to_interp(dr_cpu *d, CPU *c) {
    d->regs[DR_EAX] = c->eax; d->regs[DR_ECX] = c->ecx; d->regs[DR_EDX] = c->edx; d->regs[DR_EBX] = c->ebx;
    d->regs[DR_ESP] = c->esp; d->regs[DR_EBP] = c->ebp; d->regs[DR_ESI] = c->esi; d->regs[DR_EDI] = c->edi;
    memcpy(d->xmm, c->xmm, sizeof d->xmm); memcpy(d->st, c->st, sizeof d->st); d->fpu_top = c->top; d->fpu_cw = c->fcw; d->mxcsr = c->mxcsr;
}
static void ip_from_interp(CPU *c, dr_cpu *d) {
    c->eax = d->regs[DR_EAX]; c->ecx = d->regs[DR_ECX]; c->edx = d->regs[DR_EDX]; c->ebx = d->regs[DR_EBX];
    c->esp = d->regs[DR_ESP]; c->ebp = d->regs[DR_EBP]; c->esi = d->regs[DR_ESI]; c->edi = d->regs[DR_EDI];
    memcpy(c->xmm, d->xmm, sizeof d->xmm); memcpy(c->st, d->st, sizeof d->st); c->top = d->fpu_top; c->fcw = d->fpu_cw;
    if (c->mxcsr != d->mxcsr) cpu_set_mxcsr(c, d->mxcsr);
}
static int stub_of_thunk(uint32_t t);
static bool ip_import(dr_cpu *d, uint32_t thunk, void *user) {
    CPU *c = Gt; (void)user;
    int k = stub_of_thunk(thunk);
    if (k < 0 || !gstubs[k].fn) { fprintf(stderr, "  [interp] import at thunk %08x has no shim here\n", thunk); return false; }
    ip_from_interp(c, d);
    gstubs[k].fn(c);
    ip_to_interp(d, c);
    return true;
}
static void ip_setup(void) {
    atexit(at_exit_note);
    skip_setup(); nap_setup(); scale_setup();
    const char *w = getenv("BMAA_INTERP");
    if (w && getenv("BMAA_INTERP_STATS")) ip_calls = calloc((size_t)ngfuncs, sizeof *ip_calls);
    for (char *e; w && *w && ip_n < 16; w = *e ? e + 1 : e) {
        uint32_t lo = (uint32_t)strtoul(w, &e, 16), hi = lo;
        if (e == w) break;
        if (*e == '-') hi = (uint32_t)strtoul(e + 1, &e, 16);
        ip_lo[ip_n] = lo; ip_hi[ip_n] = hi; ip_n++;
        int nf = 0;
        for (int i = 0; i < ngfuncs; i++) if (gfuncs[i].addr >= lo && gfuncs[i].addr <= hi) { uint32_t a = gfuncs[i].addr; rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7)); nf++; }
        fprintf(stderr, "  [interp] %d functions from %08x to %08x run in the interpreter\n", nf, lo, hi);
    }
}
static int ip_wanted(uint32_t a) { for (int k = 0; k < ip_n; k++) if (a >= ip_lo[k] && a <= ip_hi[k]) return 1; return 0; }
// BMAA_INTERP_STATS=1: how often each function was entered from interpreted code (the interpreted ones and the
// translated ones they call), printed every 20 seconds: which part of the chosen code really runs.
static double ip_calls_t;
static void ip_calls_report(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = (double)ts.tv_sec;
    if (!ip_calls_t) ip_calls_t = t;
    if (t - ip_calls_t < 20) return;
    ip_calls_t = t;
    fprintf(stderr, "  [interp] entries in the last 20 s:");
    for (int i = 0; i < ngfuncs; i++) if (ip_calls[i]) { fprintf(stderr, " %x%s:%u", gfuncs[i].addr, ip_wanted(gfuncs[i].addr) ? "" : "(t)", ip_calls[i]); ip_calls[i] = 0; }
    fprintf(stderr, "\n");
}
static int ip_run(CPU *c, uint32_t a) {
    if (ip_depth >= 8) return 0;
    if (ip_calls && ip_depth == 0) { const GFunc *f0 = func_at(a); if (f0) ip_calls[f0 - gfuncs]++; ip_calls_report(); }
    dr_cpu *d = ip_cpu[ip_depth];
    if (!d) { d = ip_cpu[ip_depth] = calloc(1, sizeof *d); ip_cpu_init(d, main_cpu->g); d->on_import = ip_import; d->ilimit = ~0ull; }
    uint32_t ret = rd32(c->esp);
    ip_to_interp(d, c);
    wr32(c->esp, d->magic_ret);                 // the function returns to the interpreter's "done" address
    d->eip = a; d->exited = d->threw = d->faulted = false; d->tail_jump = false; d->nest_depth = 0;
    ip_depth++;
    // One instruction at a time, so that a call (or a jump) to a function outside the ranges goes to its translation:
    // only the chosen functions are interpreted, and the interpreter need only know their instructions.
    dr_status st;
    for (;;) {
        d->ilimit = d->icount + 1;
        st = ip_cpu_run(d);
        if (st != DR_ERR_LIMIT) break;
        uint32_t at = d->eip;
        const GFunc *f = func_at(at);
        if (ip_wanted(at)) { if (f && ip_calls) ip_calls[f - gfuncs]++; continue; }
        if (!f) continue;                        // inside a function (a jump table target, a block): carry on
        if (ip_calls) ip_calls[f - gfuncs]++;
        ip_from_interp(c, d);
        uint32_t back = rd32(c->esp);
        f->fn(c);                                // the translated function; it pops its return address
        ip_to_interp(d, c);
        d->eip = back;
        if (back == d->magic_ret) { st = DR_HALT_RET; break; }
    }
    ip_depth--;
    ip_from_interp(c, d);
    wr32(c->esp - 4, ret);                       // as the translated function would have left it
    if (st != DR_HALT_RET) { fprintf(stderr, "  [interp] %08x (%s) ended: %s at %08x\n", a, rt_func_name(a), dr_status_str(st), d->eip); stop(st); }
    if (ip_runs++ == 0) fprintf(stderr, "  [interp] the first function ran in the interpreter: %s\n", rt_func_name(a));
    return 1;
}
// BMAA_SKIP=addr[,addr]: those functions return at once, doing nothing (plain "ret" functions whose result is not
// used: the caller cleans the stack). An experiment's tool: what the game looks like without one step.
static uint32_t skip_at[8]; static int skip_n = -1;
static void skip_setup(void) {
    skip_n = 0;
    const char *w = getenv("BMAA_SKIP");
    for (char *e; w && *w && skip_n < 8; w = *e ? e + 1 : e) {
        uint32_t a = (uint32_t)strtoul(w, &e, 16);
        if (e == w) break;
        skip_at[skip_n++] = a; rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7));
        fprintf(stderr, "  [aot] %08x (%s) is skipped (BMAA_SKIP)\n", a, rt_func_name(a));
    }
}
// BMAA_SLEEP_AT=addr:ms[,addr:ms]: the thread that enters that function first waits so many milliseconds (an
// experiment: does something another thread is still doing get finished in that time?)
static uint32_t nap_at[8]; static int nap_ms[8], nap_n;
static void nap_setup(void) {
    const char *w = getenv("BMAA_SLEEP_AT");
    for (char *e; w && *w && nap_n < 8; w = *e ? e + 1 : e) {
        uint32_t a = (uint32_t)strtoul(w, &e, 16);
        if (e == w || *e != ':') break;
        nap_ms[nap_n] = (int)strtol(e + 1, &e, 10); nap_at[nap_n++] = a; rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7));
        fprintf(stderr, "  [aot] %08x (%s) waits %d ms on entry (BMAA_SLEEP_AT)\n", a, rt_func_name(a), nap_ms[nap_n - 1]);
    }
}
// BMAA_ARG_SCALE=addr:slot:factor: the float argument in that slot (0 = first) of that function is multiplied on
// entry (an experiment: one part of the game told a different time step from the rest)
static uint32_t scale_at; static int scale_slot = -1; static float scale_by;
static void scale_setup(void) {
    const char *w = getenv("BMAA_ARG_SCALE"); char *e;
    if (!w) return;
    scale_at = (uint32_t)strtoul(w, &e, 16); if (*e != ':') return;
    scale_slot = (int)strtol(e + 1, &e, 10); if (*e != ':') { scale_slot = -1; return; }
    scale_by = strtof(e + 1, NULL);
    rt_watch[scale_at >> 3] |= (uint8_t)(1u << (scale_at & 7));
    fprintf(stderr, "  [aot] %08x: float argument %d times %g (BMAA_ARG_SCALE)\n", scale_at, scale_slot, (double)scale_by);
}
int rt_on_watch(CPU *c, uint32_t a) {
    if (scale_slot >= 0 && a == scale_at) { uint32_t p = c->esp + 4 + 4 * (uint32_t)scale_slot; float v; uint32_t b = rd32(p); memcpy(&v, &b, 4); v *= scale_by; memcpy(&b, &v, 4); wr32(p, b); return 0; }
    for (int k = 0; k < nap_n; k++) if (nap_at[k] == a) { usleep((useconds_t)nap_ms[k] * 1000); return 0; }
    for (int k = 0; k < skip_n; k++) if (skip_at[k] == a) { c->esp += 4; return 1; }
    if (ip_n && ip_wanted(a)) return ip_run(c, a);

    if (watching_some && rt_watch[0] != 0xFF) {
        // DR_WATCH_FROM=lo-hi: only the entries whose caller returns into that range (one function's calls)
        static uint32_t from_lo = 1, from_hi;
        if (from_lo == 1) {
            const char *e = getenv("DR_WATCH_FROM"); char *dash = NULL;
            from_lo = e ? (uint32_t)strtoul(e, &dash, 16) : 0;
            from_hi = (dash && *dash == '-') ? (uint32_t)strtoul(dash + 1, NULL, 16) : from_lo;
        }
        if (from_lo && (rd32(c->esp) < from_lo || rd32(c->esp) > from_hi)) return 0;
        static struct { uint32_t a; int n; } seen[64];
        static int watch_max;
        if (!watch_max) watch_max = getenv("DR_WATCH_MAX") ? atoi(getenv("DR_WATCH_MAX")) : 40;
        for (int k = 0; k < 64; k++) {
            if (seen[k].a == a || !seen[k].a) { seen[k].a = a; if (++seen[k].n > watch_max) return 0; break; }
        }
        fprintf(stderr, "  [watch] %08x from %08x (%s)  a=%08x,%08x,%08x,%08x  ecx=%08x\n", a, rd32(c->esp), rt_func_name(rd32(c->esp)),
                rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), rd32(c->esp + 16), c->ecx);
        for (int k = 1; k <= 4; k++) show(rd32(c->esp + 4 * k));
        // DR_WATCH_SET=n: the first argument is an engine set/map whose elements are n bytes and start with a string
        // object; its keys, once (to see what a lookup is looked up against)
        if (getenv("DR_WATCH_SET")) {
            static int done; uint32_t m = rd32(c->esp + 4), sz = (uint32_t)atoi(getenv("DR_WATCH_SET"));
            if (!done++ && sz) {
                uint32_t data = rd32(m), num = rd32(m + 4);
                fprintf(stderr, "  [set] %08x: %u elements at %08x, hash %08x size %u\n", m, num, data, rd32(m + 0x1c), rd32(m + 0x20));
                for (uint32_t i = 0; i < num && i < 400; i++) {
                    uint32_t e = data + i * sz, d = rd32(e), n = rd32(e + 4);
                    fprintf(stderr, "    %3u: ", i);
                    for (uint32_t k = 0; d && k < n && k < 80; k++) { uint32_t ch = rd16(d + 2u * k); if (!ch) break; fputc(ch >= 32 && ch < 127 ? (int)ch : '.', stderr); }
                    fprintf(stderr, "  [next %d hashidx %d]\n", (int)rd32(e + sz - 8), (int)rd32(e + sz - 4));
                }
            }
        }
        if (getenv("DR_WATCH_LAST")) show_last();   // and what this thread was last told by the bridge
        if (getenv("DR_WATCH_MORE")) show_more(atoi(getenv("DR_WATCH_MORE")));   // or the longer memory of it
        return 0;
    }
    fprintf(stderr, "  [fn] %08x %s  ret=%08x a=%08x,%08x,%08x  eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x esp=%08x\n",
            a, rt_func_name(a), rd32(c->esp), rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12),
            c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->esp);
    return 0;
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
// How long threads wait for the lock, and what the thread holding it was doing (DR_LOCK_STATS, printed when a run
// is stopped): the engine thread spent its time here, not in its own code.
static const char *volatile big_holder_what;
static _Thread_local const char *cur_import_name;   // set by unimpl_import for the import about to run
static struct { const char *what; uint64_t ns, n; } big_waits[64];
static uint64_t big_wait_total;
static int lock_stats = -1;
static void big_lock(void) {
    pthread_once(&big_once, big_make);
    if (lock_stats < 0) lock_stats = getenv("DR_LOCK_STATS") != NULL;
    // (with statistics: a try that succeeds HAS taken the lock; taking it again below left it held for ever)
    if (lock_stats && big_depth == 0 && pthread_mutex_trylock(&big) == 0) { big_depth++; return; }
    if (lock_stats && big_depth == 0) {
        const char *what = big_holder_what;
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        pthread_mutex_lock(&big);
        uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        big_wait_total += dt;
        for (int k = 0; k < 64; k++)
            if (!big_waits[k].what || big_waits[k].what == what) { big_waits[k].what = what ? what : "(no import)"; big_waits[k].ns += dt; big_waits[k].n++; break; }
    } else {
        pthread_mutex_lock(&big);
    }
    big_depth++;
}
// DR_IMPORT_STATS: calls and time per import (printed when a run is stopped), and the time since start, to see
// whether the bridge or the translated code is where a slow stretch goes. Time includes guest code the bridge
// runs for the import (callbacks), so a host call that calls back counts its callbacks too.
static struct { const char *name; uint64_t ns, n; } istat[4096];
static pthread_mutex_t istat_mx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t istat_t0;
static void import_stat(const char *name, uint64_t ns) {
    pthread_mutex_lock(&istat_mx);
    if (!istat_t0) istat_t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint32_t h = 2166136261u; for (const char *q = name; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
    for (uint32_t i = h & 4095, k = 0; k < 4096; i = (i + 1) & 4095, k++)
        if (!istat[i].name || istat[i].name == name) { istat[i].name = name; istat[i].ns += ns; istat[i].n++; break; }
    pthread_mutex_unlock(&istat_mx);
}
static void import_report(void) {
    if (!istat_t0) return;
    uint64_t all = 0, calls = 0;
    for (int i = 0; i < 4096; i++) { all += istat[i].ns; calls += istat[i].n; }
    fprintf(stderr, "  [imports] %llu calls, %.1f s inside them (all threads), over %.1f s of run:\n", (unsigned long long)calls, all / 1e9,
            (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - istat_t0) / 1e9);
    for (int r = 0; r < 25; r++) {
        int best = -1;
        for (int i = 0; i < 4096; i++) if (istat[i].ns && (best < 0 || istat[i].ns > istat[best].ns)) best = i;
        if (best < 0) break;
        fprintf(stderr, "    %8.2f s  %10llu calls  %7.2f us each  %s\n", istat[best].ns / 1e9, (unsigned long long)istat[best].n,
                istat[best].ns / 1e3 / istat[best].n, istat[best].name);
        istat[best].ns = 0;
    }
}
static void prof_report(void);
void heapdbg_report(void);
void objc_sel_report(void);
static void big_lock_report(void) {
    import_report();
    objc_sel_report();
    heapdbg_report();
    prof_report();
    if (!lock_stats) return;
    fprintf(stderr, "  [lock] %.1f s waited for the bridge's lock in all; held by a thread in:\n", big_wait_total / 1e9);
    for (int r = 0; r < 15; r++) {
        int best = -1;
        for (int k = 0; k < 64; k++) if (big_waits[k].ns && (best < 0 || big_waits[k].ns > big_waits[best].ns)) best = k;
        if (best < 0) break;
        fprintf(stderr, "    %8.2f s  %8llu waits  %s\n", big_waits[best].ns / 1e9, (unsigned long long)big_waits[best].n, big_waits[best].what);
        big_waits[best].ns = 0;
    }
}
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
    uint32_t sp_before = c->esp, ret_before = rd32(c->esp);
    big_lock();
    big_holder_what = cur_import_name;
    bool ok = D->on_import && D->on_import(D, thunk, D->user);
    big_holder_what = NULL;
    big_unlock();
    if (!ok) stop(D->exited ? DR_HALT_EXIT : D->threw ? DR_ERR_THROW : DR_ERR_IMPORT);
    in();
    // An import leaves esp where it found it (the caller pops the return address), or one slot up when the callee
    // pops a hidden struct pointer. Anything else, or a return address that changed under it, is the bridge (or a
    // callback it made) writing over the caller's frame: said here, where it happens, instead of at a return later.
    if (!D->tail_jump && ((c->esp != sp_before && c->esp != sp_before + 4) || rd32(sp_before) != ret_before)) {
        static int said;
        if (said++ < 20)
            fprintf(stderr, "  [aot] import at thunk %08x (from %s): esp %08x -> %08x, return address %08x -> %08x\n", thunk,
                    rt_func_name(ret_before), sp_before, c->esp, ret_before, rd32(sp_before));
    }
    if (D->tail_jump) {   // the bridge found guest code to run in its place: same stack, same return address
        D->tail_jump = false;
        enter(c, D->tail_target);
        c->esp -= 4;      // that function popped the address; the caller of an import pops it too
    }
}

// Control goes to guest address t with the return address on the stack; when this returns, the address is popped.
// Thread-local variables (`__thread`). The compiler reaches one through its descriptor: three words (a function,
// a key, an offset), with the descriptor's address in eax and `call [eax]`; the function, dyld's __tlv_bootstrap
// until first use, answers the variable's address in eax and keeps every other register. Here each thread gets its
// own copy of the image's thread-local sections (initial values, then the zero-filled part) in guest memory.
uint32_t aot_malloc(uint32_t n);
static _Thread_local uint32_t tlv_block;
static int tlv_get(CPU *c, uint32_t t) {
    static int is_tlv = -1; static uint32_t tlv_marker, tmpl, tmpl_size, data_size;
    extern dr_objc *dr_objc_actual(void);
    if (is_tlv < 0) {
        const dr_image *img = dr_objc_actual()->img;
        const char *n = dr_macho32_unresolved_name(img, t);
        if (!n || strcmp(n, "__tlv_bootstrap")) return 0;
        const dr_section *d = dr_macho32_section(img, "__DATA", "__thread_data"), *z = dr_macho32_section(img, "__DATA", "__thread_bss");
        tmpl = d ? d->addr : z ? z->addr : 0;
        data_size = d ? d->size : 0;
        tmpl_size = z ? z->addr + z->size - tmpl : data_size;
        tlv_marker = t; is_tlv = tmpl_size != 0;
    }
    if (!is_tlv || t != tlv_marker) return 0;
    if (!tlv_block) {
        tlv_block = aot_malloc(tmpl_size);
        memset(G2H(tlv_block), 0, tmpl_size);
        if (data_size) memcpy(G2H(tlv_block), G2H(tmpl), data_size);
    }
    c->eax = tlv_block + rd32(c->eax + 8);
    c->esp += 4;   // the return address the caller pushed
    return 1;
}
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
    if (t < D->null_floor) {
        fprintf(stderr, "  [aot] call through a null pointer (0x%x), at %08x in %s; eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n",
                t, rd32(c->esp), rt_func_name(rd32(c->esp)), c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi);
        {   // the frames above and the arguments this function was given: a null object is often an argument's doing
            uint32_t ebp = c->ebp;
            fprintf(stderr, "  [aot]   args %08x %08x %08x %08x %08x; frames:", rd32(ebp + 8), rd32(ebp + 12), rd32(ebp + 16), rd32(ebp + 20), rd32(ebp + 24));
            for (int k = 0; k < 12 && ebp > 0x10000 && ebp < 0xA0000000u; k++) {
                fprintf(stderr, " %08x(%s)", rd32(ebp + 4), rt_func_name(rd32(ebp + 4)));
                ebp = rd32(ebp);
            }
            fprintf(stderr, "\n");
        }
        show_last();
        stop(DR_ERR_NULLCALL);
    }
    if (t >= DR_UNRESOLVED_BASE && tlv_get(c, t)) return;
    fprintf(stderr, "  [aot] call to 0x%08x, which is no function of the translation (from %s)\n", t, rt_func_name(c->fn));
    {   // who made the call (the return address it pushed) and the frames above, and the registers it had
        char b[600]; int n = snprintf(b, sizeof b, "  [aot]   called at %08x in %s; eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x; frames:",
                                      rd32(c->esp), rt_func_name(rd32(c->esp)), c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi);
        uint32_t ebp = c->ebp;
        for (int k = 0; k < 10 && n < 520 && ebp > 0x10000 && ebp < 0xA0000000u; k++) {
            n += snprintf(b + n, sizeof b - (size_t)n, " %s", rt_func_name(rd32(ebp + 4)));
            uint32_t up = rd32(ebp); if (up <= ebp) break; ebp = up;
        }
        fprintf(stderr, "%s\n", b);
    }
    if (t >= DR_UNRESOLVED_BASE) fprintf(stderr, "  [aot]   that is the marker of a data symbol nobody resolved\n");
    stop(DR_ERR_OPCODE);
}
// CFBundleGetFunctionPointerForName(bundle, CFSTR("name")): a system function looked up by name at run time. The
// game does this for `free` itself (in System.framework), so with no answer nothing it allocated was ever released.
// A function the game also imports the ordinary way has a stub, and the stub's address is a pointer the game can
// call; anything else goes on to the bridge.
void unimpl_import(CPU *c, const char *name);
void I__CFBundleGetFunctionPointerForName(CPU *c) {
    extern dr_objc *dr_objc_actual(void);
    uint32_t s = ARG(1); char name[130] = "_";
    if (s && s < DR_OBJC_HANDLE_BASE) {          // a constant string of the game's: { isa, flags, bytes, length }
        uint32_t bytes = rd32(s + 8), len = rd32(s + 12);
        if (bytes && len < 120) { memcpy(name + 1, G2H(bytes), len); name[1 + len] = 0; }
    } else if (s) {
        big_lock(); void *h = dr_objc_object_for(dr_objc_actual(), s); big_unlock();
        if (h) CFStringGetCString((CFStringRef)h, name + 1, 120, kCFStringEncodingUTF8);
    }
    if (name[1])
        for (int k = 0; k < ngstubs; k++)
            if (!strcmp(gstubs[k].name, name)) { c->eax = gstubs[k].addr; return; }
    fprintf(stderr, "  [aot] CFBundleGetFunctionPointerForName(\"%s\"): not a function the game imports; left to the bridge\n", name + 1);
    unimpl_import(c, "_CFBundleGetFunctionPointerForName");
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

// A longer memory of the calls that say something (not the allocator, the string compares or the memory copies):
// DR_LAST_MORE=n prints the last n of them when the game throws, with the text a first argument points at.
#define NMORE 4096
static _Thread_local struct { const char *name; uint32_t a[3], from, eax; } more[NMORE];
static _Thread_local unsigned nmore;
static int dull(const char *n) {
    static const char *const names[] = { "_wcscmp", "_malloc", "_free", "__Znwm", "__ZdlPv", "__Znam", "__ZdaPv", "___bzero", "_memcpy",
        "_memmove", "_memset", "_strlen", "_memcmp", "_realloc", "_calloc", "_strcmp", "_wcslen", "_pthread_mutex_lock",
        "_pthread_mutex_unlock", "_pthread_getspecific", "_pthread_self", "_OSAtomicAdd32", "_mach_absolute_time", NULL };
    for (int k = 0; names[k]; k++) if (!strcmp(n, names[k])) return 1;
    return !strncmp(n, "__ZSt", 5) || !strncmp(n, "__ZNS", 5) || !strncmp(n, "_OSAtomic", 9) || !strncmp(n, "_OSSpin", 7) ||
           !strncmp(n, "_CFString", 9) || !strncmp(n, "_CFRelease", 10) || !strncmp(n, "_CFRetain", 9) ||
           !strncmp(n, "_CFAllocator", 12) || !strncmp(n, "_vsnprintf", 10) || !strncmp(n, "_strto", 6) ||
           !strncmp(n, "_objc_msgSend", 13) || !strncmp(n, "_pthread_", 9) || !strcmp(n, "___tolower") ||
           !strcmp(n, "_posix_memalign") || !strcmp(n, "_malloc_size") || !strcmp(n, "_FSFindFolder");
}
static void show_more(int count) {
    for (unsigned k = nmore > (unsigned)count ? nmore - (unsigned)count : 0; k < nmore; k++) {
        if (nmore - k > NMORE) continue;
        uint32_t p = more[k % NMORE].a[0];
        char text[140] = "";
        if ((p >= 0x1000 && p < 0x04000000u) || (p >= 0x1f800000u && p < 0x20000000u) || (p >= 0x40000000u && p < 0x9FFFFF00u)) {
            if (rd8(p) >= 32 && rd8(p) < 127 && rd8(p + 1) >= 32 && rd8(p + 1) < 127 && rd8(p + 2) >= 32 && rd8(p + 2) < 127) {
                int j = 0; text[j++] = ' '; text[j++] = '"';
                for (int i = 0; i < 120 && rd8(p + i) >= 32 && rd8(p + i) < 127; i++) text[j++] = (char)rd8(p + i);
                text[j++] = '"'; text[j] = 0;
            } else if (rd16(p) >= 32 && rd16(p) < 127 && rd16(p + 2) >= 32 && rd16(p + 2) < 127) {
                int j = 0; text[j++] = ' '; text[j++] = 'L'; text[j++] = '"';
                for (int i = 0; i < 120 && rd16(p + 2 * i) >= 32 && rd16(p + 2 * i) < 127; i++) text[j++] = (char)rd16(p + 2 * i);
                text[j++] = '"'; text[j] = 0;
            }
        }
        fprintf(stderr, "      %-40.60s a=%08x,%08x,%08x -> %08x  from %s%s\n", more[k % NMORE].name, more[k % NMORE].a[0],
                more[k % NMORE].a[1], more[k % NMORE].a[2], more[k % NMORE].eax, rt_func_name(more[k % NMORE].from), text);
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
    if (!strcmp(name, "___cxa_throw")) {   // what it was told before it threw
        fprintf(stderr, "  [aot] the game throws a C++ exception; this thread's last calls into the bridge:\n");
        show_last();
        if (getenv("DR_LAST_MORE")) {
            fprintf(stderr, "  [aot] and the last ones that are not the allocator or string routines:\n");
            show_more(atoi(getenv("DR_LAST_MORE")));
        }
        // DR_THROW_TEXT=addr,addr: the text at those addresses when it throws (an engine's error message buffer),
        // read as 32-bit, 16-bit and 8-bit characters
        for (const char *w = getenv("DR_THROW_TEXT"); w && *w;) {
            char *e; uint32_t a = (uint32_t)strtoul(w, &e, 16);
            if (e == w) break;
            for (int ancho = 4; ancho >= 1; ancho >>= 1) {
                fprintf(stderr, "  [aot] text at %08x (%d-byte chars): \"", a, ancho);
                for (int k = 0; k < 400; k++) {
                    uint32_t ch = ancho == 4 ? rd32(a + 4u * k) : ancho == 2 ? rd16(a + 2u * k) : rd8(a + k);
                    if (!ch) break;
                    fputc(ch >= 32 && ch < 127 ? (int)ch : '.', stderr);
                }
                fprintf(stderr, "\"\n");
            }
            w = *e ? e + 1 : e;
        }
    }
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
                    // only a message that takes an argument has one to show: what follows a selector without a
                    // colon on the stack is whatever was there, and may be a handle to something that is no object
                    !strchr((const char *)G2H(sel), ':') ? "" :
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
    cur_import_name = name;
    static _Thread_local int pidx = -2;
    if (pidx == -2) pidx = getenv("DR_PROFILE") ? prof_index(c) : -1;
    if (pidx >= 0) prof_in[pidx] = name;
    static int istats = -1;
    if (istats < 0) istats = getenv("DR_IMPORT_STATS") != NULL;
    uint64_t t0 = istats ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    import(c, thunk);
    if (istats) import_stat(name, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
    if (pidx >= 0) prof_in[pidx] = NULL;
    last[slot].eax = c->eax;
    if (!dull(name)) {
        unsigned m = nmore++ % NMORE;
        more[m].name = name; more[m].a[0] = last[slot].a[0]; more[m].a[1] = last[slot].a[1]; more[m].a[2] = last[slot].a[2];
        more[m].from = last[slot].from; more[m].eax = c->eax;
    }
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
// MXCSR: rounding and the two "treat tiny numbers as zero" bits. The game sets flush-to-zero and denormals-are-zero
// (0x8040) once and switches the rounding mode around its floor conversions (0x3f80 / 0x1f80). On ARM both live in
// FPCR: RMode (bits 23:22) and FZ (bit 24), which flushes denormal inputs and results as x86 does with both bits.
// BMAA_NO_FTZ=1 leaves denormals alone, as before.
void cpu_set_mxcsr(CPU *c, uint32_t v) {
    static const uint64_t rmode[4] = {0, 2, 1, 3};             // x86 RC: nearest, down, up, zero -> ARM RMode: 00, 10, 01, 11
    static int no_ftz = -1; if (no_ftz < 0) no_ftz = getenv("BMAA_NO_FTZ") != NULL;
    c->mxcsr = v;
    uint64_t f = __builtin_arm_rsr64("FPCR");
    f = (f & ~((3ull << 22) | (1ull << 24))) | rmode[(v >> 13) & 3] << 22;
    if (!no_ftz && (v & 0x8040)) f |= 1ull << 24;
    __builtin_arm_wsr64("FPCR", f);
}
// CPUID. It used to say "leaf 1, no features at all": to the game a processor with no SSE and no SSE2, on which it
// takes whatever paths it keeps for such a machine. It is told what the translated code really offers: an Intel
// processor of the game's time with SSE, SSE2, SSE3 and SSSE3 (the instruction sets the translator knows), CMOV,
// CX8, FXSR and the time stamp counter; no MMX-only paths, no SSE4, no AVX, no hyper-threading leaf.
// BMAA_CPUID_NONE=1 gives the old answer back, for comparing.
void cpu_cpuid(CPU *c) {
    static int none = -1; if (none < 0) none = getenv("BMAA_CPUID_NONE") != NULL;
    uint32_t leaf = c->eax;
    if (none) { c->eax = 1; c->ebx = c->ecx = c->edx = 0; return; }
    c->eax = c->ebx = c->ecx = c->edx = 0;
    if (leaf == 0) { c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; }   // "GenuineIntel"
    else if (leaf == 1) {
        c->eax = 0x000206A7;                                   // family 6, a 2011 Core
        c->ebx = 0x00010800;                                   // one logical processor per package, 64-byte lines
        c->ecx = (1u << 0) | (1u << 9);                        // SSE3, SSSE3
        c->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 24) | (1u << 25) | (1u << 26);   // FPU TSC CX8 CMOV FXSR SSE SSE2
    } else if (leaf == 0x80000000u) c->eax = 0x80000000u;      // no extended leaves
}

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
    prof_register(&main_G);
    memset(&G, 0, sizeof G);
    G.fcw = 0x37F; G.mxcsr = 0x1F80;
    G.stack_hi = DR_STACK_TOP; G.stack_lo = DR_STACK_TOP - DR_STACK_SIZE;
    dr_host_call_enter = host_call_enter;
    dr_host_call_leave = host_call_leave;
    trace_window_hook = window;
    watch_some();
    if (game_init) game_init();
    { extern void dbg_init(void); dbg_init(); }   // kill -USR2: the windows and their views
    if (getenv("DR_VFS_VERBOSE")) { extern void dr_vfs_set_verbose(bool); dr_vfs_set_verbose(true); }   // every file call, by path
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
        { void *fr[40]; int nf = backtrace(fr, 40); backtrace_symbols_fd(fr, nf, 2); }   // where in the host
        _exit(139);
    }
    D->fault_addr = (uint32_t)(a - M);
    D->faulted = true;
    if (getenv("DR_FAULT_BT")) {   // the host's frames too: a guest address can be touched by the bridge, not the game
        void *fr[40]; int nf = backtrace(fr, 40);
        write(2, "  [aot] host frames at the fault:\n", 34);
        backtrace_symbols_fd(fr, nf, 2);
    }
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
    if (G.exc) eh_uncaught(&G, rt_func_name(D->eip));   // a C++ exception nothing in the guest caught
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
    // Never above where this thread's guest code stands: the caller picks a callback stack by nesting depth, and a
    // callback that the host makes while another callback is running (the window becoming key inside the game's
    // own makeKeyAndOrderFront:, itself inside runApp:) got the same stack as the one in progress and ran over it.
    {
        uint32_t here = cpu->regs[DR_ESP] ? cpu->regs[DR_ESP] : G.esp;
        if (here && here - 1024 < stack_top && here > 0x1000 + 0x10000) stack_top = here - 1024;
    }
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
// DR_PROFILE: a thread that looks every millisecond at which guest function each guest thread last entered, and
// counts it (printed when a run is stopped, per thread with more than a few samples). The function is the last
// one ENTERED: a sample inside a loop of a function that has called others since is credited to the last callee.
#define NPROF_CPU 64
static CPU *prof_cpus[NPROF_CPU];
static int nprof_cpus;
static struct { uint32_t fn; uint32_t n; } *prof_hist[NPROF_CPU];   // open addressing, 8192 slots each
static const char *volatile prof_in[64];   // the import the thread is inside, if any (64 = NPROF_CPU)
static struct { const char *name; uint32_t n; } prof_imp[NPROF_CPU][64];
static int prof_index(CPU *c) { for (int t = 0; t < nprof_cpus && t < NPROF_CPU; t++) if (prof_cpus[t] == c) return t; return -1; }
static uint32_t prof_total[NPROF_CPU];
static void prof_add(int t, uint32_t fn) {
    if (!prof_hist[t]) prof_hist[t] = calloc(8192, sizeof *prof_hist[t]);
    for (uint32_t i = (fn * 2654435761u) >> 19, k = 0; k < 8192; i = (i + 1) & 8191, k++)
        if (!prof_hist[t][i].fn || prof_hist[t][i].fn == fn) { prof_hist[t][i].fn = fn; prof_hist[t][i].n++; break; }
    prof_total[t]++;
}
static void *prof_thread(void *arg) {
    (void)arg;
    // DR_PROFILE_STACK=t: every two seconds, the frames of guest thread t (read while it runs: a sample, not a fact)
    int st = getenv("DR_PROFILE_STACK") ? atoi(getenv("DR_PROFILE_STACK")) : -1, tick = 0, shown = 0;
    for (;;) {
        usleep(1000);
        if (st >= 0 && st < nprof_cpus && prof_cpus[st] && ++tick % 2000 == 0 && shown < 60) {
            CPU *c = prof_cpus[st]; uint32_t ebp = c->ebp; char b[700]; int n = 0;
            n += snprintf(b + n, sizeof b - n, "  [stack %d] in %s%s%s:", st, rt_func_name(c->fn), prof_in[st] ? " import " : "", prof_in[st] ? prof_in[st] : "");
            for (int k = 0; k < 14 && n < 640 && ebp > 0x10000 && ebp < 0xA0000000u; k++) {
                uint32_t ra = rd32(ebp + 4);
                n += snprintf(b + n, sizeof b - n, " %08x", ra);
                uint32_t next = rd32(ebp); if (next <= ebp) break; ebp = next;
            }
            fprintf(stderr, "%s\n", b); shown++;
        }
        for (int t = 0; t < nprof_cpus; t++) {
            if (!prof_cpus[t] || !prof_cpus[t]->fn) continue;
            prof_add(t, prof_cpus[t]->fn);
            const char *in = prof_in[t];
            if (in) for (int k = 0; k < 64; k++)
                if (!prof_imp[t][k].name || prof_imp[t][k].name == in) { prof_imp[t][k].name = in; prof_imp[t][k].n++; break; }
        }
    }
    return NULL;
}
static void prof_register(CPU *c) {
    static int on = -1;
    if (on < 0) { on = getenv("DR_PROFILE") != NULL; if (on) { pthread_t th; pthread_create(&th, NULL, prof_thread, NULL); } }
    if (!on) return;
    int k = __atomic_fetch_add(&nprof_cpus, 1, __ATOMIC_SEQ_CST);
    if (k < NPROF_CPU) prof_cpus[k] = c;
}
static void prof_report(void) {
    for (int t = 0; t < nprof_cpus && t < NPROF_CPU; t++) {
        if (prof_total[t] < 200) continue;
        fprintf(stderr, "  [profile] guest thread %d: %u samples\n", t, prof_total[t]);
        for (int r = 0; r < 15; r++) {
            int best = -1;
            for (int i = 0; i < 8192; i++) if (prof_hist[t][i].n && (best < 0 || prof_hist[t][i].n > prof_hist[t][best].n)) best = i;
            if (best < 0) break;
            fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * prof_hist[t][best].n / prof_total[t], rt_func_name(prof_hist[t][best].fn));
            prof_hist[t][best].n = 0;
        }
        for (int r = 0; r < 6; r++) {
            int best = -1;
            for (int k = 0; k < 64; k++) if (prof_imp[t][k].n && (best < 0 || prof_imp[t][k].n > prof_imp[t][best].n)) best = k;
            if (best < 0) break;
            fprintf(stderr, "    %5.1f%%  inside import %s\n", 100.0 * prof_imp[t][best].n / prof_total[t], prof_imp[t][best].name);
            prof_imp[t][best].n = 0;
        }
    }
}

CPU *cpu_new(uint32_t stack_size) {
    CPU *c = calloc(1, sizeof *c);
    prof_register(c);
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
uint32_t aot_malloc(uint32_t n); void aot_free(uint32_t p);
// For the framework's eh.c: where the guest's exception objects live.
uint32_t eh_guest_alloc(uint32_t n) { return aot_malloc(n); }
void eh_guest_free(uint32_t p) { aot_free(p); }
// For the native shims that use the bridge's tables (handles): its lock.
void aot_lock(void) { big_lock(); }
void aot_unlock(void) { big_unlock(); }
uint32_t aot_stack_lo(void) { return Gt ? G.stack_lo : 0; }
uint32_t aot_stack_hi(void) { return Gt ? G.stack_hi : 0; }
// For threads.c: memory of the guest's, taken under the bridge's lock.
uint32_t aot_malloc(uint32_t n) { big_lock(); uint32_t p = dr_guest_malloc(main_cpu->g, n); big_unlock(); return p; }
void aot_free(uint32_t p) { big_lock(); dr_guest_free(main_cpu->g, p); big_unlock(); }
uint32_t aot_block_size(uint32_t p) { big_lock(); uint32_t n = dr_guest_block_size(main_cpu->g, p); big_unlock(); return n; }

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
