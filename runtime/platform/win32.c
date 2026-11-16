// Windows implementation of platform.h: CreateThread, QueryPerformanceCounter, high-resolution waitable timers,
// SRW locks and condition variables.
// The parts: win32.c (threads, interruption, fibers, time, locks), win32_fs.c, win32_mem.c, win32_sys.c, win32_load.c.
#define _WIN32_WINNT 0x0A00   // Windows 10, which the game needs anyway (Vulkan 1.3): SetProcessInformation
#include <intrin.h>
#include <io.h>
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "platform.h"

#include "win32_int.h"

_Static_assert(sizeof(PlatMutex) == sizeof(SRWLOCK), "PlatMutex holds an SRWLOCK");
_Static_assert(sizeof(PlatCond) == sizeof(CONDITION_VARIABLE), "PlatCond holds a CONDITION_VARIABLE");

// ---- threads

struct PlatThread {
    HANDLE h;
    PlatThreadFn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    PlatThread *t = p;
    t->fn(t->arg);
    return 0;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, stack ? stack : PLAT_STACK_DEFAULT, thread_main, t,
                        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    if (name) {
        wchar_t w[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(t->h, w);
    }
    ResumeThread(t->h);
    return t;
}

void plat_thread_join(PlatThread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    if (WaitForSingleObject(t->h, ms >= INFINITE ? INFINITE - 1 : (DWORD)ms) != WAIT_OBJECT_0) return 0;
    CloseHandle(t->h);
    free(t);
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) {
    CloseHandle(t->h);
    t->h = NULL;
}

int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    DWORD_PTR proc, sys;
    *all = *first = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    *all = proc;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf = len ? malloc(len) : NULL;
    int cores = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, buf, &len))
        for (DWORD off = 0; off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)((char *)buf + off);
            KAFFINITY m = e->Processor.GroupMask[0].Mask & proc;
            if (e->Processor.GroupMask[0].Group == 0 && m) *first |= m & -m, cores++;   // the lowest of the core's
            off += e->Size;
        }
    free(buf);
    return cores;
}

// Each physical core's processors and efficiency class (PROCESSOR_RELATIONSHIP, Windows 10 and later: 0 on a CPU with
// one kind of core, higher on the faster cores of a hybrid one), group 0 only, within the process's affinity.
static int core_masks(uint64_t mask[64], uint8_t cls[64]) {
    DWORD_PTR proc, sys;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf = len ? malloc(len) : NULL;
    int n = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, buf, &len))
        for (DWORD off = 0; off < len && n < 64;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)((char *)buf + off);
            KAFFINITY m = e->Processor.GroupMask[0].Mask & proc;
            if (e->Processor.GroupMask[0].Group == 0 && m) mask[n] = m, cls[n++] = e->Processor.EfficiencyClass;
            off += e->Size;
        }
    free(buf);
    return n;
}

uint64_t plat_cpu_core_of(int cpu) {
    uint64_t mask[64];
    uint8_t cls[64];
    if (cpu < 0 || cpu > 63) return 0;
    int n = core_masks(mask, cls);
    for (int k = 0; k < n; k++)
        if (mask[k] >> cpu & 1) return mask[k];
    return 1ull << cpu;
}

uint64_t plat_cpu_fast(void) {
    uint64_t mask[64], fast = 0;
    uint8_t cls[64], top = 0;
    int n = core_masks(mask, cls);
    for (int k = 0; k < n; k++)
        if (cls[k] > top) top = cls[k];
    for (int k = 0; k < n; k++)
        if (cls[k] == top) fast |= mask[k];
    return fast;
}

int plat_process_affinity(uint64_t mask) { return SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask) != 0; }

uint64_t plat_thread_cpu_ns(uint32_t tid) {
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    FILETIME a, b, k, u;
    uint64_t ns = 0;
    if (th && GetThreadTimes(th, &a, &b, &k, &u))
        ns = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) *
             100;
    if (th) CloseHandle(th);
    return ns;
}

// QueryThreadCycleTime counts the thread's cycles at the TSC rate on current CPUs (GetThreadTimes ticks at ~15.6 ms,
// too coarse for one submission's recording); the rate is measured once against QueryPerformanceCounter, 20 ms busy.
uint64_t plat_self_cpu_ns(void) {
    static double ns_per_cycle;
    if (!ns_per_cycle) {
        LARGE_INTEGER f, q0, q;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        uint64_t c0 = __rdtsc();
        do QueryPerformanceCounter(&q); while ((q.QuadPart - q0.QuadPart) * 50 < f.QuadPart);
        ns_per_cycle = (double)(q.QuadPart - q0.QuadPart) * 1e9 / (double)f.QuadPart / (double)(__rdtsc() - c0);
    }
    ULONG64 c = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &c) ? (uint64_t)((double)c * ns_per_cycle) : 0;
}

// ---- thread interruption: SuspendThread, Get/SetThreadContext, ResumeThread.
struct PlatIntr {
    CONTEXT ctx;   // first: CONTEXT wants 16-byte alignment
    HANDLE h;
};

PlatIntr *plat_intr_self(void) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, GetCurrentThreadId());
    PlatIntr *t = h ? _aligned_malloc(sizeof *t, 16) : NULL;
    if (!t) {
        if (h) CloseHandle(h);
        return NULL;
    }
    t->h = h;
    return t;
}

void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    CloseHandle(t->h);
    _aligned_free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    if (SuspendThread(t->h) == (DWORD)-1) return 0;
    memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;   // set back whole, so the kernel restores every GPR
    if (!GetThreadContext(t->h, &t->ctx)) {
        ResumeThread(t->h);
        return 0;
    }
    *rip = t->ctx.Rip;
    *rsp = t->ctx.Rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    int ok = 1;
    if (to) {
        t->ctx.Rip = to;
        ok = SetThreadContext(t->h, &t->ctx) != 0;
    }
    ResumeThread(t->h);
    return ok;
}

// ---- fibers: Windows fibers with FIBER_FLAG_FLOAT_SWITCH (the MXCSR and x87 control word switch with them).
struct PlatFiber {
    void *h;
    void (*fn)(void *arg);
    void *arg;
};

static __thread PlatFiber self_fiber;

PlatFiber *plat_fiber_self(void) {
    self_fiber.h = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    return self_fiber.h ? &self_fiber : NULL;
}

void plat_fiber_unself(void) {
    ConvertFiberToThread();
    self_fiber.h = NULL;
}

static void WINAPI fiber_start(void *p) {
    PlatFiber *f = p;
    f->fn(f->arg);
}

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->fn = fn, f->arg = arg;
    f->h = CreateFiberEx(0x10000, stack, FIBER_FLAG_FLOAT_SWITCH, fiber_start, f);
    if (!f->h) {
        free(f);
        return NULL;
    }
    return f;
}

void plat_fiber_switch(PlatFiber *to) { SwitchToFiber(to->h); }

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    DeleteFiber(f->h);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    return (uint64_t)SetThreadAffinityMask(t ? t->h : GetCurrentThread(), (DWORD_PTR)mask);
}

uint32_t plat_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (uint32_t)n : 1;
}

// PLAT_PRIO_* are the THREAD_PRIORITY_* values.
void plat_thread_priority(int prio) { SetThreadPriority(GetCurrentThread(), prio); }
void plat_thread_set_priority(PlatThread *t, int prio) { SetThreadPriority(t->h, prio); }
int plat_thread_priority_get(void) { return GetThreadPriority(GetCurrentThread()); }

void plat_yield(void) { SwitchToThread(); }

// ---- time

static uint64_t qpc_freq(void) {
    static uint64_t freq;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

uint64_t plat_counter(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

uint64_t plat_counter_hz(void) { return qpc_freq(); }

// The default timer tick is 15.6 ms; a 1 ms wait would cost a full tick. Windows 11 also ignores timeBeginPeriod for a
// process whose window is minimized or hidden, and may run it on efficiency cores at a lower clock (EcoQoS): both are
// turned off for this process (the idea from NX2navi's runtime/platform/win32.c, the Win32 calls of Microsoft's docs).
void plat_timer_1ms(void) {
    PROCESS_POWER_THROTTLING_STATE s = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s);   // fails before Windows 10 1709
    timeBeginPeriod(1);
}

static PlatScale qpc_ns;   // set before main, so no thread reads it half written

__attribute__((constructor)) static void qpc_ns_init(void) { qpc_ns = plat_scale(1000000000u, qpc_freq()); }

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return plat_scaled(qpc_ns, (uint64_t)t.QuadPart);
}

// A high-resolution waitable timer per sleeping thread (Windows 10 1803+; one handle per thread, kept until exit)
// wakes within about 0.1-0.5 ms; the last PLAT_SPIN_NS are spent yielding so the deadline is met closely.
// Without that timer kind, Sleep(1) granularity applies.
#define PLAT_SPIN_NS 300000u

static __thread HANDLE sleep_timer;
static __thread int sleep_timer_tried;

void plat_wait_ns(uint64_t ns) {
    if (!sleep_timer_tried) {
        sleep_timer_tried = 1;
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(ns / 100)};   // relative, 100 ns units
    if (!due.QuadPart) due.QuadPart = -1;
    if (sleep_timer && SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0))
        WaitForSingleObject(sleep_timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) plat_wait_ns(ns - PLAT_SPIN_NS);
    while (plat_now_ns() < end) SwitchToThread();
}

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }

// ---- locks and condition variables

void plat_mutex_init(PlatMutex *m) { InitializeSRWLock((PSRWLOCK)m); }

// MKW_LOCK_PROF=1: time from finding a lock taken to getting it, per lock and caller, as linux_sync.c measures it, and
// how long a holder kept a lock some thread waited for, per the holder's caller (which section to shorten; wlp1: 2.5-6
// ms a frame of waits). Off: one load per contention.
#define LP_N 512
typedef struct { volatile uintptr_t a, ra, lk; volatile int64_t ns, n; } LpEnt;   // a: key of (lock, caller)
static LpEnt lp_wait[LP_N], lp_hold[LP_N];
static int lp_on = -1;

// The threads waiting for each contended lock: SRW locks do not tell the holder. Every unlock looks its lock up, so the
// probes are few: probing the whole table at each unlock once it was full stalled the load at frame ~150 (wlp2).
#define LW_N 4096
#define LW_PROBES 16
typedef struct { volatile uintptr_t lk; volatile int32_t n; } LwEnt;
static LwEnt lw[LW_N];

static volatile int32_t *lw_count(const void *m, int add) {
    uintptr_t a = (uintptr_t)m;
    for (uint32_t i = (uint32_t)((a >> 3) * 0x9E3779B1u) % LW_N, k = 0; k < LW_PROBES; k++, i = (i + 1) % LW_N) {
        uintptr_t cur = __atomic_load_n(&lw[i].lk, __ATOMIC_RELAXED);
        if (!cur && add && __atomic_compare_exchange_n(&lw[i].lk, &cur, a, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) cur = a;
        if (cur == a) return &lw[i].n;
        if (!cur) return NULL;
    }
    return NULL;
}

// The locks this thread holds exclusively since MKW_LOCK_PROF found its first contention, with when and by whom.
#define LH_DEPTH 16
static __thread struct { const void *m; uintptr_t ra; uint64_t t; } lh[LH_DEPTH];
static __thread int nlh;

static void lp_add(LpEnt *lp, const void *m, uint64_t t0, uintptr_t ra) {
    uintptr_t a = (uintptr_t)m, key = a ^ ra * 0x9E3779B97F4A7C15ull;
    uint64_t d = plat_now_ns() - t0;
    for (uint32_t i = (uint32_t)((key >> 3) * 0x9E3779B1u) % LP_N, k = 0; k < LP_N; k++, i = (i + 1) % LP_N) {
        uintptr_t cur = __atomic_load_n(&lp[i].a, __ATOMIC_RELAXED);
        if (!cur && __atomic_compare_exchange_n(&lp[i].a, &cur, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            cur = key, lp[i].ra = ra, lp[i].lk = a;
        if (cur == key && lp[i].ra == ra) {
            __atomic_fetch_add(&lp[i].ns, (int64_t)d, __ATOMIC_RELAXED), __atomic_fetch_add(&lp[i].n, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

int plat_lock_report(int holds, char *buf, int size) {
    LpEnt *lp = holds ? lp_hold : lp_wait;
    int k = 0;
    for (int top = 0; top < 10; top++) {   // the ten longest waits since the last report, then reset
        int best = -1;
        for (int i = 0; i < LP_N; i++)
            if (lp[i].a && lp[i].ns > 0 && (best < 0 || lp[i].ns > lp[best].ns)) best = i;
        if (best < 0) break;
        if (k < size)
            k += snprintf(buf + k, (size_t)(size - k), "%s exe+%#llx from exe+%#llx %.1f ms (%lld)", top ? ";" : "",
                          (unsigned long long)(lp[best].lk - plat_exe_base()),
                          (unsigned long long)(lp[best].ra - plat_exe_base()), lp[best].ns / 1e6, (long long)lp[best].n);
        lp[best].ns = -lp[best].ns;   // taken; restored to zero below
    }
    for (int i = 0; i < LP_N; i++) lp[i].ns = 0, lp[i].n = 0;
    return k;
}

static void lh_push(const void *m, uintptr_t ra) {
    if (nlh == LH_DEPTH) memmove(lh, lh + 1, sizeof lh - sizeof *lh), nlh--;   // one released by another thread
    lh[nlh].m = m, lh[nlh].ra = ra, lh[nlh++].t = plat_now_ns();
}

static inline __attribute__((always_inline)) void lock_at(PlatMutex *m, uintptr_t ra) {
    if (TryAcquireSRWLockExclusive((PSRWLOCK)m)) {
        if (lp_on > 0) lh_push(m, ra);
        return;
    }
    if (lp_on < 0) {
        const char *e = getenv("MKW_LOCK_PROF");
        lp_on = e && *e && *e != '0';
    }
    if (lp_on <= 0) {
        AcquireSRWLockExclusive((PSRWLOCK)m);
        return;
    }
    uint64_t t0 = plat_now_ns();
    volatile int32_t *w = lw_count(m, 1);
    if (w) __atomic_add_fetch(w, 1, __ATOMIC_RELAXED);
    AcquireSRWLockExclusive((PSRWLOCK)m);
    if (w) __atomic_sub_fetch(w, 1, __ATOMIC_RELAXED);
    lp_add(lp_wait, m, t0, ra);
    lh_push(m, ra);
}

void plat_mutex_lock(PlatMutex *m) { lock_at(m, (uintptr_t)__builtin_return_address(0)); }
void plat_mutex_lock_from(PlatMutex *m, uintptr_t ra) { lock_at(m, ra); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }

// MKW_LOCK_PROF: m leaves this thread's held list (counted as a hold when a thread waits for it); the caller that took
// it, for lh_push when a condition variable wait takes it back.
static uintptr_t lh_pop(const PlatMutex *m) {
    for (int i = nlh - 1; i >= 0; i--)
        if (lh[i].m == m) {
            volatile int32_t *w = lw_count(m, 0);
            if (w && __atomic_load_n(w, __ATOMIC_RELAXED)) lp_add(lp_hold, m, lh[i].t, lh[i].ra);
            uintptr_t ra = lh[i].ra;
            lh[i] = lh[--nlh];
            return ra;
        }
    return 0;
}

void plat_mutex_unlock(PlatMutex *m) {
    if (lp_on > 0) lh_pop(m);
    ReleaseSRWLockExclusive((PSRWLOCK)m);
}

void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)m); }
int plat_mutex_trylock_shared(PlatMutex *m) { return TryAcquireSRWLockShared((PSRWLOCK)m) != 0; }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }

void plat_cond_init(PlatCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
// The wait releases m inside SleepConditionVariableSRW: MKW_LOCK_PROF takes it off the held list first, or the sleep
// counted as a hold (wlp2c: G «held» ~10 ms at a time by WaitProcessWideKeyAtomic).
void plat_cond_wait(PlatCond *c, PlatMutex *m) { plat_cond_wait_ns(c, m, UINT64_MAX); }

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t ms = ns == UINT64_MAX ? INFINITE : (ns + 999999) / 1000000;
    DWORD to = ns == UINT64_MAX ? INFINITE : ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    uintptr_t ra = lp_on > 0 ? lh_pop(m) : 0;
    int r = SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, to, 0) ? 1 : GetLastError() != ERROR_TIMEOUT;
    if (ra) lh_push(m, ra);
    return r;
}

void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

