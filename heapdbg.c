// DR_HEAP_WHO=1: who holds the game's memory. One allocation in 16 made with posix_memalign is remembered with the
// code that asked for it (six frames up); a free forgets it. When the run is stopped, the callers with the most
// remembered allocations still alive are printed, by frame depth: a leak shows as one caller far above the rest.
// Without the variable both calls go straight on to the heap (fast.c).
#include "rt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void unimpl_import(CPU *c, const char *name);
void fast_free(CPU *c); void fast_posix_memalign(CPU *c);   // fast.c: straight to the heap, not through the bridge
const char *rt_func_name(uint32_t a);
#define SLOTS (1u << 21)
#define DEPTH 6
typedef struct { uint32_t p, size, ra[DEPTH]; } Rec;
static Rec *recs; static int on = -1; static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long nalloc, nfree, nfree_known;

static int enabled(void) {
    if (on < 0) { on = getenv("DR_HEAP_WHO") != NULL; if (on) recs = calloc(SLOTS, sizeof *recs); }
    return on;
}
void I__posix_memalign(CPU *c) {
    uint32_t out = ARG(0), size = ARG(2), ebp = c->ebp, ra0 = rd32(c->esp);
    fast_posix_memalign(c);
    if (!enabled() || c->eax || !out) return;
    uint32_t p = rd32(out);
    pthread_mutex_lock(&mx);
    if ((nalloc++ & 15) == 0 && p) {
        for (uint32_t h = (p >> 4) * 2654435761u, k = 0; k < 64; k++) {
            Rec *r = &recs[(h + k) & (SLOTS - 1)];
            if (r->p) continue;
            r->p = p; r->size = size; r->ra[0] = ra0;
            for (int d = 1; d < DEPTH; d++) {
                r->ra[d] = (ebp > 0x10000 && ebp < 0xA0000000u) ? rd32(ebp + 4) : 0;
                uint32_t up = (ebp > 0x10000 && ebp < 0xA0000000u) ? rd32(ebp) : 0; ebp = up > ebp ? up : 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&mx);
}
void I__free(CPU *c) {
    uint32_t p = ARG(0);
    if (enabled() && p) {
        pthread_mutex_lock(&mx);
        nfree++;
        for (uint32_t h = (p >> 4) * 2654435761u, k = 0; k < 64; k++) {
            Rec *r = &recs[(h + k) & (SLOTS - 1)];
            if (r->p == p) { r->p = 0xFFFFFFFFu; nfree_known++; break; }   // a tombstone: the probe chain stays whole
            if (!r->p) break;
        }
        pthread_mutex_unlock(&mx);
    }
    fast_free(c);
}
void heapdbg_report(void) {
    if (on != 1) return;
    fprintf(stderr, "  [heap] %llu posix_memalign, %llu free (%llu of them of a remembered block)\n", nalloc, nfree, nfree_known);
    for (int d = 0; d < DEPTH; d++) {
        static struct { uint32_t ra; unsigned n; unsigned long long bytes; } top[4096];
        memset(top, 0, sizeof top);
        for (uint32_t i = 0; i < SLOTS; i++) {
            Rec *r = &recs[i];
            if (!r->p || r->p == 0xFFFFFFFFu) continue;
            for (int k = 0; k < 4096; k++) if (top[k].ra == r->ra[d] || !top[k].n) { top[k].ra = r->ra[d]; top[k].n++; top[k].bytes += r->size; break; }
        }
        fprintf(stderr, "  [heap] depth %d, remembered blocks still alive (x16 for the real number):\n", d);
        for (int r = 0; r < 5; r++) {
            int best = -1;
            for (int k = 0; k < 4096; k++) if (top[k].n && (best < 0 || top[k].n > top[best].n)) best = k;
            if (best < 0) break;
            fprintf(stderr, "    %8u blocks %8.1f MB  %08x %s\n", top[best].n, top[best].bytes / 1048576.0, top[best].ra, rt_func_name(top[best].ra));
            top[best].n = 0;
        }
    }
}
