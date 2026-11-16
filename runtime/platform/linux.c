// Linux implementation of platform.h: pthreads, CLOCK_MONOTONIC, futex locks and condition variables, POSIX files,
// /proc and sysfs for the load counters, posix_spawn for child processes.
// The parts: linux.c (threads, interruption, fibers), linux_sync.c, linux_fs.c, linux_mem.c, linux_sys.c.
#ifndef _GNU_SOURCE   // build.py passes it on Linux
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL_clipboard.h>

#include "platform.h"
#include "linux_int.h"
extern char **environ;

// ---- threads

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    volatile int32_t tid;   // the kernel's id once the thread runs, for plat_thread_set_priority
    char name[16];          // the kernel keeps 15 characters
};

// A signal stack for the calling thread, so the fault and crash handlers still run when its own stack is exhausted.
// Never freed: threads that end are few, and a stack freed while a handler might run on it would be worse.
void alt_stack(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    size_t size = 64 << 10;   // a signal frame with the AVX-512 state takes several KiB
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {.ss_sp = p, .ss_size = size};
    sigaltstack(&ss, NULL);
}

static void *thread_main(void *p) {
    PlatThread *t = p;
    alt_stack();
    __atomic_store_n(&t->tid, (int32_t)syscall(SYS_gettid), __ATOMIC_RELEASE);
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    // The stack is mapped lazily, as Windows reserves it: only the pages a thread touches cost memory.
    pthread_attr_setstacksize(&a, stack ? stack : PLAT_STACK_DEFAULT);
    int e = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}


// The physical core of each logical processor is in /sys/devices/system/cpu/cpu<n>/topology/core_id together with its
// package; the lowest-numbered processor of each (package, core) pair is the core's first.
int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    cpu_set_t set;
    *all = *first = 0;
    if (sched_getaffinity(0, sizeof set, &set)) return 0;
    int cores = 0;
    uint64_t seen[64][2];
    for (int i = 0; i < 64; i++) {
        if (!CPU_ISSET(i, &set)) continue;
        *all |= 1ull << i;
        uint64_t core = 0, pkg = 0;
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        if (!read_u64_file(path, &core)) core = (uint64_t)i;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        read_u64_file(path, &pkg);
        int k = 0;
        while (k < cores && (seen[k][0] != core || seen[k][1] != pkg)) k++;
        if (k == cores) seen[cores][0] = core, seen[cores][1] = pkg, cores++, *first |= 1ull << i;
    }
    return cores;
}

static int core_key(int i, uint64_t *core, uint64_t *pkg) {
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
    if (!read_u64_file(path, core)) return 0;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
    *pkg = 0;
    read_u64_file(path, pkg);
    return 1;
}

// SMT siblings are not numbered together everywhere: Intel's are (0 and 1), AMD's on Linux are not (0 and 16 on a
// 16-core Ryzen), so they are matched by core_id and package as plat_cpu_cores does.
uint64_t plat_cpu_core_of(int cpu) {
    uint64_t core, pkg, c, p, m = 0;
    if (cpu < 0 || cpu > 63 || !core_key(cpu, &core, &pkg)) return cpu >= 0 && cpu < 64 ? 1ull << cpu : 0;
    for (int i = 0; i < 64; i++)
        if (core_key(i, &c, &p) && c == core && p == pkg) m |= 1ull << i;
    return m | 1ull << cpu;
}

// cpufreq's cpuinfo_max_freq per processor: on an i7-14700F 5300-5400 MHz on the performance cores and 4200 on the
// efficiency cores. Those within 10 % of the highest count as fast: AMD's cores of one kind differ by a few percent
// (preferred cores), its Zen 5c cores by a third.
uint64_t plat_cpu_fast(void) {
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set)) return 0;
    uint64_t freq[64] = {0}, top = 0, all = 0, fast = 0;
    for (int i = 0; i < 64; i++) {
        if (!CPU_ISSET(i, &set)) continue;
        all |= 1ull << i;
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (!read_u64_file(path, &freq[i]) || !freq[i]) return all;   // no cpufreq (a VM): all alike
        if (freq[i] > top) top = freq[i];
    }
    for (int i = 0; i < 64; i++)
        if (all >> i & 1 && freq[i] * 10 >= top * 9) fast |= 1ull << i;
    return fast;
}

// Linux has no process-wide affinity: each thread of /proc/self/task gets the mask, and threads started later inherit
// it from the thread that starts them.
int plat_process_affinity(uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    int ok = 1;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.' && sched_setaffinity((pid_t)atoi(e->d_name), sizeof set, &set)) ok = 0;
    closedir(d);
    return ok;
}

uint64_t plat_self_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// The first field of /proc/self/task/<tid>/schedstat is the time the thread has run, in ns.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%u/schedstat", tid);
    FILE *f = fopen(path, "r");
    unsigned long long ns = 0;
    if (f) {
        if (fscanf(f, "%llu", &ns) != 1) ns = 0;
        fclose(f);
    }
    return ns;
}

// ---- thread interruption: a signal handshake in place of Windows' SuspendThread / Get/SetThreadContext.
// The caller moves the handle from IDLE to REQ and signals the thread. The thread's handler stores where it was and
// moves REQ to STOPPED, then spins until the caller moves it on to GO (with the redirect in to), changes its own
// return context, and goes back to IDLE. A caller that sees no STOPPED within INTR_WAIT_NS (the thread is blocked in
// the kernel and runs no handler) takes REQ back to IDLE; a handler that then finds IDLE returns at once.
enum { INTR_IDLE, INTR_REQ, INTR_STOPPED, INTR_GO };
#define INTR_WAIT_NS 200000u

struct PlatIntr {
    pthread_t pt;
    volatile int32_t state;
    uint64_t rip, rsp, to;
};

static __thread PlatIntr *intr_me;
static int intr_sig;

static void intr_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    PlatIntr *t = intr_me;
    ucontext_t *u = uc;
    if (!t) return;
#if defined(__aarch64__)
    t->rip = (uint64_t)u->uc_mcontext.pc;
    t->rsp = (uint64_t)u->uc_mcontext.sp;
#else
    t->rip = (uint64_t)u->uc_mcontext.gregs[REG_RIP];
    t->rsp = (uint64_t)u->uc_mcontext.gregs[REG_RSP];
#endif
    int32_t req = INTR_REQ;
    if (!__atomic_compare_exchange_n(&t->state, &req, INTR_STOPPED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_GO) plat_pause();
#if defined(__aarch64__)
    if (t->to) u->uc_mcontext.pc = t->to;
#else
    if (t->to) u->uc_mcontext.gregs[REG_RIP] = (greg_t)t->to;
#endif
    __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
}

PlatIntr *plat_intr_self(void) {
    static PlatMutex install = PLAT_MUTEX_INIT;
    if (!__atomic_load_n(&intr_sig, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&install);
        if (!intr_sig) {
            int sig = SIGRTMIN + 4;   // glibc keeps the first real-time signals for itself; SIGRTMIN skips them
            struct sigaction sa = {.sa_sigaction = intr_handler, .sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK};
            sigemptyset(&sa.sa_mask);
            if (!sigaction(sig, &sa, NULL)) __atomic_store_n(&intr_sig, sig, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&install);
    }
    if (!intr_sig) return NULL;
    PlatIntr *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->pt = pthread_self();
    intr_me = t;
    return t;
}

// Called by the thread itself as it ends, or by another once the thread no longer runs.
void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    if (intr_me == t) intr_me = NULL;
    free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    int32_t idle = INTR_IDLE;
    if (!__atomic_compare_exchange_n(&t->state, &idle, INTR_REQ, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (pthread_kill(t->pt, intr_sig)) {
        __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
        return 0;
    }
    uint64_t end = plat_now_ns() + INTR_WAIT_NS;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_STOPPED) {
        if (plat_now_ns() < end) {
            plat_pause();
            continue;
        }
        int32_t req = INTR_REQ;
        if (__atomic_compare_exchange_n(&t->state, &req, INTR_IDLE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
    *rip = t->rip;
    *rsp = t->rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    t->to = to;
    __atomic_store_n(&t->state, INTR_GO, __ATOMIC_RELEASE);
    return 1;
}

// ---- fibers: a stack switch of our own. glibc's swapcontext saves and restores the signal mask with a system call
// on every switch, and the game switches fibers often. fiber_swap saves the callee-saved registers, the MXCSR and the
// x87 control word on the current stack, stores the stack pointer in *save and loads the other one. A new fiber's
// stack is laid out as fiber_swap leaves one, returning into fiber_entry, which calls fn(arg) (r12 and r13).
struct PlatFiber {
    void *sp;
    void *stack;   // mapped stack (NULL for a thread's own context)
    size_t size;
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
#if defined(__aarch64__)
// AArch64: x19-x29, x30 (the return address), d8-d15 and FPCR in a 176-byte frame (FIBER_FRAME_WORDS words):
// 0 x19 x20, 16 x21 x22, 32 x23 x24, 48 x25 x26, 64 x27 x28, 80 x29 x30, 96 d8-d15, 160 FPCR. A new fiber's frame
// returns into plat_fiber_entry with fn in x19 and arg in x20.
#define FIBER_FRAME_WORDS 22
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, %function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    sub sp, sp, #176\n"
        "    stp x19, x20, [sp, #0]\n"
        "    stp x21, x22, [sp, #16]\n"
        "    stp x23, x24, [sp, #32]\n"
        "    stp x25, x26, [sp, #48]\n"
        "    stp x27, x28, [sp, #64]\n"
        "    stp x29, x30, [sp, #80]\n"
        "    stp d8, d9, [sp, #96]\n"
        "    stp d10, d11, [sp, #112]\n"
        "    stp d12, d13, [sp, #128]\n"
        "    stp d14, d15, [sp, #144]\n"
        "    mrs x9, fpcr\n"
        "    str x9, [sp, #160]\n"
        "    mov x9, sp\n"
        "    str x9, [x0]\n"
        "    mov sp, x1\n"
        "    ldr x9, [sp, #160]\n"
        "    msr fpcr, x9\n"
        "    ldp x19, x20, [sp, #0]\n"
        "    ldp x21, x22, [sp, #16]\n"
        "    ldp x23, x24, [sp, #32]\n"
        "    ldp x25, x26, [sp, #48]\n"
        "    ldp x27, x28, [sp, #64]\n"
        "    ldp x29, x30, [sp, #80]\n"
        "    ldp d8, d9, [sp, #96]\n"
        "    ldp d10, d11, [sp, #112]\n"
        "    ldp d12, d13, [sp, #128]\n"
        "    ldp d14, d15, [sp, #144]\n"
        "    add sp, sp, #176\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, %function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    mov x0, x20\n"
        "    blr x19\n"
        "    brk #0\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");
#else
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    movq %r13, %rdi\n"
        "    andq $-16, %rsp\n"   // the ret here left it 8 off; SysV wants 16 before the call (NX2navi e9edd6a)
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");
#endif

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    // Reserved, not committed; the lowest page stays inaccessible, so an overflow faults instead of corrupting.
    void *m = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) {
        free(f);
        return NULL;
    }
    mprotect(m, 4096, PROT_NONE);
    f->stack = m, f->size = stack;
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
#if defined(__aarch64__)
    uint64_t *sp = top - FIBER_FRAME_WORDS;   // plat_fiber_swap's frame, all zero (FPCR: the default) but these
    memset(sp, 0, FIBER_FRAME_WORDS * sizeof *sp);
    sp[0] = (uint64_t)(uintptr_t)fn;    // x19
    sp[1] = (uint64_t)(uintptr_t)arg;   // x20
    sp[11] = (uint64_t)(uintptr_t)plat_fiber_entry;   // x30
    f->sp = sp;
    return f;
#else
    uint64_t *sp = top - 10;   // fpu state (2 words), r15, r14, r13, r12, rbx, rbp, return address, padding
    sp[0] = 0x037f;            // x87 control word: the default
    sp[1] = 0x1f80;            // MXCSR: the default
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = 0;
    f->sp = sp;
    return f;
#endif
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;   // on this thread; the switch back into from sets it on whichever thread that is
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);   // pthread_timedjoin_np takes a CLOCK_REALTIME deadline
    uint64_t end = (uint64_t)at.tv_sec * 1000000000u + (uint64_t)at.tv_nsec + ns;
    at.tv_sec = (time_t)(end / 1000000000u), at.tv_nsec = (long)(end % 1000000000u);
    if (pthread_timedjoin_np(t->t, NULL, &at)) return 0;
    free(t);
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) { pthread_detach(t->t); }

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    pthread_t th = t ? t->t : pthread_self();
    cpu_set_t old, set;
    if (pthread_getaffinity_np(th, sizeof old, &old)) return 0;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    if (pthread_setaffinity_np(th, sizeof set, &set)) return 0;
    uint64_t prev = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &old)) prev |= 1ull << i;
    return prev;
}

uint32_t plat_cpu_count(void) {
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof set, &set) && CPU_COUNT(&set) > 0) return (uint32_t)CPU_COUNT(&set);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

// Windows' levels map to nice values: IDLE 19, LOWEST 4, BELOW_NORMAL 2, NORMAL 0, ABOVE_NORMAL -2, HIGHEST -5,
// TIME_CRITICAL -10. Applied only when the process may lower a nice value again (platform.h says why).
static int nice_of(int prio) {
    return prio <= PLAT_PRIO_IDLE ? 19 : prio == PLAT_PRIO_LOWEST ? 4 : prio == PLAT_PRIO_BELOW_NORMAL ? 2
         : prio == PLAT_PRIO_NORMAL ? 0 : prio == PLAT_PRIO_ABOVE_NORMAL ? -2 : prio == PLAT_PRIO_HIGHEST ? -5 : -10;
}

static int may_renice(int nice) {
    static int lowest = 100;   // the lowest nice value this process may set; 100 until probed
    if (lowest == 100) {
        struct rlimit r;
        int l = !getrlimit(RLIMIT_NICE, &r) && r.rlim_cur != RLIM_INFINITY ? 20 - (int)r.rlim_cur : -20;
        // CAP_SYS_NICE shows as being allowed to set a nice value below the limit's
        pid_t tid = (pid_t)syscall(SYS_gettid);
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, (id_t)tid);
        if (!errno && l > cur - 1 && !setpriority(PRIO_PROCESS, (id_t)tid, cur - 1)) {
            setpriority(PRIO_PROCESS, (id_t)tid, cur);
            l = -20;
        }
        __atomic_store_n(&lowest, l, __ATOMIC_RELAXED);
    }
    return lowest <= 0 && nice >= lowest;
}

static void renice(pid_t tid, int prio) {
    int nice = nice_of(prio);
    if (tid > 0 && may_renice(nice)) setpriority(PRIO_PROCESS, (id_t)tid, nice);
}

void plat_thread_priority(int prio) { renice((pid_t)syscall(SYS_gettid), prio); }

void plat_thread_set_priority(PlatThread *t, int prio) { renice(__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE), prio); }

int plat_thread_priority_get(void) {
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno) return PLAT_PRIO_NORMAL;
    return nice >= 19 ? PLAT_PRIO_IDLE : nice >= 4 ? PLAT_PRIO_LOWEST : nice >= 2 ? PLAT_PRIO_BELOW_NORMAL
         : nice >= 0 ? PLAT_PRIO_NORMAL : nice >= -2 ? PLAT_PRIO_ABOVE_NORMAL : nice >= -5 ? PLAT_PRIO_HIGHEST
         : PLAT_PRIO_TIME_CRITICAL;
}

void plat_yield(void) { sched_yield(); }

