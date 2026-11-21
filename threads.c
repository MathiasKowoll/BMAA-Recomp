// Threads, and everything that waits: answered here, natively, and kept out of DecompRecomp's bridge.
//
// The bridge has no threads (its pthread_create says EAGAIN) and is entered under one lock (aot.c). A call that
// blocks must not be made under that lock, and a thread needs a CPU of its own: so pthreads, their mutexes and
// conditions, thread-specific data, semaphores, libdispatch's queues and the sleeps are these functions. Each is
// a strong I_<name>, which the translated code calls in place of the default that goes to the bridge.
//
// A guest thread is a host thread; a guest mutex or condition is a host one, found by the guest object's address
// (created the first time it is seen, which also covers the ones a program initialises statically).
#include "rt.h"
#include "tables.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void aot_thread_begin(CPU *c);
uint32_t guest_call_here(uint32_t addr, int nargs, const uint32_t *args);
int aot_is_main_thread(void);
uint32_t aot_stack_lo(void);
uint32_t aot_stack_hi(void);
uint32_t aot_malloc(uint32_t n);
void aot_free(uint32_t p);

#define RET(v) (c->eax = (uint32_t)(v))
#define RET64(v) do { uint64_t _v = (uint64_t)(v); c->eax = (uint32_t)_v; c->edx = (uint32_t)(_v >> 32); } while (0)
#define ARG64(i) ((uint64_t)ARG(i) | ((uint64_t)ARG((i) + 1) << 32))

// ---------- guest object -> host object ----------
typedef struct { uint32_t key; void *val; } Ent;
static Ent *tab; static uint32_t tabcap = 1u << 18;
static pthread_mutex_t tablock = PTHREAD_MUTEX_INITIALIZER;
static void *map_get(uint32_t key, void *(*mk)(void)) {
    pthread_mutex_lock(&tablock);
    if (!tab) tab = calloc(tabcap, sizeof(Ent));
    uint32_t h = (key * 2654435761u) & (tabcap - 1);
    while (tab[h].key && tab[h].key != key) h = (h + 1) & (tabcap - 1);
    void *v = tab[h].val;
    if (!tab[h].key && mk) { v = mk(); tab[h].key = key; tab[h].val = v; }
    pthread_mutex_unlock(&tablock);
    return v;
}
static void map_set(uint32_t key, void *v) {
    pthread_mutex_lock(&tablock);
    if (!tab) tab = calloc(tabcap, sizeof(Ent));
    uint32_t h = (key * 2654435761u) & (tabcap - 1);
    while (tab[h].key && tab[h].key != key) h = (h + 1) & (tabcap - 1);
    tab[h].key = key; tab[h].val = v;
    pthread_mutex_unlock(&tablock);
}

// ---------- mutexes ----------
// The guest's pthread_mutexattr_t keeps the type in its second word.
static pthread_mutex_t *new_mutex(int recursive) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    return m;
}
static void *mk_mutex(void) { return new_mutex(0); }
static pthread_mutex_t *MX(uint32_t g) { return map_get(g, mk_mutex); }
void I__pthread_mutexattr_init(CPU *c) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 0); RET(0); }
void I__pthread_mutexattr_destroy(CPU *c) { RET(0); }
void I__pthread_mutexattr_destroy_UNIX2003(CPU *c) { RET(0); }
void I__pthread_mutexattr_settype(CPU *c) { wr32(ARG(0) + 4, ARG(1)); RET(0); }
void I__pthread_mutex_init(CPU *c) {
    uint32_t attr = ARG(1);
    map_set(ARG(0), new_mutex(attr && rd32(attr + 4) == PTHREAD_MUTEX_RECURSIVE));
    RET(0);
}
void I__pthread_mutex_destroy(CPU *c) { RET(0); }
void I__pthread_mutex_lock(CPU *c) { RET(pthread_mutex_lock(MX(ARG(0)))); }
// BMAA_TRYLOCK_STATS=1: the try-locks that did not get the mutex, by who asked (PhysX drops the call it guards)
void I__pthread_mutex_trylock(CPU *c) {
    int r = pthread_mutex_trylock(MX(ARG(0)));
    static int on = -1; if (on < 0) on = getenv("BMAA_TRYLOCK_STATS") != NULL;
    if (on) {
        static _Atomic unsigned tries, fails;
        tries++;
        if (r) { unsigned n = ++fails; if (n <= 400 || n % 500 == 0) fprintf(stderr, "  [trylock] busy #%u of %u tries, asked from %08x\n", n, (unsigned)tries, rd32(c->ebp + 4)); }
    }
    RET(r);
}
void I__pthread_mutex_unlock(CPU *c) { RET(pthread_mutex_unlock(MX(ARG(0)))); }

// ---------- conditions ----------
static void *mk_cond(void) { pthread_cond_t *v = malloc(sizeof *v); pthread_cond_init(v, NULL); return v; }
static pthread_cond_t *CV(uint32_t g) { return map_get(g, mk_cond); }
void I__pthread_cond_init(CPU *c) { map_set(ARG(0), mk_cond()); RET(0); }
void I__pthread_cond_destroy(CPU *c) { RET(0); }
void I__pthread_cond_signal(CPU *c) { RET(pthread_cond_signal(CV(ARG(0)))); }
void I__pthread_cond_broadcast(CPU *c) { RET(pthread_cond_broadcast(CV(ARG(0)))); }
void I__pthread_cond_wait(CPU *c) { RET(pthread_cond_wait(CV(ARG(0)), MX(ARG(1)))); }
// abstime: a 32-bit timespec. The physics library's wait (sub_27a3530) builds it as "now plus so many milliseconds"
// without carrying the nanoseconds into the seconds, so about half the time tv_nsec is a billion or more. The 32-bit
// pthread_cond_timedwait of the game's time took that as it was meant; today's refuses it at once (EINVAL), and a
// wait that was to last until the simulation step is done came back immediately. The carry is made here.
void I__pthread_cond_timedwait(CPU *c) {
    int64_t sec = (int32_t)rd32(ARG(2)), nsec = (int32_t)rd32(ARG(2) + 4);
    if (nsec >= 1000000000) { sec += nsec / 1000000000; nsec %= 1000000000; }
    if (nsec < 0) nsec = 0;
    struct timespec ts = { (time_t)sec, (long)nsec };
    RET(pthread_cond_timedwait(CV(ARG(0)), MX(ARG(1)), &ts));
}

// ---------- threads ----------
// A guest pthread_t is a small number. The guest's pthread_attr_t keeps the stack size in its second word.
typedef struct { pthread_t host; CPU *cpu; uint32_t start, arg, ret; } GThread;
#define NTHREADS 512
static GThread threads[NTHREADS];
static int nthreads = 1;                      // 1 is the main thread
static _Thread_local uint32_t me = 0;
static uint32_t self(void) { if (!me) me = aot_is_main_thread() ? 1 : (uint32_t)__atomic_add_fetch(&nthreads, 1, __ATOMIC_SEQ_CST); return me; }
static void *thread_main(void *p) {
    GThread *t = p;
    me = (uint32_t)(t - threads) + 1;
    aot_thread_begin(t->cpu);
    uint32_t a[1] = { t->arg };
    t->ret = guest_call(t->cpu, t->start, 1, a);
    return NULL;
}
void I__pthread_attr_init(CPU *c) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 512 * 1024); RET(0); }
void I__pthread_attr_destroy(CPU *c) { RET(0); }
void I__pthread_attr_setstacksize(CPU *c) { wr32(ARG(0) + 4, ARG(1)); RET(0); }
void I__pthread_create(CPU *c) {   // (pthread_t *thread, attr, start, arg)
    int h = __atomic_add_fetch(&nthreads, 1, __ATOMIC_SEQ_CST) - 1;
    if (h >= NTHREADS) { RET(EAGAIN); return; }
    GThread *t = &threads[h];
    uint32_t ss = ARG(1) ? rd32(ARG(1) + 4) : 512 * 1024;
    if (ss < (1u << 20)) ss = 1u << 20;
    t->cpu = cpu_new(ss);
    t->start = ARG(2); t->arg = ARG(3);
    // Translated code nests on the host's stack as deep as the guest does on its own, with larger frames.
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);   // a game's threads: performance cores, full speed
    int r = pthread_create(&t->host, &a, thread_main, t);
    if (!r && ARG(0)) wr32(ARG(0), (uint32_t)h + 1);
    fprintf(stderr, "  [hilos] hilo %d creado, arranca en %s (0x%08x), pila de %u KB\n", h + 1, rt_func_name(t->start), t->start, ss >> 10);
    RET(r);
}
void I__pthread_join(CPU *c) {
    uint32_t h = ARG(0);
    if (h < 2 || h > NTHREADS) { RET(ESRCH); return; }
    int r = pthread_join(threads[h - 1].host, NULL);
    if (ARG(1)) wr32(ARG(1), threads[h - 1].ret);
    RET(r);
}
void I__pthread_detach(CPU *c) { RET(0); }
void I__pthread_exit(CPU *c) { pthread_exit(NULL); }
void I__pthread_self(CPU *c) { RET(self()); }
void I__pthread_equal(CPU *c) { RET(ARG(0) == ARG(1)); }
void I__pthread_main_np(CPU *c) { RET(aot_is_main_thread()); }
void I__pthread_mach_thread_np(CPU *c) { RET(0x1000 + ARG(0)); }
void I__pthread_setname_np(CPU *c) { if (ARG(0)) pthread_setname_np((const char *)G2H(ARG(0))); RET(0); }
void I__pthread_getname_np(CPU *c) { if (ARG(1) && ARG(2)) wr8(ARG(1), 0); RET(0); }
void I__pthread_get_stackaddr_np(CPU *c) { RET(aot_stack_hi()); }      // of the calling thread: that is who asks
void I__pthread_get_stacksize_np(CPU *c) { RET(aot_stack_hi() - aot_stack_lo()); }
void I__pthread_getschedparam(CPU *c) { if (ARG(1)) wr32(ARG(1), SCHED_OTHER); if (ARG(2)) wr32(ARG(2), 31); RET(0); }
void I__pthread_setschedparam(CPU *c) { RET(0); }
void I__sched_get_priority_max(CPU *c) { RET(sched_get_priority_max((int)ARG(0))); }
void I__sched_get_priority_min(CPU *c) { RET(sched_get_priority_min((int)ARG(0))); }
void I__pthread_kill(CPU *c) { RET(0); }
void I__pthread_cancel(CPU *c) { RET(0); }
void I__sched_yield(CPU *c) { RET(sched_yield()); }
void I__usleep(CPU *c) { RET(usleep(ARG(0))); }
void I__usleep_UNIX2003(CPU *c) { RET(usleep(ARG(0))); }
void I__sleep(CPU *c) { RET(sleep(ARG(0))); }
void I__nanosleep(CPU *c) {
    struct timespec ts = { (time_t)(int32_t)rd32(ARG(0)), (long)(int32_t)rd32(ARG(0) + 4) };
    RET(nanosleep(&ts, NULL));
}

// ---------- thread-specific data ----------
// A guest key is an index into the host's keys; the values are guest words. A guest destructor is not run.
#define NKEYS 1024
static pthread_key_t hkeys[NKEYS];
static int nkeys = 1;   // 0 is never handed out: programs test a key against zero
void I__pthread_key_create(CPU *c) {
    int k = __atomic_fetch_add(&nkeys, 1, __ATOMIC_SEQ_CST);
    if (k >= NKEYS || pthread_key_create(&hkeys[k], NULL)) { RET(EAGAIN); return; }
    wr32(ARG(0), (uint32_t)k); RET(0);
}
void I__pthread_key_delete(CPU *c) { RET(0); }
void I__pthread_setspecific(CPU *c) { uint32_t k = ARG(0); RET(k && k < NKEYS ? pthread_setspecific(hkeys[k], (void *)(uintptr_t)ARG(1)) : EINVAL); }
void I__pthread_getspecific(CPU *c) { uint32_t k = ARG(0); RET(k && k < NKEYS ? (uint32_t)(uintptr_t)pthread_getspecific(hkeys[k]) : 0); }

// pthread_once_t is {long sig; char opaque[4]}; the second word is ours, 1 once the routine has run. One lock for
// all of them, recursive, because a routine may reach another once.
static pthread_mutex_t once_lock;
static pthread_once_t once_lock_init = PTHREAD_ONCE_INIT;
static void once_lock_make(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&once_lock, &a);
}
void I__pthread_once(CPU *c) {
    uint32_t once = ARG(0), fn = ARG(1);
    pthread_once(&once_lock_init, once_lock_make);
    pthread_mutex_lock(&once_lock);
    if (rd32(once + 4) != 1) { wr32(once + 4, 1); guest_call_here(fn, 0, NULL); }
    pthread_mutex_unlock(&once_lock);
    RET(0);
}

// ---------- libdispatch ----------
// Queues, groups and semaphores are the host's; the guest holds a token for each (0xD15C0000 + n). The main
// queue is whatever queue argument is not one of our tokens: the guest names it by the address of a data symbol.
#define TOKEN 0xD15C0000u
#define NOBJ 4096
static void *objs[NOBJ];
static int nobj = 1;
static uint32_t token(void *o) { int k = __atomic_fetch_add(&nobj, 1, __ATOMIC_SEQ_CST); if (k >= NOBJ) abort(); objs[k] = o; return TOKEN + (uint32_t)k; }
static void *obj(uint32_t t) { return (t > TOKEN && t < TOKEN + NOBJ) ? objs[t - TOKEN] : NULL; }
static dispatch_queue_t queue(uint32_t t) { void *o = obj(t); return o ? (__bridge dispatch_queue_t)o : dispatch_get_main_queue(); }

// A block is {isa, flags, reserved, invoke, descriptor}; invoke takes the block. One that is not global lives in
// its caller's frame, so a block handed to a queue is copied first: descriptor = {reserved, size, copy, dispose},
// the two helpers there only with flag 1 << 25.
#define BLOCK_GLOBAL (1u << 28)
#define BLOCK_HELPERS (1u << 25)
static uint32_t block_copy(uint32_t b) {
    uint32_t flags = rd32(b + 4);
    if (flags & BLOCK_GLOBAL) return b;
    uint32_t desc = rd32(b + 16), size = rd32(desc + 4);
    if (size < 20 || size > 4096) size = 64;
    uint32_t d = aot_malloc(size);
    memcpy(G2H(d), G2H(b), size);
    if (flags & BLOCK_HELPERS) { uint32_t a[2] = { d, b }; guest_call_here(rd32(desc + 8), 2, a); }
    return d;
}
static void block_run_and_drop(uint32_t b) {
    uint32_t a[1] = { b };
    guest_call_here(rd32(b + 12), 1, a);
    uint32_t flags = rd32(b + 4);
    if (flags & BLOCK_GLOBAL) return;
    if (flags & BLOCK_HELPERS) guest_call_here(rd32(rd32(b + 16) + 12), 1, a);
    aot_free(b);
}
void I__dispatch_get_global_queue(CPU *c) {
    static uint32_t t[4];
    int p = (int32_t)ARG(0), k = p > 0 ? 0 : p == 0 ? 1 : p == -2 ? 2 : 3;
    if (!t[k]) t[k] = token((__bridge void *)dispatch_get_global_queue(p, 0));
    RET(t[k]);
}
void I__dispatch_queue_create(CPU *c) {
    dispatch_queue_t q = dispatch_queue_create(ARG(0) ? (const char *)G2H(ARG(0)) : NULL, DISPATCH_QUEUE_SERIAL);
    RET(token((__bridge_retained void *)q));
}
void I__dispatch_async(CPU *c) { uint32_t b = block_copy(ARG(1)); dispatch_async(queue(ARG(0)), ^{ block_run_and_drop(b); }); }
void I__dispatch_sync(CPU *c) {
    uint32_t b = ARG(1); dispatch_queue_t q = queue(ARG(0));
    if (q == dispatch_get_main_queue() && aot_is_main_thread()) { uint32_t a[1] = { b }; guest_call_here(rd32(b + 12), 1, a); return; }
    dispatch_sync(q, ^{ uint32_t a[1] = { b }; guest_call_here(rd32(b + 12), 1, a); });
}
void I__dispatch_async_f(CPU *c) { uint32_t ctx = ARG(1), fn = ARG(2); dispatch_async(queue(ARG(0)), ^{ uint32_t a[1] = { ctx }; guest_call_here(fn, 1, a); }); }
void I__dispatch_sync_f(CPU *c) {
    uint32_t ctx = ARG(1), fn = ARG(2); dispatch_queue_t q = queue(ARG(0));
    if (q == dispatch_get_main_queue() && aot_is_main_thread()) { uint32_t a[1] = { ctx }; guest_call_here(fn, 1, a); return; }
    dispatch_sync(q, ^{ uint32_t a[1] = { ctx }; guest_call_here(fn, 1, a); });
}
void I__dispatch_group_create(CPU *c) { RET(token((__bridge_retained void *)dispatch_group_create())); }
void I__dispatch_group_async(CPU *c) {
    uint32_t b = block_copy(ARG(2));
    dispatch_group_async((__bridge dispatch_group_t)obj(ARG(0)), queue(ARG(1)), ^{ block_run_and_drop(b); });
}
void I__dispatch_group_wait(CPU *c) { RET(dispatch_group_wait((__bridge dispatch_group_t)obj(ARG(0)), ARG64(1)) ? 1 : 0); }
void I__dispatch_semaphore_create(CPU *c) { RET(token((__bridge_retained void *)dispatch_semaphore_create((long)(int32_t)ARG(0)))); }
// BMAA_NO_SUSPEND=1 (for testing while the Mac is used for something else): the game suspends itself when it stops
// being the application in front, by taking a semaphore its main loop needs (sub_1bc51d0) and giving it back when
// it comes forward again (sub_1bc52b0). With the variable those two do neither, and the game keeps running behind.
static int no_suspend(void) { static int v = -1; if (v < 0) v = getenv("BMAA_NO_SUSPEND") != NULL; return v; }
void I__dispatch_semaphore_wait(CPU *c) {
    if (no_suspend() && rd32(c->esp) == 0x1bc5286u) { RET(0); return; }
    RET(dispatch_semaphore_wait((__bridge dispatch_semaphore_t)obj(ARG(0)), ARG64(1)) ? 1 : 0);
}
void I__dispatch_semaphore_signal(CPU *c) {
    if (no_suspend() && rd32(c->esp) == 0x1bc5365u) { RET(0); return; }
    RET(dispatch_semaphore_signal((__bridge dispatch_semaphore_t)obj(ARG(0))) ? 1 : 0);
}
void I__dispatch_time(CPU *c) { RET64(dispatch_time(ARG64(0), (int64_t)ARG64(2))); }
void I__dispatch_release(CPU *c) {}   // the host objects are kept: a handful, for as long as the game runs
void I__dispatch_retain(CPU *c) {}
void I__dispatch_once(CPU *c) {
    uint32_t pred = ARG(0), block = ARG(1);
    pthread_once(&once_lock_init, once_lock_make);
    pthread_mutex_lock(&once_lock);
    if (rd32(pred) == 0) { uint32_t a[1] = { block }; guest_call_here(rd32(block + 12), 1, a); wr32(pred, 0xFFFFFFFFu); }
    pthread_mutex_unlock(&once_lock);
}

// ---------- named semaphores ----------
void I__sem_open(CPU *c) {   // (name, oflag, mode, value)
    sem_t *s = sem_open((const char *)G2H(ARG(0)), (int)ARG(1), (mode_t)ARG(2), ARG(3));
    RET(s == SEM_FAILED ? 0xFFFFFFFFu : token(s));
}
void I__sem_wait(CPU *c) { RET(sem_wait(obj(ARG(0)))); }
void I__sem_post(CPU *c) { RET(sem_post(obj(ARG(0)))); }
void I__sem_close(CPU *c) { RET(sem_close(obj(ARG(0)))); }
void I__sem_unlink(CPU *c) { RET(sem_unlink((const char *)G2H(ARG(0)))); }

// ---------- OSAtomic, spin locks, the memory barrier ----------
// DecompRecomp answers these for its single-threaded interpreter: a compare-and-swap that reads and then writes, a
// spin lock that writes 1 without waiting for anybody, a memory barrier that does nothing. With the game's threads
// really running at once that is no exclusion at all (the cloth of the cape, simulated on several threads, came out
// torn). Here they are the real operations on the game's memory, and the barrier is a barrier: on ARM it is needed.
#include <sched.h>
#define A32(g) ((uint32_t *)G2H(g))
// bool OSAtomicCompareAndSwap32/Int/Ptr[Barrier](old, new, address)
static void cas32(CPU *c) { uint32_t exp = ARG(0); RET(ARG(2) ? __atomic_compare_exchange_n(A32(ARG(2)), &exp, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) : 0); }
void I__OSAtomicCompareAndSwap32(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwap32Barrier(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwapInt(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwapIntBarrier(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwapPtr(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwapPtrBarrier(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwapLong(CPU *c) { cas32(c); }
void I__OSAtomicCompareAndSwapLongBarrier(CPU *c) { cas32(c); }
// int32_t OSAtomicAdd32[Barrier](amount, address): the new value
static void add32(CPU *c) { RET(ARG(1) ? __atomic_add_fetch(A32(ARG(1)), ARG(0), __ATOMIC_SEQ_CST) : 0); }
void I__OSAtomicAdd32(CPU *c) { add32(c); }
void I__OSAtomicAdd32Barrier(CPU *c) { add32(c); }
void I__OSAtomicIncrement32(CPU *c) { RET(ARG(0) ? __atomic_add_fetch(A32(ARG(0)), 1, __ATOMIC_SEQ_CST) : 0); }
void I__OSAtomicIncrement32Barrier(CPU *c) { I__OSAtomicIncrement32(c); }
void I__OSAtomicDecrement32(CPU *c) { RET(ARG(0) ? __atomic_sub_fetch(A32(ARG(0)), 1, __ATOMIC_SEQ_CST) : 0); }
void I__OSAtomicDecrement32Barrier(CPU *c) { I__OSAtomicDecrement32(c); }
// int64_t OSAtomicAdd64[Barrier](amount (two words), address)
static void add64(CPU *c) { RET64(ARG(2) ? __atomic_add_fetch((uint64_t *)G2H(ARG(2)), ARG64(0), __ATOMIC_SEQ_CST) : 0); }
void I__OSAtomicAdd64(CPU *c) { add64(c); }
void I__OSAtomicAdd64Barrier(CPU *c) { add64(c); }
// OSSpinLock: a word, 0 when free
void I__OSSpinLockLock(CPU *c) {
    uint32_t *l = A32(ARG(0));
    for (unsigned n = 0;; n++) {
        uint32_t free_ = 0;
        if (__atomic_compare_exchange_n(l, &free_, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        if (n > 100) sched_yield();
    }
    RET(0);
}
void I__OSSpinLockTry(CPU *c) { uint32_t free_ = 0; RET(__atomic_compare_exchange_n(A32(ARG(0)), &free_, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); }
void I__OSSpinLockUnlock(CPU *c) { __atomic_store_n(A32(ARG(0)), 0, __ATOMIC_SEQ_CST); RET(0); }
void I__spin_lock(CPU *c) { I__OSSpinLockLock(c); }
void I__spin_lock_try(CPU *c) { I__OSSpinLockTry(c); }
void I__spin_unlock(CPU *c) { I__OSSpinLockUnlock(c); }
void I__OSMemoryBarrier(CPU *c) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
