// A record of every call the guest makes into the bridge, the same from the interpreter and from the translated
// code, so that the two can be compared line by line: the first line that differs is just after the first guest
// instruction the translation got wrong (or the first thing that is not the same from one run to the next).
//
// The driver is compiled with dr_bridge_dispatch renamed to this, in both programs (see the Makefile).
//   DR_TRACE_IMPORTS=file   write the record there
//   DR_TRACE_LIMIT=n        stop writing after n calls (default 2,000,000)
#include <stdio.h>
#include <stdlib.h>
#include "dr_x86.h"

bool dr_bridge_dispatch(dr_cpu *cpu, uint32_t thunk, void *user);

// Two runs can only be compared as far as they are given the same things. With DR_SAME_RANDOM set, the bridge's
// arc4random (this definition is the one its objects link to) is a fixed sequence; without it, it is the system's.
uint32_t arc4random(void) {
    static int fixed = -1; static uint32_t s = 0x2545F491u;
    if (fixed < 0) fixed = getenv("DR_SAME_RANDOM") != NULL;
    if (!fixed) { uint32_t v; arc4random_buf(&v, sizeof v); return v; }
    s = s * 1664525u + 1013904223u;
    return s;
}

// DR_TRACE_AT=n: what the guest executes between call n returning and call n+1. The interpreter prints every
// instruction (its own trace); the translated code prints every function it enters (aot.c sets the hook).
void (*trace_window_hook)(int on);

// What an argument points at, in a word: 24 bytes of it, if it is a pointer into memory that is surely there (the
// image, the stacks, the part of the heap handed out). Two runs whose calls agree but whose data does not are
// told apart by this, a long way before the data changes which call comes next.
static uint32_t points_at(const dr_cpu *cpu, uint32_t p) {
    const dr_guest *g = cpu->g;
    int ok = (p >= 0x1000 && p < 0x03600000u) || (p >= 0x1f800000u && p < 0x20000000u - 24) ||
             (p >= DR_HEAP_BASE && g->heap > 24 && p < g->heap - 24);
    if (!ok) return 0;
    uint32_t h = 2166136261u;
    for (int k = 0; k < 24; k += 4) {   // a word that is a handle for a host object counts as any other handle
        uint32_t w = dr_r32(g, p + (uint32_t)k);
        if (w >= 0xE0000000u && w < 0xE1000000u) w = 0xE0000000u;
        h = (h ^ w) * 16777619u;
    }
    return h;
}

// The clocks the bridge reads, under DR_SAME_RANDOM: each reading is a little later than the one before, the same
// in every run. (These definitions are what the bridge's objects link to, and what its dlsym finds; the system's
// own frameworks keep the real ones.) Without the variable they are the real clocks, asked the long way round.
#include <time.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
static int same_world(void) { static int v = -1; if (v < 0) v = getenv("DR_SAME_RANDOM") != NULL; return v; }
static uint64_t tick(void) { static uint64_t t; return t += 100000; }   // 100 microseconds a reading, in ns
uint64_t mach_absolute_time(void) {
    if (same_world()) return 1000000000ull + tick();
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
time_t time(time_t *out) {
    time_t t;
    if (same_world()) t = 1795000000 + (time_t)(tick() / 1000000000ull);
    else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); t = ts.tv_sec; }
    if (out) *out = t;
    return t;
}
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return 0;
    if (same_world()) { uint64_t n = tick(); tv->tv_sec = 1795000000 + (time_t)(n / 1000000000ull); tv->tv_usec = (suseconds_t)(n % 1000000000ull / 1000); }
    else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); tv->tv_sec = ts.tv_sec; tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000); }
    return 0;
}
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void) {
    if (same_world()) return 816000000.0 + (double)tick() / 1e9;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec - kCFAbsoluteTimeIntervalSince1970 + (double)ts.tv_nsec / 1e9;
}

bool traced_dispatch(dr_cpu *cpu, uint32_t thunk, void *user) {
    static FILE *f; static uint64_t n, limit, at = ~0ull, span = 1; static int asked;
    if (!asked) {
        asked = 1;
        if (getenv("DR_TRACE_AT")) at = strtoull(getenv("DR_TRACE_AT"), NULL, 0);
        if (getenv("DR_TRACE_SPAN")) span = strtoull(getenv("DR_TRACE_SPAN"), NULL, 0);   // calls the window stays open for
        const char *p = getenv("DR_TRACE_IMPORTS");
        if (p) f = fopen(p, "w");
        limit = getenv("DR_TRACE_LIMIT") ? strtoull(getenv("DR_TRACE_LIMIT"), NULL, 0) : 2000000;
    }
    bool on = f && n < limit;
    if (at != ~0ull && n > at && n < at + span)
        fprintf(stderr, "  [call] %llu thunk %08x ret=%08x a=%08x,%08x,%08x\n", (unsigned long long)n, thunk,
                dr_r32(cpu->g, cpu->regs[DR_ESP]), dr_r32(cpu->g, cpu->regs[DR_ESP] + 4),
                dr_r32(cpu->g, cpu->regs[DR_ESP] + 8), dr_r32(cpu->g, cpu->regs[DR_ESP] + 12));
    if (on) {
        uint32_t sp = cpu->regs[DR_ESP];
        uint32_t a0 = dr_r32(cpu->g, sp + 4), a1 = dr_r32(cpu->g, sp + 8), a2 = dr_r32(cpu->g, sp + 12), a3 = dr_r32(cpu->g, sp + 16);
        fprintf(f, "%llu %08x ret=%08x a=%08x,%08x,%08x,%08x bx=%08x si=%08x di=%08x bp=%08x sp=%08x at=%08x,%08x,%08x,%08x",
                (unsigned long long)n, thunk, dr_r32(cpu->g, sp), a0, a1, a2, a3, cpu->regs[DR_EBX], cpu->regs[DR_ESI],
                cpu->regs[DR_EDI], cpu->regs[DR_EBP], sp, points_at(cpu, a0), points_at(cpu, a1), points_at(cpu, a2), points_at(cpu, a3));
    }
    if (at != ~0ull && n == at + span) { cpu->trace = false; if (trace_window_hook) trace_window_hook(0); fprintf(stderr, "  [window] closed at call %llu\n", (unsigned long long)n); }
    n++;
    bool ok = dr_bridge_dispatch(cpu, thunk, user);
    if (at != ~0ull && n == at + 1) { fprintf(stderr, "  [window] open after call %llu\n", (unsigned long long)at); cpu->trace = true; if (trace_window_hook) trace_window_hook(1); }
    if (on) { fprintf(f, " -> %08x\n", cpu->regs[DR_EAX]); if (n == limit) fclose(f); }
    return ok;
}
