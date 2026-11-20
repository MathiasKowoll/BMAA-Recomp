// The imports the engine calls tens of millions of times that are pure functions of their arguments and of guest
// memory: answered here, straight from translated code, instead of through the bridge. The bridge's path (its
// lock, the hand-over of registers, the bookkeeping per call) cost about a microsecond each, which was most of the
// engine's loading time: 40 million mach_absolute_time and 20 million tolower in three minutes.
//
// Nothing here touches the bridge's state (handles, files, the guest heap), so no lock is needed.
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
