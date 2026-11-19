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
// One guest thread, as in DecompRecomp today.
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <unistd.h>

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
// what their first four arguments point at, when they point into guest memory that is there.
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
    fprintf(stderr, "        %08x:", p);
    for (int k = 0; k < 32; k++) fprintf(stderr, " %02x", rd8(p + k));
    fprintf(stderr, "  |");
    for (int k = 0; k < 32; k++) { uint8_t ch = rd8(p + k); fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
    fprintf(stderr, "|\n");
}
void rt_on_watch(CPU *c, uint32_t a) {
    if (watching_some && rt_watch[0] != 0xFF) {
        static struct { uint32_t a; int n; } seen[64];
        for (int k = 0; k < 64; k++) {
            if (seen[k].a == a || !seen[k].a) { seen[k].a = a; if (++seen[k].n > 40) return; break; }
        }
        fprintf(stderr, "  [watch] %08x from %08x (%s)  a=%08x,%08x,%08x,%08x  ecx=%08x\n", a, rd32(c->esp), rt_func_name(rd32(c->esp)),
                rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), rd32(c->esp + 16), c->ecx);
        for (int k = 1; k <= 4; k++) show(rd32(c->esp + 4 * k));
        return;
    }
    fprintf(stderr, "  [fn] %08x %s  ret=%08x a=%08x,%08x,%08x  eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x esp=%08x\n",
            a, rt_func_name(a), rd32(c->esp), rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12),
            c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->esp);
}

static dr_cpu *D;            // DecompRecomp's CPU
static CPU G;                // the same guest thread, as the translated code sees it
static sigjmp_buf *leave;    // the innermost dr_cpu_run
static dr_status leave_with;
static uint64_t nimports, ncalls_ind;

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
    if (!D->on_import || !D->on_import(D, thunk, D->user))
        stop(D->exited ? DR_HALT_EXIT : D->threw ? DR_ERR_THROW : DR_ERR_IMPORT);
    in();
    if (D->tail_jump) {   // the bridge found guest code to run in its place: same stack, same return address
        D->tail_jump = false;
        enter(c, D->tail_target);
        c->esp -= 4;      // that function popped the address; the caller of an import pops it too
    }
}

// Control goes to guest address t with the return address on the stack; when this returns, the address is popped.
static void enter(CPU *c, uint32_t t) {
    if (t >= DR_THUNK_BASE) { import(c, t); c->esp += 4; return; }
    const GFunc *f = func_at(t);
    if (f) { f->fn(c); return; }
    for (int k = 0; k < ngstubs; k++)   // the address of a stub, taken as a function pointer
        if (gstubs[k].addr == t) { gstubs[k].fn(c); c->esp += 4; return; }
    if (t < D->null_floor) stop(DR_ERR_NULLCALL);
    fprintf(stderr, "  [aot] call to 0x%08x, which is no function of the translation (from %s)\n", t, rt_func_name(c->fn));
    if (t >= DR_UNRESOLVED_BASE) fprintf(stderr, "  [aot]   that is the marker of a data symbol nobody resolved\n");
    stop(DR_ERR_OPCODE);
}
void call_ind(CPU *c, uint32_t t) { ncalls_ind++; enter(c, t); }

// ---- names of imports -> thunks ----
// A stub is `jmp *[pointer]` (ff 25, then the pointer's address), and DecompRecomp's loader wrote the thunk there.
static struct { const char *name; uint32_t thunk; } *by_name;
static uint32_t nby_name;
static uint32_t hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static uint32_t thunk_of(const char *name) {
    if (!by_name) {
        nby_name = 1; while (nby_name < (uint32_t)ngstubs * 2 + 16) nby_name <<= 1;
        by_name = calloc(nby_name, sizeof *by_name);
        for (int k = 0; k < ngstubs; k++) {
            uint32_t a = gstubs[k].addr, t = 0;
            if (rd8(a) == 0xFF && rd8(a + 1) == 0x25) t = rd32(rd32(a + 2));
            uint32_t i = hash(gstubs[k].name) & (nby_name - 1);
            while (by_name[i].name) i = (i + 1) & (nby_name - 1);
            by_name[i].name = gstubs[k].name; by_name[i].thunk = t;
        }
    }
    for (uint32_t i = hash(name) & (nby_name - 1); by_name[i].name; i = (i + 1) & (nby_name - 1))
        if (!strcmp(by_name[i].name, name)) return by_name[i].thunk;
    return 0;
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

void unimpl_import(CPU *c, const char *name) {
    if (!strcmp(name, "_longjmp") || !strcmp(name, "__longjmp") || !strcmp(name, "_siglongjmp")) {
        GJmp *j = gjmp_slot(ARG(0));
        j->val = ARG(1) ? ARG(1) : 1;
        longjmp(j->host, 1);
    }
    uint32_t thunk = thunk_of(name);
    if (window_open) fprintf(stderr, "  [imp] %s  a=%08x,%08x,%08x,%08x  from %s\n", name, ARG(0), ARG(1), ARG(2), ARG(3), rt_func_name(rd32(c->esp)));
    if (thunk < DR_THUNK_BASE) {
        fprintf(stderr, "  [aot] import %s has no thunk (its pointer holds 0x%08x), called from %s\n", name, thunk, rt_func_name(c->fn));
        stop(DR_ERR_IMPORT);
    }
    import(c, thunk);
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
    memset(&G, 0, sizeof G);
    G.fcw = 0x37F; G.mxcsr = 0x1F80;
    D = cpu;
    trace_window_hook = window;
    watch_some();
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
    if (!D || !leave || a < M || a >= M + DR_GUEST_SIZE) {
        char b[256];
        int n = snprintf(b, sizeof b, "  [aot] host fault at %p (guest memory is %p..%p), in or under guest %s\n",
                         (void *)a, (void *)M, (void *)(M + DR_GUEST_SIZE), rt_func_name(G.fn));
        write(2, b, (size_t)n);
        _exit(139);
    }
    D->fault_addr = (uint32_t)(a - M);
    D->faulted = true;
    stop(DR_ERR_FAULT);
}
void dr_cpu_install_fault_handler(dr_cpu *cpu) {
    D = cpu;
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

dr_status dr_cpu_run(dr_cpu *cpu) {
    D = cpu;
    sigjmp_buf here, *outer = leave;
    if (sigsetjmp(here, 1)) { leave = outer; return leave_with; }
    leave = &here;
    in();
    G.fn = cpu->eip;
    enter(&G, cpu->eip);     // returns when that function does, its return address (the sentinel) popped
    out();
    leave = outer;
    cpu->eip = cpu->magic_ret;
    return DR_HALT_RET;
}

dr_status dr_cpu_call_nested(dr_cpu *cpu, uint32_t func, const uint32_t *args, int argc, uint32_t stack_top,
                             uint32_t *out_eax) {
    if (cpu->nest_depth >= DR_MAX_NEST) return DR_ERR_LIMIT;
    uint32_t saved_regs[8]; memcpy(saved_regs, cpu->regs, sizeof saved_regs);
    uint32_t saved_eip = cpu->eip, saved_flags = cpu->flags, saved_fn = G.fn;
    bool saved_exited = cpu->exited, saved_tail = cpu->tail_jump;
    uint32_t saved_tail_target = cpu->tail_target, saved_magic = cpu->magic_ret;

    cpu->nest_depth++;
    cpu->magic_ret = 0xDEAD0000u + (uint32_t)cpu->nest_depth;
    cpu->regs[DR_ESP] = stack_top;
    for (int i = argc - 1; i >= 0; i--) dr_push(cpu, args[i]);
    dr_push(cpu, cpu->magic_ret);
    cpu->eip = func;
    cpu->exited = false;

    dr_status st = dr_cpu_run(cpu);
    if (out_eax) *out_eax = cpu->regs[DR_EAX];
    cpu->fail_eip = (st == DR_OK || st == DR_HALT_RET) ? 0 : cpu->eip;

    cpu->magic_ret = saved_magic;
    cpu->nest_depth--;
    memcpy(cpu->regs, saved_regs, sizeof saved_regs);
    cpu->eip = saved_eip; cpu->flags = saved_flags; cpu->exited = saved_exited;
    cpu->tail_jump = saved_tail; cpu->tail_target = saved_tail_target;
    G.fn = saved_fn;
    return st;
}

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
