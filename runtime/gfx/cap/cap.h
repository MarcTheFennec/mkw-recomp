// Frame capture and offline replay: MKW_CAPTURE records what the Vulkan back end
// receives over some presented frames (guest memory, the back end's object state, every submission and present) into
// one zstd stream; `mkw --replay DIR` feeds it to the back end again without the game, in seconds instead of a run to
// the race. Guest memory sits at the same host addresses in every process (memory.c mem_init), so the pointers inside
// it (pools, programs, descriptor pools) stay valid; host-heap pointers in op payloads (pinned tex_t copies) are
// written out as fix-ups.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#define CAP_MAGIC 0x3150414357424b4dull   // "MKWBCAP1"
#define CAP_VERSION 1u

// Records: {type, size of what follows} + payload. Memory records come before the state that points into them.
enum {
    CAP_META = 1,     // CapMeta
    CAP_MEM,          // {addr, len} + len bytes: guest pages as they are now
    CAP_ZERO,         // {addr, len}: guest pages that read zero now
    CAP_POOL_INIT,    // {pool_t address}
    CAP_POOL_FINI,    // {pool_t address}
    CAP_MAPV,         // {pool_t address, n} + n NVNmappingRequest (0x28 bytes)
    CAP_TEXREG,       // CapTexReg
    CAP_SAMPREG,      // {id} + samp_par
    CAP_TEXREL,       // tex_t
    CAP_PROGREL,      // {prog_t address}
    CAP_SUBMIT,       // {n} + per handle: {bytes, nfix} + recording + nfix offsets (uint32) + nfix tex_t
    CAP_PRESENT,      // CapPresent
    CAP_WTEX,         // CapWtex + bytes
    CAP_GWRITTEN,     // {addr, size}
    CAP_FINISH,       // the game drained the queue (nvnQueueFinish)
    CAP_FRAME,        // {present number}: the frame's records end here (after its CAP_PRESENT)
    CAP_END,
    CAP_GPUMEM,       // {addr, len} + len bytes: a physical pool chunk as the GPU left it (vk_mem_phys_dump)
    CAP_SPARSE,       // {klen, len} + key + len bytes: a sparse tile slot (vk_sparse_dump)
    CAP_WIMG,         // CapWimg + bytes: layers of a decoded ASTC image as the image holds them (vk_texture_decoded)
    // new types go after CAP_END: the numbers above are in every capture already written
    CAP_LAST = CAP_WIMG,
};
typedef struct { uint32_t type, pad; uint64_t size; } CapHdr;
typedef struct {
    uint64_t magic;
    uint32_t version, depth_minus_w;   // nvn_depth_minus_w at the capture
    uint64_t first_frame, frames;      // presents captured: first_frame .. first_frame + frames - 1
    uint64_t heap_base, unused;        // guest layout of the capturing process (the replay checks its own)
    uint64_t alias_base, stack_base;
} CapMeta;
typedef struct { int32_t id, has_view; tex_t t; view_t v; } CapTexReg;
typedef struct { tex_t t; int32_t has_crop, crop[4]; } CapPresent;
typedef struct { tex_t t; int32_t has_view, pad; view_t v; NVNcopyRegion r; uint64_t row, img, bytes; } CapWtex;
typedef struct { tex_t t; uint32_t format, level, layer, nlayers; uint64_t bytes; } CapWimg;   // format: the VkFormat

// cap_io.c: the zstd stream. Writer: one thread at a time (callers hold their own lock).
typedef struct CapOut CapOut;
CapOut *cap_out_open(const char *path);
void cap_out(CapOut *o, uint32_t type, const void *a, size_t na, const void *b, size_t nb);   // header + a + b
void cap_out_raw(CapOut *o, const void *p, size_t n);            // more payload of a record whose size covered it
void cap_out_close(CapOut *o);
uint64_t cap_out_bytes(const CapOut *o);                          // uncompressed bytes so far
typedef struct CapIn CapIn;
CapIn *cap_in_open(const char *path);
int cap_in_next(CapIn *in, CapHdr *h);                            // 0 at the end of the stream
void cap_in_read(CapIn *in, void *p, size_t n);                   // payload bytes of the current record (fatal short)
void cap_in_close(CapIn *in);

// cap_mem.c: guest memory snapshots. Walks the module images, the stack and heap block and the alias region (committed,
// readable, populated pages) and writes CAP_MEM; cap_mem_delta writes the pages that changed since (page hashes),
// CAP_ZERO for the ones that went back to zero. 0 when the platform cannot list populated pages.
int cap_mem_snapshot(CapOut *o);
uint64_t cap_mem_delta(CapOut *o);                                 // pages written
void cap_mem_restore(const void *data, uint64_t addr, uint64_t len);   // commits and copies (replay)
void cap_mem_zero(uint64_t addr, uint64_t len);
void cap_mem_commit(uint64_t addr, uint64_t len);                  // pages not committed yet read zero

// cap_record.c: the back end MKW_CAPTURE wraps (gfx_backend); b otherwise.
const GfxBackend *cap_wrap(const GfxBackend *b);
int cap_request(const char *dir, int frames);   // ctl capture: from the next present; 0 when the back end is not wrapped
int cap_busy(void);                             // a capture is being written or waits to start
// cap_replay.c: mkw --replay DIR
int cap_replay_main(int argc, char **argv);
