// MKW_REPLAY_PAR (default 1 with 6 or more physical cores, 0 below; "ab": in the B blocks): one submission's replay
// recorded on several threads. The game
// sends a race frame as one NVN command buffer of 2,500-3,100 draws, and one thread records it in 16-23 ms
//. Here a light walk follows the replay state through the submission's ops
// without recording (0.5 ms a race frame, walk1) and cuts them into chunks of MKW_REPLAY_PAR_DRAWS draws and
// dispatches, keeping a copy of the state (VkExec) at each cut. The chunks are recorded at once, each on a thread with
// its copy into a command buffer of its own (vk_exec_chunk_begin starts it as a split does: nothing bound), the
// replay thread taking chunks too. The replay thread sends the chunks to the queue in order as they are ready
// (vk_sync_split_with; the part before them goes with the first ones), between the chunks it records and then while it
// waits: one send at the end left the GPU idle for the whole recording, while the serial replay sends a part every
// 64 draws (pab1/pab1i: recording 20.6 -> 12.7 ms a frame, the frame +5.5 ms).
// - Texture uploads were already per submission (one check per submission against its start's snapshot of written
//   pages, make_fresh_snap): all of them before the first chunk is what the serial replay does for its first part.
// - What a draw reads of earlier ops' effects stays in order: the replay state (the walk's copies), the inline uniform
//   updates (the walk lists them; vk_exec_draw_upd_seed), the GPU-written ranges (note_written, per command buffer).
// - Conditional rendering is a CPU-side decision at record time (vk_exec_xfer.c): the walk decides each condition in op
//   order, with the submission's own earlier reports as pending, and the chunks use its decisions (a race frame has one).
// - Kept serial: samples-passed counters (queries and their completion order; none in a race, walk1), and every
//   diagnosis that times ops or passes or dumps a frame.
// - The uniform snapshot drops (vk_usnap_drop, ~0.5 per submission, walk1) are made where a chunk records them: a later
//   chunk recorded first would bind the copy instead of the GPU's bytes (img-rev: whole frames black). Drops carry
//   their replay position (vk_usnap.c), and the join records again, in order, each chunk that bound a copy at a
//   position after a drop (fix_order). Dropping the recently written regions ahead instead made the draws before the
//   write read guest memory: black as well (img-rev2).
// - The walk hands each chunk out as soon as it is past its end (MKW_REPLAY_PAR_PIPE): walking the whole submission
//   first left the GPU idle for the walk (2.1 ms a race submission on the i5-8400) and the frame +0.8 ms with chunks
//   although the GPU was busy 1.6-4.5 ms less (tl60a/tl60b, GTX 1050 Ti with MKW_FPS=60). A prescan rules out first
//   the ops the walk could not leave once chunks are out (samples-passed reports and resets), keeps cuts for the
//   queue switches and lists the texture slots, all checked before the walk (MKW_REPLAY_PAR_PRECHECK).
// - A serial head (MKW_REPLAY_PAR_HEAD, 256 draws; 0 off): the replay thread records the submission's start itself,
//   as the serial replay does (its first part sent after MKW_SPLIT_US), up to the first target change after that many
//   draws; the prescan and the slot check then run while the GPU works on it. Before them, the GPU waited for both
//   and for chunk 0 on another thread: it began 5.4-6.3 ms after the game's last submit against ~2.4 serial, and the
//   frame was +3.8 ms with chunks (pp63a/pp63b, GTX 1050 Ti, 1 player, MKW_FPS=60). With the head it begins 1.5-2.4
//   ms after, as serial, and the frame is +0.28 ms with the scene balanced, within noise (hd3a/hd3b).
// MKW_REPLAY_WALK=1 (diagnosis): the walk alone, timed, without chunks.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define MAX_CHUNKS VK_SPLIT_EXTRA
#define MAX_THREADS 16
#define DIMG_MAX 128

typedef struct {
    int hi, depth;   // handle and OP_CALL depth
    const op_hdr *h;
    const op_hdr *ret[VK_EXEC_CALL_DEPTH];
} Pos;

typedef struct {
    Pos pos;              // first op
    VkExec *ex;           // replay state at that op (the chunk's own copy, changed by its recording)
    VkExec *ex0;          // the same, kept for a second recording
    uint64_t op0;         // ops of the submission before it (its replay positions follow, vk_exec_op_pos)
    int upd0, cond0;      // inline uniform updates and conditions of the submission before it
    int dimg0;            // depth images bound before it (P.dimg; -1: more than DIMG_MAX)
    VkCommandBuffer cb;
    uint64_t drops;       // uniform snapshot drops it made
    VkCompletionList done; // its completion callbacks, added in chunk order once every chunk is recorded
    VkExecFrame counters;  // its MKW_TRACE=gfx frame counters, summed into vk_exec_frame_all at the join
    VkUsnapLog ulog;       // the uniform snapshot regions it bound from the copy and those it dropped
} Chunk;

typedef struct { VkCommandBuffer cb; uint64_t fence; } CbSlot;
typedef struct {
    VkCommandPool pool;   // one per thread: a pool is used by one thread at a time
    CbSlot *slot;
    int nslot, cslot;
} ThreadCbs;

// A submission left serial: a samples-passed report, reset or a conditional render (ops that must stay in order), under
// two chunks, counting on at the start.
enum { WHY_REPORT, WHY_RESET, WHY_COND, WHY_QUEUE, WHY_FEW, WHY_BUSY, WHY_N };

static struct {
    int mode, walk_diag, threads, chunk_draws, first_draws, reverse, warm, depth_off;
    int order[MAX_CHUNKS], norder;   // MKW_REPLAY_PAR_ORDER=a,b,c...: the first chunks taken in that order
    int cut, cut_op;                 // MKW_REPLAY_PAR_CUT, MKW_REPLAY_PAR_CUT_OP
    int oplog[2];                    // MKW_REPLAY_PAR_OPLOG
    uint64_t subs;
    // the plan of the submission being recorded
    Chunk ch[MAX_CHUNKS];
    VkExec *exbuf[MAX_CHUNKS], *exbuf0[MAX_CHUNKS];
    uint64_t plan_ops;
    int nch, plan_draws;
    const void **upd;
    int nupd, cupd;
    VkCondPlanned *cond;  // the conditions' decisions, in op order (vk_exec_cond_plan)
    int ncond, ccond;
    VkCondPlan cplan;
    VkExec end;
    VkExecQueues wq;          // the walk's queue states (OP_QUEUE)
    uint32_t *tids;           // the texture slots the submission may sample (precheck)
    int ntid, ctid;
    VkImage dimg[DIMG_MAX];   // the depth images the walk bound, in order of first binding (vk_exec_chunk_begin)
    int ndimg, dimg_over;
    VkImage wdimg;            // the walk's current depth image
    uint32_t wdlayers;        // its layers
    VkImage dc_img;           // a layered depth image whose clears have begun and not reached every layer: no cut
    uint32_t dc_left;
    // the job
    const uint64_t *handles;
    int n;
    uint64_t fence;
    uint64_t next;        // the job's generation in the high half, the next chunk to take in the low one
    int ready[MAX_CHUNKS]; // chunk recorded (its thread is done with it)
    int sent;             // chunks sent to the queue, in order
    VkExec *ex;           // the replay thread's state: its command buffer takes the first send's part before the chunks
    uint32_t gen;
    PlatMutex lock;
    PlatCond work, finished;
    ThreadCbs tcb[MAX_THREADS + 1];   // 0: the replay thread
    // since the last perf line
    uint64_t t_job, first_ns;   // when the chunks were handed out; summed: until the first send (the GPU starts there)
    uint64_t n_par, n_serial[WHY_N], n_chunks, n_draws, walk_ns, rec_ns, max_rec_ns, drops_early, w_subs, w_frames0, n_cond, n_again;
    uint64_t n_inpass;    // chunks cut inside a pass (at twice their draws without a target change)
    uint64_t n_qcut;      // chunks cut at a queue switch
    uint64_t n_pre, pre_ns;   // slots checked ahead, and the time (precheck)
    // where the first send waits (summed over submissions): chunk 0's draws, walk start to its hand-out, to its record
    uint64_t t_walk, ch0_draws, ch0_pub_ns, ch0_rdy_ns;
    int wdraws;           // the walk's draws so far
    int precheck;         // MKW_REPLAY_PAR_PRECHECK (default 1)
    int inpass;           // MKW_REPLAY_PAR_INPASS (default 1): those cuts made
    // MKW_REPLAY_PAR_PIPE: chunks handed out while the walk goes on (pipe_now: in this submission)
    int pipe, pipe_now;
    int avail;            // chunks handed out (the walk is past their end); the others wait for it
    int walking;          // the walk of the job is not done: chunks past avail may still come
    int pre_from;         // tids[] checked ahead up to here
    int qleft;            // the submission's OP_QUEUE ops the walk has not reached: cuts kept for them
    uint64_t n_pipe;      // submissions whose first chunk was handed out before the walk ended
    int head_draws;       // MKW_REPLAY_PAR_HEAD
    int head_now;         // the walk is still in the serial head (the replay thread records each op)
    int head_at;          // the walk's draws where the head ended (chunk 0 starts there); -1: no head ended
    uint64_t n_head, head_ndraws, head_ns;   // heads ended, their draws and time
} P;


static void dimg_note(VkImage img) {
    for (int i = 0; i < P.ndimg; i++)
        if (P.dimg[i] == img) return;
    if (P.ndimg == DIMG_MAX) P.dimg_over = 1;
    else P.dimg[P.ndimg++] = img;
}

static int is_draw(uint32_t op) {
    return op == OP_DRAW || op == OP_DRAW_IDX || op == OP_DRAW_IDX_IND || op == OP_DISPATCH || op == OP_DISPATCH_IND ||
           op == OP_DRAW_MESH;
}

// Moves p to the next op that is not BEGIN, END, JUMP or CALL; 0 past the submission's last op.
static int settle(Pos *p, const uint64_t *handles, int n) {
    for (;;) {
        const op_hdr *h = p->h;
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const p_jump *j = nvn_op_payload(h);
        switch (h->op) {
        case OP_BEGIN: p->h = nvn_op_next(h); break;
        case OP_JUMP: p->h = (const op_hdr *)(uintptr_t)j->target; break;
        case OP_CALL:
            if (p->depth >= VK_EXEC_CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", VK_EXEC_CALL_DEPTH);
            p->ret[p->depth++] = nvn_op_next(h);
            p->h = nvn_op_first(j->target);
            break;
        case OP_END:
            if (p->depth) { p->h = p->ret[--p->depth]; break; }
            if (++p->hi >= n) return 0;
            p->h = nvn_op_first(handles[p->hi]);
            break;
        default: return 1;
        }
    }
}

static void tid_note(const uint64_t *h, int n) {
    for (int i = 0; i < n; i++) {
        if (!h[i]) continue;
        if (P.ntid == P.ctid) {
            P.ctid = P.ctid ? P.ctid * 2 : 4096;
            if (!(P.tids = realloc(P.tids, sizeof *P.tids * (size_t)P.ctid))) rt_fatal("vk: out of memory (replay chunks)");
        }
        P.tids[P.ntid++] = (uint32_t)(h[i] & 0xfffff);   // the slot id (vk_exec_bind.c tex_info)
    }
}

// The once-per-submission check of every texture slot the submission may sample (vk_texture_by_id_locked: upload if
// the guest wrote it), made here under one texture lock before the chunks start, instead of at each slot's first
// binding on a chunk's thread, where all of them took the lock (lpc1: 142 ms a second of waits on tex_lock with
// chunks, 5.5 serial). The check answers from the submission's write-watch snapshot, so making it earlier in the
// submission changes nothing; a slot bound and never sampled is checked as well. The chunks then find each slot
// checked (vk_texture_checked) and skip the lock. MKW_REPLAY_PAR_PRECHECK=0: as before, to measure.
// With MKW_REPLAY_PAR_PIPE, the slots noted since the last call, before the chunks holding them are handed out.
static void precheck(void) {
    if (P.pre_from >= P.ntid) return;
    uint64_t t0 = kern_now_ns();
    uint32_t ep = vk_texture_epoch();
    vk_texture_lock();
    for (int i = P.pre_from; i < P.ntid; i++)
        if (!vk_texture_checked((int)P.tids[i], ep)) vk_texture_by_id_locked((int)P.tids[i], NULL, NULL), P.n_pre++;
    vk_texture_unlock();
    P.pre_from = P.ntid;
    P.pre_ns += kern_now_ns() - t0;
}

static void job_start(void);
static void send_ready(int wait);

// The job: its chunks become takeable as they are handed out (P.avail), by the threads woken here.
static void job_start(void) {
    P.t_job = kern_now_ns();
    for (int k = 0; k < MAX_CHUNKS; k++) __atomic_store_n(&P.ready[k], 0, __ATOMIC_RELEASE);
    plat_mutex_lock(&P.lock);
    uint32_t gen = ++P.gen;
    __atomic_store_n(&P.next, (uint64_t)gen << 32, __ATOMIC_RELEASE);
    P.walking = 1;
    plat_cond_broadcast(&P.work);
    plat_mutex_unlock(&P.lock);
}

// MKW_REPLAY_PAR_PIPE: chunks before `upto` are complete (the walk added the one after them): checked ahead, handed
// out to the threads, and the ready ones sent. The walk went first in full and the GPU got nothing until it ended:
// 2.1 ms a race submission on the i5-8400, the GPU idle 4.3 ms more a frame than serial (tl60a, 16/11/2026).
static void publish(int upto) {
    if (!P.pipe_now || upto <= P.avail) return;
    if (P.precheck) precheck();
    if (!P.avail) job_start(), P.ch0_pub_ns += P.t_job - P.t_walk;
    plat_mutex_lock(&P.lock);
    __atomic_store_n(&P.avail, upto, __ATOMIC_RELEASE);
    plat_cond_broadcast(&P.work);
    plat_mutex_unlock(&P.lock);
    send_ready(0);
}

// Ops the walk cannot leave once chunks are handed out, counted first (one pass over the submission, no state): a
// samples-passed report or reset (serial), the queue switches (cuts kept for them) and the arrays' sizes (no realloc
// while threads read them); it notes the texture slots for the check ahead, too. 0: the submission stays serial.
static int prescan(const Pos *from, const uint64_t *handles, int n, int *nupd, int *ncond, int *nqueue) {
    Pos p = *from;
    *nupd = *ncond = *nqueue = 0;
    while (settle(&p, handles, n)) {
        const op_hdr *h = p.h;
        p.h = nvn_op_next(h);
        switch (h->op) {
        case OP_UPDATE_UB: ++*nupd; break;
        case OP_RENDER_COND: ++*ncond; break;
        case OP_QUEUE: ++*nqueue; break;
        case OP_BIND_TEX:
            if (P.precheck) {
                const p_bind_handles *b = nvn_op_payload(h);
                tid_note(b->h, b->n < 0 ? 0 : b->n > 32 ? 32 : b->n);
            }
            break;
        case OP_REPORT_COUNTER:
            if (((const p_report_counter *)nvn_op_payload(h))->type == NVN_COUNTER_TYPE_SAMPLES_PASSED)
                return P.n_serial[WHY_REPORT]++, 0;
            break;
        case OP_RESET_COUNTER:
            if (((const p_reset_counter *)nvn_op_payload(h))->type == NVN_COUNTER_TYPE_SAMPLES_PASSED)
                return P.n_serial[WHY_RESET]++, 0;
            break;
        default: break;
        }
    }
    if (*nqueue > MAX_CHUNKS - 2) return P.n_serial[WHY_QUEUE]++, 0;
    return 1;
}

// The walk leaves the submission serial; never once chunks are handed out (prescan keeps those ops out of a pipelined
// walk: the chunks handed out would be recorded and the rest lost).
static int bail(int why) {
    if (P.avail) rt_fatal("vk: replay chunks: the walk left a submission serial (%d) after handing out chunks", why);
    return P.n_serial[why]++, 0;
}

static void chunk_add(const Pos *p, const VkExec *st) {
    Chunk *c = &P.ch[P.nch];
    if (!P.exbuf[P.nch] && !(P.exbuf[P.nch] = malloc(sizeof(VkExec)))) rt_fatal("vk: out of memory (replay chunks)");
    if (!P.exbuf0[P.nch] && !(P.exbuf0[P.nch] = malloc(sizeof(VkExec)))) rt_fatal("vk: out of memory (replay chunks)");
    c->ex0 = P.exbuf0[P.nch], c->op0 = P.plan_ops;
    c->pos = *p, c->ex = P.exbuf[P.nch], c->upd0 = P.nupd, c->cond0 = P.ncond, c->cb = VK_NULL_HANDLE, c->drops = 0;
    c->dimg0 = P.dimg_over || (P.depth_off && P.nch) ? -1 : P.ndimg;
    c->done.n = 0, c->ulog.nhit = c->ulog.nhpos = 0;
    memset(&c->counters, 0, sizeof c->counters);
    *c->ex = *st;
    if (P.nch == 1) P.ch0_draws += (uint64_t)(P.wdraws - (P.head_at > 0 ? P.head_at : 0));
    __atomic_store_n(&P.nch, P.nch + 1, __ATOMIC_RELEASE);
    publish(P.nch - 1);
}

// Draws of chunk k: the first ones grow from MKW_REPLAY_PAR_FIRST (64, doubling) up to C, so the GPU starts about as
// early as with the serial replay's 64-draw parts. With C draws from the first chunk the GPU waited for the whole
// first chunk, ~3-4 ms with every thread recording (pab2/pab2i: fence wait +3.5 ms, GPU busy +0.8).
// The rest of the submission from chunk 0's start, recorded serially on the replay thread: after a serial head, when
// the rest did not go in chunks (nothing was handed out, and the replay state is the one at chunk 0's start).
static void serial_rest(void) {
    Pos p = P.ch[0].pos;
    while (settle(&p, P.handles, P.n)) {
        const op_hdr *h = p.h;
        p.h = nvn_op_next(h);
        if (vk_exec_op(P.ex, h)) vk_exec_drew(P.ex);
    }
}

// Ops that end the serial head: what prescan rules out (the rest then stays serial) and conditions, which the walk
// decides from the submission's earlier reports.
static int head_stop(const op_hdr *h, const void *a) {
    if (h->op == OP_RENDER_COND) return 1;
    if (h->op == OP_REPORT_COUNTER) return ((const p_report_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED;
    if (h->op == OP_RESET_COUNTER) return ((const p_reset_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED;
    return 0;
}

// The serial head ends at op `at` (the replay state there is w's): chunk 0 starts there, and the prescan and slot
// check cover the rest. 0: the rest stays serial (vk_exec_par records it, serial_rest).
static int head_end(const Pos *at, const VkExec *w, int draws) {
    Chunk *c = &P.ch[0];
    c->pos = *at, c->op0 = P.plan_ops, c->upd0 = P.nupd, c->cond0 = P.ncond;
    c->dimg0 = P.dimg_over || P.depth_off ? -1 : P.ndimg;
    *c->ex = *w;
    P.head_now = 0, P.head_at = draws;
    P.n_head++, P.head_ndraws += (uint64_t)draws, P.head_ns += kern_now_ns() - P.t_walk;
    int nupd, ncond, nq;
    P.ntid = P.pre_from = 0;
    if (P.precheck)
        for (int s = 0; s < NVN_BIND_STAGES; s++) tid_note(w->tex[s], 128);
    if (!prescan(at, P.handles, P.n, &nupd, &ncond, &nq)) return 0;
    if (P.precheck) precheck();
    nupd += P.nupd, ncond += P.ncond;
    if (nupd > P.cupd && !(P.upd = realloc(P.upd, sizeof *P.upd * (size_t)(P.cupd = nupd + 1024))))
        rt_fatal("vk: out of memory (replay chunks)");
    if (ncond > P.ccond && !(P.cond = realloc(P.cond, sizeof *P.cond * (size_t)(P.ccond = ncond + 64))))
        rt_fatal("vk: out of memory (replay chunks)");
    P.qleft = nq;
    P.t_walk = kern_now_ns();
    return 1;
}

static int chunk_size(int k, int C) {
    if (P.first_draws <= 0) return C;
    int s = P.first_draws << (k < 16 ? k : 16);
    return s < C ? s : C;
}

// The light walk: follows the replay state from ex0 through the submission into P.end without recording, cutting
// chunks of about chunk_size draws and dispatches (at most MAX_CHUNKS). 0 when an op that must stay in order shows up.
// A chunk is cut where the render target changes once it has its draws, as the serial replay splits only between
// passes (maybe_split): a cut inside a pass ends it and loads its targets again, with a full barrier between (pab3:
// GPU busy +2.7 ms a frame with 10 chunks); at twice its draws without a change, it is cut anyway. Not where only the
// layer of the depth image changes: the layered depth clears (vk_exec.c dc_layered, GPU -2.8 ms a frame, rt-z5) clear
// every layer of the shadow map at once only within the chunk that bound it first.
// With a serial head (P.head_now), the replay thread records each op as the walk follows it, and the head ends where a
// chunk could be cut once it has P.head_draws draws. 2: the head took the whole submission (recorded serially).
static int plan(const VkExec *ex0, const uint64_t *handles, int n, int C) {
    VkExec *w = &P.end;
    *w = *ex0;
    P.nch = P.nupd = P.ncond = P.cplan.n = 0;
    P.plan_ops = 0;
    P.ndimg = P.dimg_over = 0, P.wdimg = P.dc_img = VK_NULL_HANDLE, P.wdlayers = P.dc_left = 0;
    P.wq.n = 0, P.wq.cur = 0;   // as the serial replay's, per submission (vk_exec_record)
    if (!P.pipe_now) P.ntid = P.pre_from = 0;   // (with it, prescan noted and checked them all)
    if (P.precheck && !P.pipe_now)
        for (int s = 0; s < NVN_BIND_STAGES; s++) tid_note(ex0->tex[s], 128);
    Pos p = {0, 0, nvn_op_first(handles[0]), {0}};
    chunk_add(&p, w);
    int draws = 0, in_chunk = 0;
    while (settle(&p, handles, n)) {
        const op_hdr *h = p.h;
        const void *a = nvn_op_payload(h);
        Pos at = p;   // the op itself: a cut before it starts the next chunk there
        p.h = nvn_op_next(h);
        if (P.head_now && head_stop(h, a)) {
            if (!head_end(&at, w, draws)) return 0;
            in_chunk = 0;
        }
        if (!P.head_now && P.cut_op && P.plan_ops == (uint64_t)P.cut_op && P.nch + P.qleft < MAX_CHUNKS)
            in_chunk = 0, chunk_add(&at, w);
        if (P.oplog[1] && draws >= P.oplog[0] && draws <= P.oplog[1])
            rt_log("vk: par oplog: submission %llu after draw %d: op %llu %s", (unsigned long long)P.subs, draws,
                   (unsigned long long)P.plan_ops, nvn_op_name(h->op));
        switch (h->op) {
        case OP_SET_RT:
            if (memcmp(&w->rt, a, sizeof w->rt)) {
                const p_set_rt *r = a;
                const VkTexImage *di = r->depth ? vk_texture_get(r->depth) : NULL;
                VkImage dimg = di ? di->image : VK_NULL_HANDLE;
                int fits = P.nch + P.qleft < MAX_CHUNKS && (P.depth_off || ((!dimg || dimg != P.wdimg) && !P.dc_img));
                if (P.head_now) {
                    if (fits && draws >= P.head_draws) {
                        if (!head_end(&at, w, draws)) return 0;
                        in_chunk = 0;
                    }
                } else if (!P.cut && !P.cut_op && in_chunk >= chunk_size(P.nch - 1, C) && fits)
                    in_chunk = 0, chunk_add(&at, w);
                P.wdimg = dimg, P.wdlayers = di ? di->layers : 0;
                if (dimg) dimg_note(dimg);
                w->rt = *(const p_set_rt *)a, w->rt_resolved = 0;
                // The targets' images, made now as the pass would make them (vk_texture_get): a chunk recorded before
                // the one rendering a target samples it, and a target the game registers anew each frame had no image
                // yet: the title's shadow map bound as a 32x32 stand-in, Mario lit wrong (dp01/dp10, MKW_DUMP).
                for (int i = 0; i < w->rt.n && i < 8; i++)
                    if (w->rt.color[i]) vk_texture_get(w->rt.color[i]);
            }
            break;
        case OP_CLEAR_DS:
            // The game clears its shadow map layer by layer, a pass between (e1: 2 of 3 layers still cleared one at a
            // time with cuts only kept off where the depth image stays): no cut until each layer had its clear.
            if (P.wdimg && P.wdlayers >= 2 && !P.depth_off) {
                if (P.dc_img != P.wdimg) P.dc_img = P.wdimg, P.dc_left = P.wdlayers;
                if (!--P.dc_left) P.dc_img = VK_NULL_HANDLE;
            }
            if (!vk_exec_state_op(w, h->op, a) && !vk_exec_bind_state(w, h->op, a)) vk_exec_xfer_state(w, h->op, a);
            break;
        case OP_REPORT_COUNTER:
            if (((const p_report_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED) return bail(WHY_REPORT);
            vk_exec_cond_plan(&P.cplan, h->op, a);
            break;
        case OP_QUEUE:
            // The game's queues take turns in split screen (nvn_qorder.c: each view's culling on the compute queue);
            // their states swap there (vk_exec.c queue_switch). The walk swaps its own and the next chunk starts after
            // the op, which the chunk ending at it skips: that chunk's end is the switch's pass end and full barrier.
            // Serial before (16/11/2026, #449: no 2-player submission went in chunks).
            if (P.qleft) P.qleft--;
            if (vk_exec_queue_swap(&P.wq, w, ((const p_queue *)a)->id) && !P.head_now) {
                Chunk *c = &P.ch[P.nch - 1];
                if (c->op0 == P.plan_ops) {   // nothing in the chunk yet (the submission's first op): it starts after
                    c->pos = p, c->op0 = P.plan_ops + 1, *c->ex = *w;
                    break;
                }
                if (P.nch >= MAX_CHUNKS) return bail(WHY_QUEUE);
                P.plan_ops++, in_chunk = 0, chunk_add(&p, w), P.plan_ops--, P.n_qcut++;
            }
            break;
        case OP_BIND_TEX:
            if (P.precheck && !P.pipe_now) {
                const p_bind_handles *b = a;
                tid_note(b->h, b->n < 0 ? 0 : b->n > 32 ? 32 : b->n);
            }
            vk_exec_bind_state(w, h->op, a);
            break;
        case OP_RESET_COUNTER:
            if (((const p_reset_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED) return bail(WHY_RESET);
            break;
        case OP_RENDER_COND: {
            if (P.ncond == P.ccond) {
                P.ccond = P.ccond ? P.ccond * 2 : 64;
                if (!(P.cond = realloc(P.cond, sizeof *P.cond * (size_t)P.ccond))) rt_fatal("vk: out of memory (replay chunks)");
            }
            int on = vk_exec_cond_plan(&P.cplan, h->op, a);
            P.cond[P.ncond++] = (VkCondPlanned){a, on};
            w->render_cond = *(const p_render_cond *)a, w->has_render_cond = 1, w->render_disabled = !on;
            P.n_cond++;
            break;
        }
        case OP_UPDATE_UB:
            if (P.nupd == P.cupd) {
                P.cupd = P.cupd ? P.cupd * 2 : 1024;
                if (!(P.upd = realloc(P.upd, sizeof *P.upd * (size_t)P.cupd))) rt_fatal("vk: out of memory (replay chunks)");
            }
            P.upd[P.nupd++] = a;
            break;
        default:
            if (is_draw(h->op)) {
                draws++, P.wdraws = draws;
                if (P.pipe_now && !(draws & 31) && P.sent < P.avail) send_ready(0);   // the chunks done meanwhile
                if (P.head_now ? 0 : P.cut_op ? 0 : P.cut ? draws == P.cut
                             : ++in_chunk >= 2 * chunk_size(P.nch - 1, C) && P.inpass && P.nch + P.qleft < MAX_CHUNKS && !P.dc_img)
                    P.plan_ops++, in_chunk = 0, chunk_add(&p, w), P.plan_ops--, P.n_inpass += !P.cut;
            } else if (!vk_exec_state_op(w, h->op, a) && !vk_exec_bind_state(w, h->op, a)) {
                vk_exec_xfer_state(w, h->op, a);
            }
        }
        if (P.head_now && vk_exec_op(P.ex, h)) vk_exec_drew(P.ex);
        P.plan_ops++;
    }
    P.plan_draws = draws;
    return P.head_now ? 2 : 1;
}

// A begun command buffer of thread t's pool for the submission of value fence.
static VkCommandBuffer chunk_cb(int t, uint64_t fence) {
    VkCtx *c = vk_ctx();
    ThreadCbs *tc = &P.tcb[t];
    if (!tc->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &tc->pool) != VK_SUCCESS) rt_fatal("vk: replay chunk command pool");
    }
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    CbSlot *s = NULL;
    for (int i = 0; i < tc->nslot && !s; i++)
        if (tc->slot[i].fence <= gpu) s = &tc->slot[i];
    if (!s) {
        if (tc->nslot == tc->cslot) {
            tc->cslot = tc->cslot ? tc->cslot * 2 : 16;
            if (!(tc->slot = realloc(tc->slot, sizeof *tc->slot * (size_t)tc->cslot))) rt_fatal("vk: out of memory (replay chunks)");
        }
        s = &tc->slot[tc->nslot++];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = tc->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: replay chunk command buffer");
    }
    s->fence = fence;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    if (vk_guard_on()) vk_guard_cb_begin(s->cb);
    return s->cb;
}

static void chunk_record(int k, int t, int again) {
    Chunk *c = &P.ch[k];
    VkExec *ex = c->ex;
    if (again) *ex = *c->ex0, memset(&c->counters, 0, sizeof c->counters);   // the first recording is not submitted
    else *c->ex0 = *ex;
    ex->cb = c->cb = chunk_cb(t, P.fence);
    ex->fence = P.fence;
    vk_mem_recording(P.fence);
    vk_completion_defer(&c->done);
    vk_exec_frame_cur = &c->counters;
    c->ulog.nhit = c->ulog.nhpos = 0;
    vk_usnap_log(&c->ulog);
    vk_exec_xfer_again(again);
    vk_exec_chunk_begin(ex, P.dimg, c->dimg0);
    vk_exec_op_pos_set(c->op0);
    vk_exec_draw_upd_seed(ex, P.upd, c->upd0);
    // (a chunk handed out before the walk ended has the next one: the walk goes on adding to nch, ncond and dimg)
    int last = k == __atomic_load_n(&P.nch, __ATOMIC_ACQUIRE) - 1;
    vk_exec_cond_use(P.cond + c->cond0, (last ? P.ncond : P.ch[k + 1].cond0) - c->cond0);
    uint64_t d0 = vk_usnap_thread_drops();
    uint64_t ops = last ? UINT64_MAX : P.ch[k + 1].op0 - c->op0;   // up to the next chunk's first op
    Pos p = c->pos;
    while (ops-- && settle(&p, P.handles, P.n)) {
        const op_hdr *h = p.h;
        p.h = nvn_op_next(h);
        if (h->op != OP_QUEUE) vk_exec_op(ex, h);   // the walk made the switch (plan): it ends this chunk
    }
    vk_exec_chunk_end(ex);
    // Ended here, on the thread whose pool it comes from: the replay thread that submits it must not touch it, as this
    // thread may then be recording its next chunk from the same pool (vk_sync_split_with).
    vkEndCommandBuffer(c->cb);
    vk_exec_cond_use(NULL, 0);
    vk_completion_defer(NULL);
    vk_exec_frame_cur = &vk_exec_frame_all;
    vk_usnap_log(NULL);
    vk_exec_xfer_again(0);
    vk_mem_recording(t == 0 ? P.fence : 0);   // the replay thread goes on recording the submission
    c->drops = vk_usnap_thread_drops() - d0;
}

static void send_ready(int wait);

// Chunk k once handed out (MKW_REPLAY_PAR_PIPE: the walk may still be before its end); 0 when the job has no chunk k.
static int chunk_wait(int k, uint32_t gen) {
    // (avail first: a later job bumps gen before it hands out anything, so a late thread sees the new gen here)
    if (k < __atomic_load_n(&P.avail, __ATOMIC_ACQUIRE) && __atomic_load_n(&P.gen, __ATOMIC_ACQUIRE) == gen) return 1;
    plat_mutex_lock(&P.lock);
    while (k >= P.avail && P.walking && P.gen == gen) plat_cond_wait(&P.work, &P.lock);
    int ok = k < P.avail && P.gen == gen;
    plat_mutex_unlock(&P.lock);
    return ok;
}

// A thread late from the previous job takes nothing of the next one: its generation no longer matches.
static void take_chunks(int t, uint32_t gen) {
    for (;;) {
        uint64_t v = __atomic_fetch_add(&P.next, 1, __ATOMIC_ACQ_REL);
        int k = (int)(uint32_t)v;
        if ((uint32_t)(v >> 32) != gen || !chunk_wait(k, gen)) return;
        k = P.reverse ? P.nch - 1 - k : k < P.norder && P.order[k] < P.nch ? P.order[k] : k;
        chunk_record(k, t, 0);
        plat_mutex_lock(&P.lock);   // the replay thread may wait for this chunk to send it (send_ready)
        if (!k) P.ch0_rdy_ns += kern_now_ns() - P.t_job;
        __atomic_store_n(&P.ready[k], 1, __ATOMIC_RELEASE);
        plat_cond_broadcast(&P.finished);
        plat_mutex_unlock(&P.lock);
        if (!t) send_ready(0);
    }
}

static void worker_main(void *arg) {
    int t = (int)(intptr_t)arg;
    uint32_t seen = 0;
    for (;;) {
        plat_mutex_lock(&P.lock);
        while (P.gen == seen) plat_cond_wait(&P.work, &P.lock);
        seen = P.gen;
        plat_mutex_unlock(&P.lock);
        take_chunks(t, seen);
    }
}

static void setup(void) {
    // On by default with 6+ physical cores: with the serial head it is neutral where the GPU sets the frame, with 1
    // player (hd3a/hd3b: +0.28 ms) and 2 (hd4a/hd4b: +0.50 ms, the recording 34 -> 30 ms), i5-8400 with 6 cores and
    // a GTX 1050 Ti; it gains where the recording sets it. Off below: on a Steam Deck's 4 cores (8 threads, hence not
    // plat_cpu_count) the chunk threads took the game's CPU, 2 players 25.4 -> 22 fps (#449, before the head). 0 when
    // the core count is unknown.
    uint64_t all, first;
    P.mode = rt_knob("REPLAY_PAR", plat_cpu_cores(&all, &first) >= 6 ? "1" : "0");
    const char *e = cfg_get("MKW_REPLAY_WALK");
    P.walk_diag = e && atoi(e) > 0;
    e = cfg_get("MKW_REPLAY_PAR_DRAWS");
    P.chunk_draws = e && atoi(e) > 0 ? atoi(e) : 384;
    // MKW_REPLAY_PAR_WARM (120): submissions replayed serially first, so the lazily set up switches and caches settle.
    // mkw --replay sets 0: a capture holds 12-16 submissions, and with 120 none was ever recorded in chunks.
    e = cfg_get("MKW_REPLAY_PAR_WARM");
    P.warm = e ? atoi(e) : 120;
    e = cfg_get("MKW_REPLAY_PAR_FIRST");   // the first chunk's draws, doubling up to MKW_REPLAY_PAR_DRAWS; 0: all equal
    P.first_draws = e ? atoi(e) : 64;
    // MKW_REPLAY_PAR_INPASS=0 (a measurement): no cut inside a pass, only where the target changes. Those cuts store
    // and load the pass's targets again behind a full barrier (pab3); with them the GPU went +1.6-1.9 ms a frame on the
    // 660M (cut2) and +3 on the GTX 1050 Ti (wpar1).
    P.inpass = rt_knob("REPLAY_PAR_INPASS", "1") != 0;
    P.precheck = rt_knob("REPLAY_PAR_PRECHECK", "1") != 0;
    // MKW_REPLAY_PAR_PIPE (default 1; "ab"; 0: the walk in full first, as before 16/11/2026): each chunk handed out
    // to the threads, and sent when ready, as soon as the walk is past its end (publish).
    P.pipe = rt_knob("REPLAY_PAR_PIPE", "1");
    e = cfg_get("MKW_REPLAY_PAR_HEAD");   // draws of the serial head (with PIPE and threads); 0: none
    P.head_draws = e ? atoi(e) : 256;
    e = cfg_get("MKW_REPLAY_PAR_THREADS");   // threads besides the replay thread
    uint32_t cpus = plat_cpu_count();
    P.threads = e ? atoi(e) : cpus >= 8 ? 3 : cpus >= 4 ? 2 : 1;
    if (P.threads < 0) P.threads = 0;   // 0: the chunks in order on the replay thread (a check of the chunking alone)
    // MKW_REPLAY_PAR_DEPTH=0 (a measurement): as before 07/11/2026, chunks after the first clear depth layer by layer
    // and are cut between the layers of one depth image.
    e = cfg_get("MKW_REPLAY_PAR_DEPTH");
    P.depth_off = e && !atoi(e);
    // MKW_REPLAY_PAR_ORDER=reverse (diagnosis): chunks taken last first; with no threads, out of order without any
    // concurrency: whether a fault comes from the order chunks are recorded in or from threads racing.
    e = cfg_get("MKW_REPLAY_PAR_ORDER");
    P.reverse = e && !strcmp(e, "reverse");
    // MKW_REPLAY_PAR_CUT=X (diagnosis): two chunks, cut after the submission's X-th draw. With ORDER=1,0 and no threads
    // a fault from the recording order shows for X between the op that changes some state and the one reading it:
    // two searches over X find both.
    // MKW_REPLAY_PAR_OPLOG=from,to (diagnosis): the walk logs every op after draw `from` up to draw `to`.
    e = cfg_get("MKW_REPLAY_PAR_OPLOG");
    if (e) P.oplog[0] = atoi(e), P.oplog[1] = strchr(e, ',') ? atoi(strchr(e, ',') + 1) : P.oplog[0];
    e = cfg_get("MKW_REPLAY_PAR_CUT");
    P.cut = e ? atoi(e) : 0;
    // MKW_REPLAY_PAR_CUT_OP=X (diagnosis): as CUT, cut before the submission's X-th op (OPLOG numbers them): the op that
    // changes some state and the one that reads it, not just their draws.
    e = cfg_get("MKW_REPLAY_PAR_CUT_OP");
    P.cut_op = e ? atoi(e) : 0;
    e = cfg_get("MKW_REPLAY_PAR_ORDER");
    // MKW_REPLAY_PAR_ORDER=1,0,2 (diagnosis): the first chunks of each submission taken in that order, the rest in
    // order; with no threads it tells which pair of chunks a fault from the recording order lies in.
    for (const char *s = e; s && !P.reverse && *s && P.norder < MAX_CHUNKS; s += strcspn(s, ","), s += *s == ',')
        P.order[P.norder++] = atoi(s);
    if (P.threads > MAX_THREADS) P.threads = MAX_THREADS;
    if (P.walk_diag) rt_log("vk: light walk of each submission before its replay (MKW_REPLAY_WALK, a measurement)");
    if (!P.mode) return;
    plat_cond_init(&P.work), plat_cond_init(&P.finished);
    for (int t = 1; t <= P.threads; t++)
        if (!plat_thread_spawn(worker_main, (void *)(intptr_t)t, "vk chunks", 0)) rt_fatal("vk: cannot start a replay chunk thread");
    rt_log("vk: replay in chunks of %d draws on %d threads besides the replay thread%s (MKW_REPLAY_PAR)", P.chunk_draws,
           P.threads, P.mode == 2 ? ", in the B blocks" : "");
}

// A chunk that bound a uniform snapshot copy at a position after a GPU write into that region (made by an earlier
// chunk that recorded later) has to bind guest memory there: the chunk records again, now that every drop position
// is known. Its first command buffer is ended and left unsubmitted; its callbacks stay queued (they free what it used).
static int wrong_hits(const Chunk *c) {
    for (int i = 0; i < c->ulog.nhit; i++)
        if (vk_usnap_drop_at(c->ulog.hit[i]) <= (uint64_t)(uint32_t)c->ulog.hpos[i]) return 1;
    return 0;
}

// Chunks from the next unsent one while they are ready, then sent together; wait: until every chunk is sent. A chunk's
// wrong_hits is known once the chunks up to it are recorded: a drop is kept at its lowest position (vk_usnap_drop),
// and later chunks only drop at positions after its own.
// Only the chunks handed out (P.avail): during a pipelined walk, the later ones are not planned yet.
static void send_ready(int wait) {
    while (P.sent < P.avail) {
        int from = P.sent;
        for (int k = from; k < P.avail; k++) {
            if (!__atomic_load_n(&P.ready[k], __ATOMIC_ACQUIRE)) {
                if (!wait || k > from) break;
                plat_mutex_lock(&P.lock);
                while (!__atomic_load_n(&P.ready[k], __ATOMIC_ACQUIRE)) plat_cond_wait(&P.finished, &P.lock);
                plat_mutex_unlock(&P.lock);
            }
            Chunk *c = &P.ch[k];
            // the first recording was ended by its thread (chunk_record); the second is on this thread's pool
            if (vk_usnap_regions() && wrong_hits(c)) chunk_record(k, 0, 1), P.n_again++;
            vk_exec_frame_add(&c->counters);
            for (int i = 0; i < c->done.n; i++) vk_completion_add(c->done.e[i].fence, c->done.e[i].fn, c->done.e[i].arg);
            if (c->drops && k < P.nch - 1) P.drops_early += c->drops;
            P.sent = k + 1;
        }
        if (P.sent == from) return;
        VkCommandBuffer cbs[MAX_CHUNKS];
        for (int k = from; k < P.sent; k++) cbs[k - from] = P.ch[k].cb;
        if (!from) vk_exec_pass_end(P.ex), P.first_ns += kern_now_ns() - P.t_job;   // the part before ends outside a pass
        P.ex->cb = vk_sync_split_with(cbs, P.sent - from);
        if (!wait) return;
    }
}

int vk_exec_par(VkExec *ex, const uint64_t *handles, int n, int diag) {
    if (!P.subs++) setup();
    if (P.walk_diag && !P.mode) {   // the walk alone
        P.pipe_now = 0;
        uint64_t t0 = kern_now_ns();
        plan(ex, handles, n, 256);
        P.n_draws += (uint64_t)P.plan_draws;
        uint64_t d = kern_now_ns() - t0;
        P.walk_ns += d, P.w_subs++;
        if (d > P.max_rec_ns) P.max_rec_ns = d;
        return 0;
    }
    // A frame dump (MKW_DUMP) records its copies into the command buffer being recorded: with chunk threads that is
    // several at once, so it stays serial; with none (MKW_REPLAY_PAR_THREADS=0) the chunks may be dumped, in their order
    // (diag, which a dump also sets, still keeps them serial: the next step for the title's order fault).
    if (!P.mode || !RT_KNOB_ON(P.mode) || diag || (vk_dump_armed && P.threads) || P.subs <= (uint64_t)P.warm) return 0;
    if (vk_exec_oq_busy()) return P.n_serial[WHY_BUSY]++, 0;
    uint64_t t0 = kern_now_ns();
    P.handles = handles, P.n = n, P.fence = ex->fence, P.ex = ex, P.sent = 0, P.avail = 0, P.qleft = 0;
    // MKW_REPLAY_PAR_PIPE: chunks handed out from the walk (publish) once prescan rules out an op it could not leave;
    // the diagnoses that pick the chunks' order need them all first
    P.pipe_now = RT_KNOB_ON(P.pipe) && !P.reverse && !P.norder;
    P.head_now = P.pipe_now && P.threads && P.head_draws > 0, P.head_at = -1;
    if (P.pipe_now && !P.head_now) {
        int nupd, ncond, nq;
        P.ntid = P.pre_from = 0;
        if (P.precheck)
            for (int s = 0; s < NVN_BIND_STAGES; s++) tid_note(ex->tex[s], 128);
        Pos p0 = {0, 0, nvn_op_first(handles[0]), {0}};
        if (!prescan(&p0, handles, n, &nupd, &ncond, &nq)) return 0;
        // Every slot checked here, before any chunk is out: checked chunk by chunk during the walk, the check held
        // tex_lock while the threads recorded, 298 ms a second of their sleeps, and chunk 0's 60 draws took 3.3 ms
        // instead of ~1.2 (lk62, 16/11/2026).
        if (P.precheck) precheck();
        if (nupd > P.cupd && !(P.upd = realloc(P.upd, sizeof *P.upd * (size_t)(P.cupd = nupd + 1024))))
            rt_fatal("vk: out of memory (replay chunks)");
        if (ncond > P.ccond && !(P.cond = realloc(P.cond, sizeof *P.cond * (size_t)(P.ccond = ncond + 64))))
            rt_fatal("vk: out of memory (replay chunks)");
        P.qleft = nq;
    }
    P.t_walk = kern_now_ns(), P.wdraws = 0;
    int r = plan(ex, handles, n, P.chunk_draws);
    if (r == 2) return P.n_serial[WHY_FEW]++, 2;   // the head took it all
    if (!r || P.nch < 2) {
        if (P.nch < 2) P.n_serial[WHY_FEW]++;   // (bail already stops when chunks are out)
        if (P.head_at < 0) return 0;
        serial_rest();   // the head was recorded: the rest from where it ended
        return 2;
    }
    if (!P.avail) P.ch0_pub_ns += kern_now_ns() - P.t_walk;   // (handed out below)
    if (P.precheck) precheck();
    if (!P.avail) job_start();
    else P.n_pipe++;
    plat_mutex_lock(&P.lock);   // the walk is done: every chunk handed out
    __atomic_store_n(&P.avail, P.nch, __ATOMIC_RELEASE);
    P.walking = 0;
    plat_cond_broadcast(&P.work);
    plat_mutex_unlock(&P.lock);
    uint64_t t1 = kern_now_ns();
    // With the walk handing chunks out, the threads have the first ones recorded by now: this thread only sends, in
    // order as they are ready. Taking a chunk too, it sent the ready ones only after recording its own (a 384-768 draw
    // one, 3-4 ms): first send 4.6 ms after the chunks began against 1.3, the GPU starting 7-12 ms after the game's last
    // submit against 2-4 serial (pp60a, 16/11/2026).
    if (!P.pipe_now || !P.threads) take_chunks(0, P.gen);
    send_ready(1);
    uint64_t t2 = kern_now_ns();
    // The rest of the submission continues from the state after its last op, in the command buffer of the last send.
    VkExec keep = *ex;
    *ex = P.end;
    ex->cb = keep.cb, ex->fence = keep.fence, ex->recorded = keep.recorded;
    ex->views = keep.views, ex->nviews = keep.nviews, ex->cap_views = keep.cap_views;
    ex->in_pass = 0, ex->rt_resolved = 0, ex->dirty = VK_EXEC_DIRTY_ALL, ex->barrier_at = ex->last_dispatch = ex->clear_run_at = -1;
    ex->nfbl = 0, ex->fbl_mask = 0, ex->fb_dirty = 0, ex->fb_fs = NULL;
    vk_exec_draw_new_cb();
    P.n_par++, P.n_chunks += (uint64_t)P.nch, P.n_draws += (uint64_t)P.plan_draws, P.walk_ns += t1 - t0, P.rec_ns += t2 - t1;
    if (t2 - t1 > P.max_rec_ns) P.max_rec_ns = t2 - t1;
    return 1;
}

void vk_exec_par_log(void) {
    uint64_t frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), nf = frames - P.w_frames0;
    if (!nf && P.n_par) nf = 1;   // mkw --replay's closing line: its presents may not count as frames
    P.w_frames0 = frames;
    if (P.walk_diag && !P.mode && P.w_subs && nf)
        rt_log("perf: light walk %.3f ms per frame (%llu frames, %llu submissions, max %.3f ms each; %.0f draws per frame)",
               (double)P.walk_ns / 1e6 / (double)nf, (unsigned long long)nf, (unsigned long long)P.w_subs,
               (double)P.max_rec_ns / 1e6, (double)P.n_draws / (double)nf);
    else if (P.mode && nf)
        rt_log("perf: replay in chunks: %llu submissions (%.1f chunks, %.0f draws each), serial %llu (in-order op) %llu "
               "(under 2 chunks) %llu (counting on); in-order ops: report %llu reset %llu cond %llu queues %llu; per frame walk %.3f ms, chunks %.3f ms (max %.3f a submission); "
               "first send %.2f ms after the chunks start (a submission); "
               "uniform snapshot drops before the last chunk %llu, chunks recorded again for them %llu; conditions decided by the walk %llu; "
               "cuts inside a pass %.1f, at a queue switch %.1f a submission (MKW_REPLAY_PAR_INPASS); "
               "slots checked ahead %.0f per frame in %.3f ms (MKW_REPLAY_PAR_PRECHECK); "
               "chunks handed out during the walk in %llu submissions (MKW_REPLAY_PAR_PIPE); first chunk %.0f draws, "
               "handed out %.2f ms after the walk began and recorded %.2f ms after that; "
               "serial heads %llu of %.0f draws in %.2f ms (MKW_REPLAY_PAR_HEAD)", (unsigned long long)P.n_par,
               P.n_par ? (double)P.n_chunks / (double)P.n_par : 0, P.n_par ? (double)P.n_draws / (double)P.n_par : 0,
               (unsigned long long)(P.n_serial[WHY_REPORT] + P.n_serial[WHY_RESET] + P.n_serial[WHY_COND] +
                                    P.n_serial[WHY_QUEUE]),
               (unsigned long long)P.n_serial[WHY_FEW], (unsigned long long)P.n_serial[WHY_BUSY],
               (unsigned long long)P.n_serial[WHY_REPORT], (unsigned long long)P.n_serial[WHY_RESET],
               (unsigned long long)P.n_serial[WHY_COND], (unsigned long long)P.n_serial[WHY_QUEUE], (double)P.walk_ns / 1e6 / (double)nf,
               (double)P.rec_ns / 1e6 / (double)nf, (double)P.max_rec_ns / 1e6,
               P.n_par ? (double)P.first_ns / 1e6 / (double)P.n_par : 0, (unsigned long long)P.drops_early, (unsigned long long)P.n_again,
               (unsigned long long)P.n_cond, P.n_par ? (double)P.n_inpass / (double)P.n_par : 0,
               P.n_par ? (double)P.n_qcut / (double)P.n_par : 0, (double)P.n_pre / (double)nf, (double)P.pre_ns / 1e6 / (double)nf,
               (unsigned long long)P.n_pipe, P.n_par ? (double)P.ch0_draws / (double)P.n_par : 0,
               P.n_par ? (double)P.ch0_pub_ns / 1e6 / (double)P.n_par : 0, P.n_par ? (double)P.ch0_rdy_ns / 1e6 / (double)P.n_par : 0,
               (unsigned long long)P.n_head, P.n_head ? (double)P.head_ndraws / (double)P.n_head : 0,
               P.n_head ? (double)P.head_ns / 1e6 / (double)P.n_head : 0);
    P.walk_ns = P.rec_ns = P.max_rec_ns = P.first_ns = P.n_par = P.n_chunks = P.n_draws = P.drops_early = P.w_subs = P.n_cond = P.n_again = 0;
    P.n_inpass = P.n_qcut = P.n_pre = P.pre_ns = P.n_pipe = P.ch0_draws = P.ch0_pub_ns = P.ch0_rdy_ns = 0;
    P.n_head = P.head_ndraws = P.head_ns = 0;
    memset(P.n_serial, 0, sizeof P.n_serial);
}
