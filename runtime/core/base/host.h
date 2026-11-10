// Interface of the host runtime: guest memory, modules, kernel, file system and boot.
//
// Guest addresses are host addresses. Every guest-visible region lies below 2^39 (Horizon's IPC buffer descriptors carry 39
// address bits; the SDK keeps that limit). Fixed parts of the guest address map:
//   modules           MOD_<X>_BASE from layout.h, from 0x70_0000_0000
//   kernel region     RT_KERNEL_BASE, 256 MiB: TLR, TLS blocks, host objects the guest reads (mem_kernel_alloc)
//   host functions    RT_HOSTFN_BASE, 1 MiB, never accessible: synthetic addresses of host functions
// The stack, heap and alias regions are placed at start-up (mem_layout).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "dispatch.h"

// ---- logging and results
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // "[host] <msg>\n" to the log, one write
// The log is stderr plus, in a game run whose stderr is not a file, logs\mkw.log next to mkw (main.c log_open).
// rt_logf and rt_log_raw write to both without the prefix and the newline: crash reports, fatal errors, guest output.
void rt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_vlogf(const char *fmt, va_list ap);
void rt_log_raw(const char *p, size_t n);
void rt_log_flush(void);                        // fflush + fsync of the log file handle before exiting on crash/device loss
const char *rt_log_path(void);                  // the log file, NULL when this run writes none
enum { RT_TRACE_SVC = 1, RT_TRACE_HOSTCALL = 2, RT_TRACE_FS = 8, RT_TRACE_GFX = 16, RT_TRACE_PERF = 32,
       RT_TRACE_SCHED = 64, RT_TRACE_AUDIO = 128, RT_TRACE_PRESENT = 256,
       RT_TRACE_DRAW = 512 };   // draw: the replay's per-op and per-draw clocks (a TSC read per stage of every draw)
extern unsigned rt_trace;                       // from MKW_TRACE=svc,host,fs,gfx,perf,sched,audio,present (or all)
// prof_win.c: MKW_PROFILE=<path> sampling profiler; MKW_TRACE=perf frame timing (waits of the render thread)
// PERF_TEX: texture uploads on the render thread (deswizzle, decode, staging), PERF_ASTC included
enum { PERF_PACE, PERF_FENCE, PERF_DRAIN, PERF_PIPE, PERF_TEX, PERF_ASTC, PERF_N };
void prof_start(void);
void prof_write(void);                          // idempotent; boot.c calls it before _exit
uint64_t perf_begin(void);                      // start time of a wait; pair with perf_end
void perf_end(int kind, uint64_t t0);           // counts render-thread time only
void perf_replay(uint64_t t0);                  // replay (NVN -> Vulkan recording) time on any thread, waits inside it included
void perf_replay_thread(void);                  // the calling thread replays for the render thread: perf_end counts its waits
void perf_main_thread(void);                    // the calling thread is the guest main thread (MKW_PROFILE_MAIN)
// Events per 60-frame window: graphics pipelines finished by the background workers, draws skipped while pending,
// first texture uploads and re-uploads after CPU writes, draws that waited for their pending pipeline and got it, draws
// into a target the previous frame did not render that waited for their pending pipeline instead of being skipped
// (vk_exec_draw.c prepare_draw). The rest feed only
// the flight recorder (flight.c): graphics pipelines created on any thread, texture images created, images created at an
// address where a live image of another descriptor already starts, write-backs done and skipped because the CPU
// rewrote the storage after the GPU render (vk_texture.c vk_texture_write_back).
enum { PERF_CNT_PIPE_ASYNC, PERF_CNT_PIPE_SKIP, PERF_CNT_TEX_UP, PERF_CNT_TEX_REUP, PERF_CNT_PIPE_WAIT, PERF_CNT_ONESHOT_WAIT, PERF_CNT_PIPE_BUILT, PERF_CNT_TEX_NEW, PERF_CNT_TEX_ALIAS,
       PERF_CNT_WB, PERF_CNT_WB_SKIP, PERF_CNT_N };
void perf_count(int kind);
void perf_present(uint64_t frame);
void perf_summary_flush(void);                  // prints the frame-time summary of the running period (exit); the 60 s lines come from perf_present
void gfx_exit_log(void);                        // back end's lines for the exit summary (weak no-op in prof_win.c)
void perf_gpu_wait(uint64_t t0);                // a wait for a GPU fence ended (any thread; the render thread's also counts as PERF_FENCE)
// GPU side of the 60-frame perf line, filled by the graphics back end when it set perf_gpu_stats. gpu_ns: GPU busy time
// since the previous call (timestamp queries, overlaps counted once), have_gpu 0 when the device has no timestamps.
typedef struct {
    int have_gpu, have_vram, vram_exact;
    uint64_t gpu_ns, gpu_parts, gpu_missed;     // busy time, submission parts timed, parts left untimed
    uint64_t vram_used, vram_budget;
} PerfGpu;
extern void (*perf_gpu_stats)(PerfGpu *out);
// The performance overlay (perfmon.c): mode 0 off, 1 fps, 2 full. While it is on, perf_present hands every frame to
// perfmon_frame; full also takes the event counts (perf_count) and the GPU timestamps as MKW_TRACE=perf does.
void perf_set_overlay(int mode);
int perf_gpu_timing(void);                      // GPU timestamps wanted: MKW_TRACE=perf or the full overlay
void perf_gpu_busy(uint64_t ns);                // GPU busy time of a timed submission part (vk_sync.c, any thread)
uint64_t perf_gpu_busy_total(void);             // its running total (the overlay's GPU time per frame)
#define RT_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
void rt_sha256(const void *p, size_t n, uint8_t out[32]);   // BCrypt; fatal if the provider fails

// ---- guest address map (fixed part)
#define RT_ADDRESS_LIMIT (1ull << 39)
#define RT_KERNEL_BASE 0x7010000000ull
#define RT_KERNEL_SIZE 0x10000000ull
#define RT_HOSTFN_BASE 0x7020000000ull
#define RT_HOSTFN_SIZE 0x100000ull

// ---- AAPCS64 argument access for host functions (section 2.2)
static inline uint64_t arg_x(const Cpu *c, int i) { return i < 8 ? c->x[i] : LD64(c->sp + 8 * (uint64_t)(i - 8)); }
static inline void *arg_p(const Cpu *c, int i) { return (void *)(uintptr_t)arg_x(c, i); }
static inline float arg_s(const Cpu *c, int i) { uint32_t b = (uint32_t)c->v[i]; float f; memcpy(&f, &b, 4); return f; }
static inline double arg_d(const Cpu *c, int i) { uint64_t b = (uint64_t)c->v[i]; double d; memcpy(&d, &b, 8); return d; }
static inline void ret_x(Cpu *c, uint64_t v) { c->x[0] = v; }
static inline void ret_s(Cpu *c, float f) { uint32_t b; memcpy(&b, &f, 4); c->v[0] = b; }
static inline void ret_d(Cpu *c, double d) { uint64_t b; memcpy(&b, &d, 8); c->v[0] = b; }

// ---- host functions (hostfn.c)
// Every host function is a GuestFn at a synthetic address RT_HOSTFN_BASE + 16*i. It runs on the calling guest thread,
// may clobber x0-x18, v0-v7, v16-v31, must preserve x19-x29, sp, v8-v15, tpidr, and returns to the guest BL site with
// a C return. On entry c->pc holds the function's own synthetic address.
typedef struct { const char *name; GuestFn fn; } RtHostEntry;
#define RT_CAT2(a, b) a##b
#define RT_CAT(a, b) RT_CAT2(a, b)
// File-scope declarations; fn must be declared before. The loader binds them by symbol name. The entries of every
// object file meet in one section per kind, which hostfn.c walks: on PE/COFF ".ovr$m", which the linker sorts between
// hostfn.c's ".ovr$a" and ".ovr$z" markers; on ELF "mkw_ovr", which the linker brackets with __start_mkw_ovr and
// __stop_mkw_ovr.
#ifdef _WIN32
#define RT_HOST_SECTION(kind) section("." #kind "$m")
#else
#define RT_HOST_SECTION(kind) section("mkw_" #kind)
#endif
#define HOST_OVERRIDE(sym, fn) /* replaces sym in every GOT/GLOB_DAT/ABS64 of every module */                  \
    __attribute__((used, RT_HOST_SECTION(ovr))) static const RtHostEntry RT_CAT(rt_ovr_, __COUNTER__) = {sym, fn}
#define HOST_OBSERVER(sym, fn) /* JUMP_SLOT only; fn calls rt_original(c) */                                    \
    __attribute__((used, RT_HOST_SECTION(obs))) static const RtHostEntry RT_CAT(rt_obs_, __COUNTER__) = {sym, fn}
#define HOST_FUNCTION(name, fn) /* no symbol: hostfn_seal registers it; hostfn_address(fn) after that */        \
    __attribute__((used, RT_HOST_SECTION(fns))) static const RtHostEntry RT_CAT(rt_fns_, __COUNTER__) = {name, fn}
uint64_t hostfn_address(GuestFn fn);                       // synthetic address of a HOST_FUNCTION; fatal if none.
                                                           // For the vtables of objects the host hands the guest
uint64_t hostfn_register(const char *name, GuestFn fn);    // synthetic address; before hostfn_seal only. The same
                                                           // (name, fn) pair returns the same address
GuestFn hostfn_override(const char *name);                 // NULL if none
GuestFn hostfn_observer(const char *name);                 // NULL if none
void hostfn_add_override(const char *name, GuestFn fn);    // an override outside the linker table (probes); before
                                                           // ldr_load
const char *hostfn_name(uint64_t addr);                    // name registered at a synthetic address, NULL if none
void hostfn_seal(void);                                    // build the dispatch table (rt_dispatch_init over every
                                                           // linked module); idempotent; registration ends here
uint64_t hostfn_calls(void);                               // host-function calls so far, all threads
void hostfn_report(void);                                  // call count per host function to stderr, busiest first
int hostfn_table_size(int observers);                      // linked HOST_OVERRIDE (0) / HOST_OBSERVER (1) entries
const char *hostfn_table_name(int observers, int i);       // symbol of linked entry i, NULL past the end
uint64_t rt_original(Cpu *c);                              // inside an observer: call the guest definition with the
                                                           // current x0..x7, return x0. Uses c->pc, so call it before
                                                           // anything that changes c->pc (rt_call restores it)
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...);         // host -> guest, section 2.3. n <= 8 arguments, each read
                                                           // as uint64_t: cast every argument. Seals the dispatch
                                                           // table on first use. Sets x30 = RT_RET_HOST, restores
                                                           // x30 and pc, returns x0. Does not touch sp
#define RT_RET_HOST 0x70200ff000ull                        // x30 sentinel inside the host-function window; a jump to
                                                           // it is a named fatal

// ---- guest memory (memory.c)
typedef struct { uint64_t base, size; } RtRange;
typedef struct { RtRange aslr, stack, heap, alias, kernel, hostfn; uint64_t pool; } RtGuestLayout;
typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } RtMemoryInfo;   // Horizon layout
void mem_init(void);                                       // reserve every region; on a collision re-run the process
                                                           // (up to 20 times, MKW_REEXEC), then fatal
const RtGuestLayout *mem_layout(void);
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr);                      // 0x01
uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm);             // 0x02
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at); // 0x03
uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n);                       // 0x04
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n);                     // 0x05
uint32_t mem_query(uint64_t a, RtMemoryInfo *out);                              // 0x06
uint32_t mem_map_physical(uint64_t a, uint64_t n);                              // 0x2C
uint32_t mem_unmap_physical(uint64_t a, uint64_t n);                            // 0x2D
int mem_info(uint32_t id, uint64_t *out);                  // GetInfo memory ids 2-7, 12-17, 21, 22, 28; 0 if not a memory id
void *mem_kernel_alloc(uint64_t size, uint64_t align);     // zeroed, guest-visible, in the kernel region; align <= 64 KiB
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
                                                           // GetWriteWatch(RESET): fills pages[] (4 KiB addresses)
                                                           // with the pages of [addr, addr+size) written since the
                                                           // last call, returns their count (at most max; a page
                                                           // past max stays reported next time). The range must lie
                                                           // inside the stack/heap block or the alias region
                                                           // (write watch); otherwise returns UINT64_MAX
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
                                                           // the same without the reset: the bits stay for the next
                                                           // mem_written_pages (verify checks only)
int mem_freeze(uint64_t a);                                // makes the 2 MiB region at a (2 MiB aligned, watched,
                                                           // committed PAGE_READWRITE) read-only until a write or
                                                           // mem_thaw thaws it; 1 frozen (or already), 0 refused
int mem_frozen(uint64_t a);                                // the 2 MiB region of a is frozen: no page in it changed
                                                           // since its freeze
void mem_thaw(uint64_t a, uint64_t n);                     // makes the frozen regions over [a, a+n) writable again;
                                                           // before a kernel write (file read) into guest memory
void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws);
                                                           // regions frozen now; freezes and thaws since the last call
int mem_trap(uint64_t a, uint64_t n, const void *src);     // [a, a+n) (watched) takes the n bytes at src: the whole
                                                           // pages on first access (read trap), the rest now; src
                                                           // stays valid until mem_untrap; 0: traps unavailable,
                                                           // nothing done
void mem_untrap(uint64_t a, uint64_t n);                   // fills the trapped pages of [a, a+n) and forgets the
                                                           // sources inside it (before src goes)
int mem_trapped(uint64_t a);                               // the page of a is trapped (its bytes are still the source's)
// Base traps (memory_btrap.c): pages of a range standing for the shadow's merge base until something writes them.
void mem_base_hook(void (*fn)(uint64_t page));             // called under frz_lock before a base-trapped page changes
int mem_base_trap(uint64_t lo, uint64_t hi, uint64_t *bits);   // whole pages of [lo, hi) (page aligned) read-only;
                                                           // bit per page trapped; 0: no trap map (Windows)
void mem_base_release(uint64_t lo, uint64_t hi);           // the pages still trapped back to read-write, no copy
int64_t mem_base_pages(void);                              // pages base-trapped now
// MKW_EARLY_READ=1 (memory_early.c, a diagnosis): the pages a submit's GPU work writes are watched from the submit
// (fence: its value) until first touched, counting the game's touches before the GPU finished it.
int mem_early_on(void);
int mem_early_waits(void);   // an early read of what the GPU writes waits (MKW_EARLY_READ 2 or 3)
// MKW_FENCE_EARLY (nvn_api_core.c nvnSyncWait): the game's fence waits end once the submit is recorded, before its GPU
// work; mem_early_rel is the newest fence so released. An early read then also waits for a writer up to it (the console
// game finds its bytes there after the wait), and the shadow copy waits for its GPU (vk_sync.c prologue).
extern int mem_fence_early;
extern uint64_t mem_early_rel;
void mem_early_set_done(uint64_t (*fence_done)(void), int (*fence_wait)(uint64_t value, uint64_t timeout_ns),
                        uint64_t (*fence_queued)(void));
uint64_t mem_early_inflight(uint64_t a, uint64_t n, uint64_t after);   // a writer of [a, a+n) the GPU runs now, 0 none
void mem_early_wb_check(uint64_t a, uint64_t n, uint64_t f);            // fence f's merged write-back of [a, a+n)
void mem_early_watch(uint64_t a, uint64_t n, uint64_t fence, int kind);   // kind: 0 counter, 1 copy, 2 texture to
                                                                          // buffer, 3 clear, 4 storage buffer, 5 uniform
void mem_copy_out(void *dst, uint64_t a, uint64_t n);                    // memcpy from guest memory, watched pages kept
void mem_early_log(uint64_t frame);                                       // every frame; a line every 600
void mem_early_gpu_wrote(uint64_t a, uint64_t n, uint64_t fence);         // the replay: submission fence writes [a, a+n)
void mem_early_invalidate(uint64_t a, uint64_t n);                        // the game reads [a, a+n) of a cached pool next
void mem_trap_stats(uint64_t *now, uint64_t *trapped_pages, uint64_t *access_fills, uint64_t *other_fills);
                                                           // pages trapped now; trapped and filled since the last call
void mem_kernel_free(void *p, uint64_t size);              // TLR/TLS reuse after thread exit

// ---- modules (loader.c)
typedef struct {
    const char *name; uint64_t base, size, text_end, data_off;   // text_end, data_off: offsets from base
    uint64_t init, init_array, init_array_size, ctor_flag;  // absolute guest addresses; init_array_size in bytes
} RtLoaded;
void ldr_load(const char *exefs_dir);                      // load, relocate (overrides applied), seal; once
int ldr_count(void);
const RtLoaded *ldr_module(int i);                         // order: main, subsdk0, sdk
void seqwatch_init(void);                                  // MKW_SEQ_WATCH=1: log the game's sequence nodes (seqwatch.c)
void impcensus_init(void);                                 // MKW_IMPORT_CENSUS=<file>: SDK functions main calls
void impcensus_hit(uint64_t addr, uint64_t lr);            // from fnprof_hit, while impcensus_on
void impcensus_write(void);                                // the census file, at exit (prof_write)
extern volatile int impcensus_on;
void hostfn_time_report(uint64_t frame);                   // MKW_HOST_TIME=1: host functions by time, every 600 presents
void hostfn_time_window(uint64_t frame);                   // the same since the last report, now (seqwatch.c MKW_SEQ_EXIT)
void seqwatch_frame(uint64_t frame);                       // each present: phases whose update stopped have ended
int seqwatch_window(void);                                 // MKW_SEQ_EXIT's window is open: no periodic host time report
int64_t seqwatch_since_us(void);                         // since MKW_SEQ_UNTIL's event, -1 before (the A/B world clock)
int seqwatch_hold(void);                                   // a MKW_SEQ_HOLD node is active: no input script presses
void seqwatch_movie_end(void);                              // a video reached its end of stream (ov_codec.c)
void seqwatch_log_active(void);                            // ctl seq: the active sequence nodes
uint64_t ldr_symbol(const char *name);                     // exported definition, 0 if none
uint64_t ldr_plt_stub(const char *module, const char *name); // PLT stub whose GOT slot holds name, 0 if none
const char *ldr_describe(uint64_t a, char *buf, int n);    // "main+0x1234 (symbol+0x10)", "host <name>", or the address
uint8_t *ldr_read_verified(const char *dir, const char *file, size_t *len);   // whole file, SHA-256 checked against
                                                           // the game code's tables (fatal on mismatch); free() it

// Written by tools/build/build.py into build/obj/rt/defaults.c (the game code's own tables: gamelib.h).
extern const char rt_default_exefs[], rt_default_romfs[], rt_default_profile[];
extern const char rt_port_version[];                       // the port's version

// ---- kernel (kernel.c)
typedef struct RtThread RtThread;
Cpu *kern_cpu(void);                                       // current guest thread's Cpu, NULL on host threads
uint32_t kern_main_thread(Cpu **cpu);                      // main thread object and Cpu for the calling host thread
                                                           // (main.npdm: 1 MiB stack at the top of the stack region,
                                                           // priority 44, core 0); on the main thread: its handle
void kern_main_stack(uint64_t *base, uint64_t *size);      // the main thread's stack, size 0 before kern_main_thread;
                                                           // svc.c reports it to QueryMemory, memory.c does not know it
void kern_run_on_guest_main(void (*fn)(Cpu *c));           // start the guest main host thread, run fn there, join
int kern_core_away(void);                                  // give the virtual core up around a blocking host wait
void kern_core_back(int *away);
void kern_checkpoint(void);                                // preemption point, at every SVC and indirect call
// A preempt request is pending for the thread (Cpu.thread): its RtThread starts with the flag (kernel.c).
static inline int kern_preempt_pending(const void *thread) { return __atomic_load_n((const int *)thread, __ATOMIC_RELAXED); }
// Set once before guest threads start (MKW_CORE_DIAG, MKW_ASYNC_PREEMPT): host calls (hostfn.c) and SVCs (svc.c) count their depth in
// the thread's second int, so the kernel knows whether a core owner runs game code at the top level.
extern int kern_host_track;
static inline int *kern_host_depth(void *thread) { return (int *)thread + 1; }
void kern_host_left(void *thread);                         // back at depth 0 with a preempt request pending
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns);   // perf_present, every present (also logs the
                                                           // asynchronous preemption counters)
void kern_sched_report(uint64_t frame);                    // MKW_TRACE=sched: per-thread time split, on present
uint32_t kern_handle_new(int kind, void *obj);             // kind: KERN_TMEM (svc.c)
void *kern_handle_get(uint32_t h, int kind);
void kern_handle_close(uint32_t h);                        // CloseHandle from the host side, any thread; fatal on a
                                                           // bad handle
enum { KERN_TMEM = 5 };
typedef struct { uint64_t addr, size; uint32_t perm; } KernTransferMemory;   // svcCreateTransferMemory (0x15)
uint32_t kern_event_new(void);                             // writable handle
uint32_t kern_event_readable(uint32_t writable);
void kern_event_signal(uint32_t writable);
void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear);   // guest thread only
void kern_signal_system_event(uint64_t ev);                // any thread
int kern_svc(Cpu *c, uint32_t imm);                        // thread/sync SVCs; 1 if handled
uint64_t kern_now_ns(void);
extern uint64_t rt_guest_start_ns;   // kern_now_ns when the guest started (boot.c), 0 before
typedef struct { uint64_t svcs, threads, host_calls; } RtStats;
RtStats rt_stats(void);                                    // host_calls: hostfn_calls()

// ---- file system (romfs.c / fs_host.c)
void fs_init(void);                                        // idempotent; paths from rt_config()

// ---- account (ov_system.c)
void account_uid(uint64_t out[2]);                         // the local user's nn::account::Uid {lo, hi}, persistent in
                                                           // <profile>/account.uid; fs_host.c names save data after it

// ---- code patches (patches.c) and the game clock (game.c)
void rt_patches_init(void);                                // after ldr_load: switches from the environment, the patch
                                                           // page committed; logs the sets
void rt_patch_hosts(void);                                 // before the dispatch table: the host entry points the
                                                           // patch page holds (hostfn_seal)
void rt_patch_switch(int set, int on);                     // at run time (live settings): each site takes the new code
                                                           // the next time it runs
int64_t frame_step_world_us(void);                         // game time since the phase MKW_SEQ_UNTIL waits for, -1 before
void ctl_init(void);                                       // MKW_CTL=<fifo>: commands to the running game (ctl.c)
void ctl_run(char *line);                                  // one ctl command now (MKW_SEQ_CTL, seqwatch.c)
void rt_shot_request(const char *path);                    // the next presented frame to path, full size (boot.c)
uint64_t frame_step_ticks(void);                           // game time since the first present in 60 Hz ticks (input scripts)
// In-launch A/B measurement: MKW_AB=<seconds> splits the world clock (frame_step_world_us) into blocks of that much
// game time in the order A B B A (repeated; B A A B with MKW_AB_INVERT=1); rt_ab() is 1 in the B blocks, 0 in A
// blocks, before the world clock and without MKW_AB. rt_knob(name, def) reads the switch MKW_<name> once per call
// (def when unset): 0 off, 2 for "ab" (on in the B blocks only), 1 for anything else;
// callers keep it and test it with RT_KNOB_ON. The perf line counts the B frames of its window (tools/ab_summary.py
// compares the windows that are wholly A or wholly B).
int rt_ab(void);
int rt_knob(const char *name, const char *def);
#define RT_KNOB_ON(mode) ((mode) == 2 ? rt_ab() : (mode))
void frame_step_present(uint64_t display_ns, int interval); // nvnQueuePresentTexture: the frame's display time and
                                                           // present interval (60 Hz ticks)
void frame_rate_host(uint64_t patch_page);                 // game.c MKW_FPS: register its host function (patches.c)
void frame_rate_init(uint64_t patch_page);                 // and point the frame object's vtable slot at it
// ---- boot (boot.c, main.c)
typedef struct { const char *exefs, *romfs, *profile; int boot; double seconds; const char *shot, *gpu; } RtConfig;
                                                           // romfs: the dump's loose romfs/ directory;
                                                           // frames: --frames N (0 = no limit); shot: --shot
                                                           // PATH; gpu: MKW_GPU
const RtConfig *rt_config(void);
void rt_boot_event(const char *what, Cpu *c, const char *detail);   // checkpoint log; "nvn" ends --boot
void rt_boot(void);                                        // boot.c
// Modes of mkw. main.c has run mem_init and parsed the paths; each mode calls ldr_load itself if it needs the
// modules, registers its host functions, then seals (hostfn_seal, or implicitly at the first rt_call). Return the
// process exit code.
int probe_cfg(void);                                       // cfg_probe.c
int probe_loader(void), probe_kernel(void), probe_audio(void), probe_services(void), probe_fs(void);
int probe_gfx_abi(void);                                   // gfx_abi_probe.c
int probe_vkdevice(void);                                 // vk_device.c
int probe_nvn(void), probe_nvn_tex(void);                  // nvn_probe.c, nvn_tex_probe.c
int probe_window(void);                                    // gfx_window_probe.c
int probe_vkmem(void), probe_vkexec(void), probe_astc(void);                 // tex_probe.c, vk_exec_probe.c, astc_probe.c
int probe_texpre(void);                                    // texpre_probe.c: GPU BC7 against bc7enc on game textures

int vk_texpre_encode(void);                                  // vk_texpre.c: --texpre (BC7 of the recorded ASTC textures)
