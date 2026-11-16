// Internal header of the NVN front end, shared by nvn_api_core.c and the other
// slices (textures, samplers, programs, windows, state objects, command buffers). Adapted from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c and nvn_impl.h @ b75be8cf
// (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile).
#pragma once
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "nvn_obj.h"

// NVN_API(nvnX) { body }: the strong nvnp_nvnX(Cpu *c); counts the call in gfx_counters, MKW_TRACE=gfx logs the first.
#define NVN_API(n)                                                                                                   \
    static void nvn_impl_##n(Cpu *c);                                                                                \
    void nvnp_##n(Cpu *c) {                                                                                          \
        static uint8_t seen;                                                                                         \
        nvn_called(&seen, #n);                                                                                       \
        nvn_impl_##n(c);                                                                                             \
    }                                                                                                                \
    static void nvn_impl_##n(Cpu *c)
void nvn_called(uint8_t *seen, const char *name);          // the counting prologue of NVN_API
#define NVN_LOG(...) do { if (rt_trace & RT_TRACE_GFX) rt_log("nvn: " __VA_ARGS__); } while (0)   // MKW_TRACE=gfx
// A wait for the GPU, the display or another thread gives the virtual core up until the end of the enclosing scope.
#define NVN_BLOCKING() __attribute__((cleanup(kern_core_back))) int nvn_away_ = kern_core_away()
static inline int nvn_arg_bool(const Cpu *c, int i) { return (int)(arg_x(c, i) & 0xff); }   // NVNboolean: low 8 bits only

#define NVN_POOL_MAGIC MAGIC('P', 'O', 'O', 'L')           // pool_t.magic of an initialized pool
#define NVN_VPOOL_PAGE 0x10000ull                          // NVN virtual mapping granularity (64 KiB)
static inline int nvn_gpu_only(uint64_t a) { return a - RT_GPU_VA_BASE < RT_GPU_VA_SIZE; }   // address in the GPU window
// A CPU-invisible physical or virtual pool lives in the GPU window: mem is its GPU address (never mapped on the host),
// game_mem the game's storage (never touched), span the reserved window bytes, host NULL. Every other pool: mem is the
// game's storage, the GPU address equals the guest address.

typedef struct { NVNdevice *dev; pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; pool_t *pool; int64_t off; uint64_t size; } buf_t;           FITS(buf_t, NVNbuffer);
typedef struct {                                           // NVNmappingRequest as main passes it (0x28 bytes)
    const NVNmemoryPool *physical_pool;                    // NULL unmaps
    int64_t physical_offset, virtual_offset;
    uint64_t size;
    int32_t storage_class;
    uint32_t padding;
} nvn_mapping_t;
_Static_assert(sizeof(nvn_mapping_t) == 0x28, "NVNmappingRequest stride");

const pool_t *nvn_pool_find(uint64_t addr, uint64_t len);  // GPU-visible pool holding [addr, addr+len], NULL if none
uint64_t nvn_new_serial(void);                             // next tex_t / prog_t serial, from 1
uint64_t nvn_fence_last(void);                             // fence value of the newest submit (0 before the first)
void nvn_present_fence_note(void);                         // a present: notes the newest fence (SyncWait census)
void nvn_sync_wait_window(void);                           // a present: the waits of the last 60, by where the value was
int nvn_depth_minus_w(void);                               // 1 after DeviceSetDepthMode(NEAR_IS_MINUS_W), else 0
void nvn_depth_minus_w_set(int on);                        // the mode of a capture being replayed (runtime/gfx/cap)
void nvn_guest_written(uint64_t addr, uint64_t size);      // the host or the game wrote pool storage: back end re-reads
// MKW_TRACE=perf: MemoryPoolFlushMappedRange [0] and BufferFlushMappedRange [1] calls [0] and bytes [1] by the pool's
// CPU class (0 CPU_CACHED, 1 CPU_UNCACHED, 2 other) since the previous call.
void nvn_flush_stats(uint64_t out[2][3][2]);
void nvn_submit_prepare(const uint64_t *handles, int n);   // weak no-op; the command-buffer slice reads deferred binds
// Asynchronous replay: as nvn_submit_prepare, writing the handles to replay to out (a clone where a still queued
// submission uses the same recording with other deferred binds)
void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n);
int gfx_replay_async(void);                                // 1: the back end replays submissions on its own thread
uint64_t gfx_replayed_submits(void);                       // newest submit number (nvn_submit_count) replayed
#define NVN_HANDLE_TEX(h) ((int)((h) & 0xfffff))           // combined texture handle = texture id | sampler id << 20
#define NVN_HANDLE_SAMP(h) ((int)(((h) >> 20) & 0xfff))
extern GfxStats gfx_counters;                              // behind gfx_stats(); add with __atomic_fetch_add. frames:
                                                           // the back end's present; nvn_calls/nvn_distinct: NVN_API
// nvn_dstk.c (ctl drawstacks): draws counted by guest call stack and depth target for the next N frames
void nvn_dstk_request(int frames);
void nvn_dstk_rt(const void *depth_mem);
void nvn_dstk_draw(const Cpu *c);

// ---- nvn_api_obj.c: textures, samplers, programs, windows
#define NVN_TEX_MAGIC MAGIC('T', 'E', 'X', 0)              // tex_t.magic of an initialized texture
#define NVN_SAMP_MAGIC MAGIC('S', 'M', 'P', 0)             // samp_t.magic
#define NVN_PROG_MAGIC MAGIC('P', 'R', 'O', 'G')           // prog_t.magic
#define NVN_WIN_MAGIC MAGIC('W', 'I', 'N', 'D')            // win_t.magic
// The live texture whose storage starts at mem, copied into *out; 0 if none. The game re-initializes transient textures
// at the same pool memory without registering them again, so a registered serial that was finalized since resolves here.
int nvn_tex_live_at(const void *mem, tex_t *out);
// Byte offset of a view's first level and layer inside the texture storage (nvnTextureGetViewOffset); v may be NULL.
uint64_t nvn_tex_view_offset(const tex_t *t, const view_t *v);

// ---- nvn_api_cmd.c, nvn_ext.c: command buffers and their replay contract
// A command handle (what QueueSubmitCommands receives) points at a host copy of one recording: a flat sequence of
// 8-byte-aligned records {op_hdr, payload}, OP_BEGIN first and OP_END last. Called recordings are inlined, so a copy
// holds no OP_CALL or OP_JUMP. Texture pointers in payloads (p_set_rt, p_copy_b2t, p_copy_t2t, p_clear_tex) are
// immutable host copies that live for the run, one per tex_t.serial. Deferred binds (p_bind_handles.src != 0) hold the
// handles read at submit (nvn_submit_prepare). Payloads of the ops nvn_obj.h does not define:
typedef struct { volatile uint32_t *val; uint32_t mode, value; } p_signal_event;   // OP_SIGNAL_EVENT; mode: NVNeventSignalMode
typedef p_signal_event p_wait_event;                                                // OP_WAIT_EVENT; mode: NVNeventWaitMode
// OP_QUEUE: the game's queue whose ops follow (only in the streams nvn_qorder.c builds, where two queues' work takes
// turns at their event waits: each queue goes on with its own replay state, vk_exec.c queue_switch)
typedef struct { uint64_t id; } p_queue;
void nvn_event_cpu_signal(volatile uint32_t *val);   // nvnEventSignal: nvn_qorder.c reads the event's memory again
// nvn_qorder.c, under the front end's submit lock after nvn_submit_prepare_async: 0, the submission goes to the back
// end as it is; 1, it waits for another queue's work and is held (*fe: nvn_qorder_attach it to the back end's slot).
int nvn_qorder_submit(const void *queue, const uint64_t *handles, int n, void **fe);
void nvn_qorder_attach(void *fe, void *slot);
uint64_t nvn_stream_handle(const uint8_t *ops, size_t n);   // nvn_api_cmd.c: a replay-only recording of these ops
typedef struct { uint64_t type, addr; } p_report_counter;  // OP_REPORT_COUNTER: NVNcounterType, NVNcounterData address
typedef struct { uint64_t type; } p_reset_counter;         // OP_RESET_COUNTER
typedef struct { uint64_t src, dst, size; } p_copy_b2b;    // OP_COPY_B2B: GPU addresses
typedef struct { uint64_t dst, size; uint8_t data[]; } p_update_ub;   // OP_UPDATE_UB: size bytes written at dst
typedef struct { int32_t bits, pad; } p_barrier;           // OP_BARRIER: NVNbarrierBits
typedef struct { uint64_t target; } p_jump;                // OP_JUMP / OP_CALL: next record / called recording
// OP_DRAW_IDX_IND: DrawElementsIndirect; ind = GPU address of {count, instances, first index, base vertex, base
// instance} (VkDrawIndexedIndirectCommand's layout), read by the GPU when the draw runs. MultiDrawElementsIndirectCount:
// count = GPU address of the draw count (0: one draw), up to max draws stride bytes apart
// snap: the arguments as guest memory held them at the submit (nvn_submit_prepare, one draw without count in CPU-visible
// memory; snapped 1): the game rewrites them for its next frame while the replay is behind (lf6, 06/11/2026).
typedef struct { int32_t prim, itype; uint64_t ibuf, ind, count; int32_t max, stride; uint32_t snap[5]; int32_t snapped; } p_draw_idx_ind;
typedef struct { uint64_t ind, caller; } p_dispatch_ind;   // OP_DISPATCH_IND: ind = GPU address of the 3 group counts
// OP_CLEAR_BUF: size bytes at addr filled with the 32-bit value in GPU order (ClearBuffer; ReportValue is a 4-byte one)
typedef struct { uint64_t addr, size; uint32_t value, pad; } p_clear_buf;
_Static_assert(sizeof(p_signal_event) == 16 && sizeof(p_report_counter) == 16 && sizeof(p_copy_b2b) == 24 &&
               sizeof(p_update_ub) == 16, "op payload layouts of hle_nvn.c");
static inline const op_hdr *nvn_op_first(uint64_t handle) { return (const op_hdr *)(uintptr_t)handle; }   // OP_BEGIN
static inline const op_hdr *nvn_op_next(const op_hdr *h) { return (const op_hdr *)(const void *)((const uint8_t *)h + h->size); }
static inline const void *nvn_op_payload(const op_hdr *h) { return h + 1; }
int nvn_handle_live(uint64_t handle);                      // 1 while the handle's copy exists (its control memory was
                                                           // not added again)
uint64_t nvn_handle_bytes(uint64_t handle);                // bytes of the copy, OP_BEGIN to OP_END; 0 if not live
extern uint32_t nvn_sub_sp_reports, nvn_sub_reports, nvn_sub_events, nvn_sub_waits;
#define NVN_EARLY_MAX 4096
typedef struct { uint64_t addr, size; int kind; } NvnEarlyDst;   // MKW_EARLY_READ: a submit's GPU write destinations
extern NvnEarlyDst nvn_sub_dst[NVN_EARLY_MAX];
extern int nvn_sub_ndst;
extern volatile uint32_t *nvn_sub_event_val[64];
int nvn_report_at_submit(void);   // MKW_REPORT_AT_SUBMIT (nvn_api_cmd.c)   // the submission being prepared (nvn_api_cmd.c)
uint64_t nvn_submit_count(void);                           // QueueSubmitCommands calls so far
const char *nvn_op_name(uint32_t op);                      // "DRAW_IDX", ...; "?" past OP_COUNT_
// MemoryPoolInvalidateMappedRange: the CPU is about to read pool storage the GPU wrote. Weak no-op in nvn_ext.c; a back
// end that renders into host-side copies defines it and writes them back.
void nvn_pool_invalidate(const pool_t *p, uint64_t off, uint64_t size);
// MemoryPoolFlushMappedRange: the CPU wrote [mem + off, mem + off + size). Weak no-op in nvn_ext.c; a back end that keeps
// device-side copies of pool memory (vk_memory.c's VRAM mirror) defines it and copies the range again.
void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size);

// NVN2 (nvn2.c)
int nvn_prog_set_shaders(prog_t *o, int n, const NVNshaderData *sd);   // nvnProgramSetShaders' body (nvn_api_obj.c)
void nvn2_note_generated(const Cpu *c, int n, const NVNshaderData *sd);   // MKW_SPIRV_DUMP: stack of generated programs
void nvn2_write_watch_arm(void);   // MKW_WRITE_WATCH: re-arms the watched pages (every present)
void nvn2_note_label(const prog_t *o, const char *label);                 // MKW_SPIRV_DUMP: and their debug label
uint64_t nvn2_sdk_proc(const char *name);    // the driver's own function for an NVN2 proc, or 0
int nvn2_use_sdk_proc(const char *name);     // 1 when that function is a pure accessor the game may call directly
int nvn2_program_stage(const uint8_t *control, const uint8_t *code);     // NVNshaderStage at control+0x56 (6 task, 7 mesh)
uint32_t nvn2_program_length(const uint8_t *control, const uint8_t *code);   // code bytes, cut as extract.py does
void nvn2_program_key(const uint8_t *control, const uint8_t *code, uint8_t key[32]);   // prepared SPIR-V name
