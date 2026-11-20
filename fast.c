// The imports the engine calls tens of millions of times that are pure functions of their arguments and of guest
// memory: answered here, straight from translated code, instead of through the bridge. The bridge's path (its
// lock, the hand-over of registers, the bookkeeping per call) cost about a microsecond each, which was most of the
// engine's loading time: 40 million mach_absolute_time and 20 million tolower in three minutes.
//
// Nothing here touches the bridge's state (handles, files, the guest heap), so no lock is needed.
#include <stdlib.h>
#include "rt.h"
#include <ctype.h>
#include <mach/mach_time.h>
#include <string.h>

#define RET(v) (c->eax = (uint32_t)(v))
static const char *S(uint32_t a) { return (const char *)G2H(a); }

void I____tolower(CPU *c) { RET(tolower((int)ARG(0))); }
void I____toupper(CPU *c) { RET(toupper((int)ARG(0))); }

// The host's: the bridge answered it the same (trace.c replaces it with a fixed clock under DR_SAME_RANDOM, and
// that replacement is what this call reaches too).
// In nanoseconds, with a time base of 1/1: what every Intel Mac this game ever ran on gave it. Apple Silicon counts
// in ticks of 125/3 ns, and code written for Intel reads the counter as nanoseconds in places without asking for the
// time base: the engine's 60-a-second frame limiter then waited 41.67 times too long (1.5 frames a second).
#include <time.h>
void I__mach_absolute_time(CPU *c) { uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }
void I__mach_timebase_info(CPU *c) { if (ARG(0)) { wr32(ARG(0), 1); wr32(ARG(0) + 4, 1); } RET(0); }

void I__memcpy(CPU *c)  { memmove(G2H(ARG(0)), G2H(ARG(1)), ARG(2)); RET(ARG(0)); }
void I__memmove(CPU *c) { memmove(G2H(ARG(0)), G2H(ARG(1)), ARG(2)); RET(ARG(0)); }
void I__memset(CPU *c)  { memset(G2H(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
void I__memcmp(CPU *c)  { RET(memcmp(G2H(ARG(0)), G2H(ARG(1)), ARG(2))); }
void I__strlen(CPU *c)  { RET(strlen(S(ARG(0)))); }
void I__strcmp(CPU *c)  { RET(strcmp(S(ARG(0)), S(ARG(1)))); }
void I__strncmp(CPU *c) { RET(strncmp(S(ARG(0)), S(ARG(1)), ARG(2))); }

// wchar_t is four bytes on both sides, but the guest's strings are compared by hand, as the bridge does, so that
// a pointer is never taken for the host's wchar_t (same size, same order: the result is the same).
void I__wcscmp(CPU *c) {
    uint32_t a = ARG(0), b = ARG(1);
    for (;; a += 4, b += 4) {
        uint32_t x = rd32(a), y = rd32(b);
        if (x != y) { RET(x < y ? -1 : 1); return; }
        if (!x) { RET(0); return; }
    }
}
void I__wcslen(CPU *c) { uint32_t a = ARG(0), n = 0; while (rd32(a + 4 * n)) n++; RET(n); }

// vswscanf(text, format, va_list): the wide characters are the host's size and so are int, float and double; what
// differs is that each output is a 32-bit pointer on the game's stack and that its `long` is four bytes. So the
// format goes to the host's own with the `l` taken off the integer conversions and the pointers translated.
#include <wchar.h>
void I__vswscanf(CPU *c) {
    const wchar_t *text = (const wchar_t *)G2H(ARG(0)), *f = (const wchar_t *)G2H(ARG(1));
    uint32_t ap = ARG(2);
    wchar_t fmt[512]; int n = 0, nout = 0; void *out[16] = {0};
    for (; *f && n < 500; f++) {
        fmt[n++] = *f;
        if (*f != L'%') continue;
        if (f[1] == L'%') { fmt[n++] = *++f; continue; }
        int skip = 0, lpos = -1;
        for (f++; *f && n < 500; f++) {
            if (*f == L'*') skip = 1;
            if (*f == L'l' || *f == L'q') { if (lpos < 0) lpos = n; }
            fmt[n++] = *f;
            if (*f == L'[') { for (f++; *f && n < 500; f++) { fmt[n++] = *f; if (*f == L']' && f[-1] != L'[' ) break; } break; }
            if (wcschr(L"diouxXncsCSeEfgGaAp", *f)) {
                // the game's long is an int: one `l` comes off an integer conversion (two stay: long long is 8 in both)
                if (lpos >= 0 && wcschr(L"diouxXn", *f) && !(lpos + 1 < n - 1 && fmt[lpos + 1] == L'l')) {
                    for (int k = lpos; k < n - 1; k++) fmt[k] = fmt[k + 1];
                    n--;
                }
                break;
            }
        }
        if (!*f) break;
        if (!skip && nout < 16) { uint32_t g = rd32(ap + 4u * (uint32_t)nout); out[nout++] = g ? G2H(g) : NULL; }
    }
    fmt[n] = 0;
    c->eax = (uint32_t)swscanf(text, fmt, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8], out[9], out[10],
                               out[11], out[12], out[13], out[14], out[15]);
}

// The engine's "seconds since boot" (asked many times a frame): sysctl kern.boottime and gettimeofday. Both are
// two 32-bit numbers in the game's layout; through the bridge they were 40% of the engine thread on the title screen.
#include <sys/sysctl.h>
#include <sys/time.h>
void unimpl_import(CPU *c, const char *name);
void I__sysctl(CPU *c) {   // (int *name, u_int namelen, void *old, size_t *oldlen, void *new, size_t newlen)
    if (ARG(1) == 2 && rd32(ARG(0)) == CTL_KERN && rd32(ARG(0) + 4) == KERN_BOOTTIME && ARG(2) && ARG(3) && rd32(ARG(3)) >= 8 && !ARG(4)) {
        static struct timeval boot;
        if (!boot.tv_sec) { int mib[2] = { CTL_KERN, KERN_BOOTTIME }; size_t n = sizeof boot; sysctl(mib, 2, &boot, &n, NULL, 0); }
        wr32(ARG(2), (uint32_t)boot.tv_sec); wr32(ARG(2) + 4, (uint32_t)boot.tv_usec); wr32(ARG(3), 8);
        RET(0); return;
    }
    unimpl_import(c, "_sysctl");
}
void I__gettimeofday(CPU *c) {
    struct timeval tv; gettimeofday(&tv, NULL);
    if (ARG(0)) { wr32(ARG(0), (uint32_t)tv.tv_sec); wr32(ARG(0) + 4, (uint32_t)tv.tv_usec); }
    if (ARG(1)) { wr32(ARG(1), 0); wr32(ARG(1) + 4, 0); }
    RET(0);
}

// task_info(task, MACH_TASK_BASIC_INFO, out, count in and out): the engine reads its own memory use every frame.
// The structure is packed the same way in both worlds (64-bit sizes, 4-byte packing), so the Mac's own goes in.
#include <mach/mach.h>
void I__task_info(CPU *c) {
    if (ARG(1) != MACH_TASK_BASIC_INFO || !ARG(2) || !ARG(3)) { unimpl_import(c, "_task_info"); return; }
    mach_msg_type_number_t n = rd32(ARG(3));
    struct mach_task_basic_info info;
    if (n > MACH_TASK_BASIC_INFO_COUNT) n = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t k = task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n);
    if (!k) { memcpy(G2H(ARG(2)), &info, n * sizeof(natural_t)); wr32(ARG(3), n); }
    c->eax = (uint32_t)k;
}

// BMAA_NCPU=n: the number of processors the game is told (hw.ncpu, hw.logicalcpu_max, hw.physicalcpu_max), for telling
// a fault of the game's own threads working together from one of a single thread's arithmetic.
void I__sysctlbyname(CPU *c) {   // (name, old, oldlen, new, newlen)
    static int n = -1; if (n < 0) n = getenv("BMAA_NCPU") ? atoi(getenv("BMAA_NCPU")) : 0;
    const char *name = ARG(0) ? (const char *)G2H(ARG(0)) : "";
    if (n > 0 && ARG(1) && ARG(2) && rd32(ARG(2)) >= 4 &&
        (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu_max") || !strcmp(name, "hw.physicalcpu_max"))) {
        wr32(ARG(1), (uint32_t)n); wr32(ARG(2), 4); c->eax = 0; return;
    }
    unimpl_import(c, "_sysctlbyname");
}

// ---- The calls the game makes millions of times while it plays ----
// A run of four and a half minutes made 155 million calls out of the game, and 220 seconds went inside them: the
// general way finds each by comparing names and holds the runtime's one lock meanwhile, about a microsecond a call,
// and the rendering thread stood waiting for that lock. These are the ones at the top of that list, answered here
// directly: the allocator (19 million pairs of posix_memalign and free), bzero, the float tests and the maths
// functions, and the two clock calls the sound library asks 35 thousand times a second.
#include <math.h>
#include <strings.h>
uint32_t aot_malloc(uint32_t n); void aot_free(uint32_t p); uint32_t aot_block_size(uint32_t p);
void fast_free(CPU *c) { if (ARG(0)) aot_free(ARG(0)); c->eax = 0; }   // I__free and I__posix_memalign are heapdbg.c's
void fast_posix_memalign(CPU *c) {   // (void **out, alignment, size): the heap's blocks are aligned already
    uint32_t p = aot_malloc(ARG(2));
    if (ARG(0)) wr32(ARG(0), p);
    c->eax = p ? 0 : 12;
}
void I__malloc_size(CPU *c) { c->eax = ARG(0) ? aot_block_size(ARG(0)) : 0; }
void I____bzero(CPU *c) { if (ARG(1)) memset(G2H(ARG(0)), 0, ARG(1)); }
void I__bzero(CPU *c) { if (ARG(1)) memset(G2H(ARG(0)), 0, ARG(1)); }
void I____isnanf(CPU *c) { c->eax = isnan(ARGF(0)) != 0; }
void I____isinff(CPU *c) { c->eax = isinf(ARGF(0)) != 0; }
void I__cosf(CPU *c) { FPUSH((double)cosf(ARGF(0))); }
void I__sinf(CPU *c) { FPUSH((double)sinf(ARGF(0))); }
void I__acosf(CPU *c) { FPUSH((double)acosf(ARGF(0))); }
void I__fmodf(CPU *c) { FPUSH((double)fmodf(ARGF(0), ARGF(1))); }
void I__powf(CPU *c) { FPUSH((double)powf(ARGF(0), ARGF(1))); }
void I____umoddi3(CPU *c) {
    uint64_t a = (uint64_t)ARG(0) | (uint64_t)ARG(1) << 32, b = (uint64_t)ARG(2) | (uint64_t)ARG(3) << 32;
    uint64_t r = b ? a % b : 0;
    c->eax = (uint32_t)r; c->edx = (uint32_t)(r >> 32);
}
void I__strcasecmp(CPU *c) { c->eax = (uint32_t)strcasecmp((const char *)G2H(ARG(0)), (const char *)G2H(ARG(1))); }
// UpTime(): an AbsoluteTime, here nanoseconds (as mach_absolute_time above, with its 1/1 time base);
// AbsoluteToDuration(AbsoluteTime): milliseconds when it reaches one, else minus the microseconds
void I__UpTime(CPU *c) { uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }
void I__AbsoluteToDuration(CPU *c) {
    uint64_t ns = (uint64_t)ARG(0) | (uint64_t)ARG(1) << 32;
    c->eax = ns >= 1000000ull ? (uint32_t)(int32_t)(ns / 1000000ull) : (uint32_t)(int32_t)(-(int64_t)(ns / 1000ull));
}
void I__AbsoluteToNanoseconds(CPU *c) { c->eax = ARG(0); c->edx = ARG(1); }

// DR_IMPORT_STATS: which Objective-C messages the game sends, by selector (the general way costs over ten
// microseconds a message, so the ones sent every frame are the next to answer directly).
#include <stdio.h>
#include <pthread.h>
static struct { const char *sel; uint64_t n, ns; } sels[1024];
static pthread_mutex_t sels_mx = PTHREAD_MUTEX_INITIALIZER;
static void sel_count(uint32_t sel, uint64_t ns) {
    if (!sel || sel >= 0xE0000000u) return;
    const char *name = (const char *)G2H(sel);
    pthread_mutex_lock(&sels_mx);
    for (int i = 0; i < 1024; i++) {
        if (!sels[i].sel) sels[i].sel = name;
        if (sels[i].sel == name || !strcmp(sels[i].sel, name)) { sels[i].n++; sels[i].ns += ns; break; }
    }
    pthread_mutex_unlock(&sels_mx);
}
void objc_chain_report(void);
void objc_sel_report(void) {
    objc_chain_report();
    for (int r = 0; r < 30; r++) {
        int best = -1;
        for (int i = 0; i < 1024 && sels[i].sel; i++) if (sels[i].ns && (best < 0 || sels[i].ns > sels[best].ns)) best = i;
        if (best < 0) break;
        fprintf(stderr, "  [objc-sel] %8.2f s %9llu  %s\n", sels[best].ns / 1e9, (unsigned long long)sels[best].n, sels[best].sel);
        sels[best].ns = 0;
    }
}
static int sel_stats(void) { static int v = -1; if (v < 0) v = getenv("DR_IMPORT_STATS") != NULL; return v; }
const char *rt_func_name(uint32_t a);
static struct { uint32_t ra[5]; uint64_t n; } chains[64];
static void chain_count(CPU *c) {            // who sends it: the return addresses up the game's frames
    uint32_t ra[5] = { rd32(c->esp) }, ebp = c->ebp;
    for (int k = 1; k < 5; k++) { if (ebp < 0x1000 || ebp >= 0xE0000000u) break; ra[k] = rd32(ebp + 4); ebp = rd32(ebp); }
    pthread_mutex_lock(&sels_mx);
    for (int i = 0; i < 64; i++) {
        if (!chains[i].n) memcpy(chains[i].ra, ra, sizeof ra);
        if (!memcmp(chains[i].ra, ra, sizeof ra)) { chains[i].n++; break; }
    }
    pthread_mutex_unlock(&sels_mx);
}
void objc_chain_report(void) {
    for (int i = 0; i < 64 && chains[i].n; i++) {
        fprintf(stderr, "  [objc-who] %8llu ", (unsigned long long)chains[i].n);
        for (int k = 0; k < 5; k++) fprintf(stderr, " %s", chains[i].ra[k] ? rt_func_name(chains[i].ra[k]) : "-");
        fputc('\n', stderr);
    }
}
void I__objc_msgSend(CPU *c) {
    if (!sel_stats()) { unimpl_import(c, "_objc_msgSend"); return; }
    uint32_t sel = ARG(1); uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (sel && sel < 0xE0000000u && !strcmp((const char *)G2H(sel), "screens")) chain_count(c);
    unimpl_import(c, "_objc_msgSend");
    sel_count(sel, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
}
void I__objc_msgSend_stret(CPU *c) {
    if (!sel_stats()) { unimpl_import(c, "_objc_msgSend_stret"); return; }
    uint32_t sel = ARG(2); uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    unimpl_import(c, "_objc_msgSend_stret");
    sel_count(sel, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
}
