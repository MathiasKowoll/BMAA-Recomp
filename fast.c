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
void I__mach_absolute_time(CPU *c) { uint64_t t = mach_absolute_time(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }

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
