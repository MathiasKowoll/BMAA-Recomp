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

// DR_TRACE_AT=n: what the guest executes between call n returning and call n+1. The interpreter prints every
// instruction (its own trace); the translated code prints every function it enters (aot.c sets the hook).
void (*trace_window_hook)(int on);

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
        fprintf(f, "%llu %08x ret=%08x a=%08x,%08x,%08x,%08x bx=%08x si=%08x di=%08x bp=%08x sp=%08x",
                (unsigned long long)n, thunk, dr_r32(cpu->g, sp), dr_r32(cpu->g, sp + 4), dr_r32(cpu->g, sp + 8),
                dr_r32(cpu->g, sp + 12), dr_r32(cpu->g, sp + 16), cpu->regs[DR_EBX], cpu->regs[DR_ESI],
                cpu->regs[DR_EDI], cpu->regs[DR_EBP], sp);
    }
    if (at != ~0ull && n == at + span) { cpu->trace = false; if (trace_window_hook) trace_window_hook(0); fprintf(stderr, "  [window] closed at call %llu\n", (unsigned long long)n); }
    n++;
    bool ok = dr_bridge_dispatch(cpu, thunk, user);
    if (at != ~0ull && n == at + 1) { fprintf(stderr, "  [window] open after call %llu\n", (unsigned long long)at); cpu->trace = true; if (trace_window_hook) trace_window_hook(1); }
    if (on) { fprintf(f, " -> %08x\n", cpu->regs[DR_EAX]); if (n == limit) fclose(f); }
    return ok;
}
