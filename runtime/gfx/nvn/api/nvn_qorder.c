// The game's queues ordered by their events (MKW_QUEUE_WAITS, default 1; 0 off, "ab": in the B blocks). MKW's two worker
// queues pass stages back and forth with nvnCommandBufferWaitEvent; replayed in submit order on our one Vulkan queue,
// a wait the signals submitted before it do not meet ran its work before what it waits for (wc1: 50-65 % of the waits,
// each met by the other queue's next submit; Hookshot's finding in another MKW recomp, an idea only: GPL-3.0).
//
// Here, at submit (game thread, under the front end's submit lock), each event's value is followed in the order the
// console's GPU would reach the signals. A submission that reaches a wait not met is held: its place in the replay
// queue is kept (gfx submit_held, so its fence value, captures and what follows it stay where they were), and the ops
// up to the wait go into the episode's stream. Every later submission while one is held is held too; each queue runs
// as far as its waits allow, in turns, and its ops follow in the stream behind an OP_QUEUE mark (the back end keeps a
// replay state per queue, vk_exec.c queue_switch). When no queue waits any more, the first held submission gets the
// stream as one recording and the others none: every fence value is signalled after all the work, the first one later
// than on the console, never earlier. A CPU signal (nvnEventSignal) can let a queue on too.
//
// Let go as it stands, as before (gfx_held_force): when the replay reaches a held submission still empty and waits
// MKW_QUEUE_HOLD_MS (vk_backend.c), the oldest waiting queue goes past its wait, the others as far as theirs allow.
// MKW_QUEUE_WAITS=0 only counts the waits the submit order breaks (the census of wc1), one line every 1,500 submits.
// On by default since #430: in a split-screen race both views' culling blocks run on the compute queue and write the
// same buffers, each followed on the console by its own view; in submit order the second block ran before the first
// view, so every view drew with the last view's culling (a parked player 1 lost the crowd the far player 2 no longer
// saw: i430-def against i430-qw, GTX 1050 Ti, 13.8 against 14.8 fps). Off before for +3.1 ms a frame on a Radeon 660M
// (qw2j); the black cacti it seemed to add were a translator atomic, fixed in 9129803.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define EV_MAX 512
#define RUNS_MAX 4
#define SLOTS_MAX 64

typedef struct Piece { struct Piece *next; size_t n; uint8_t ops[]; } Piece;   // a held submission's ops, flat
typedef struct { const void *queue; Piece *head, *tail; size_t pos; } Run;      // pos: head's wait not met
typedef struct { void *slot; int attached, decided; uint64_t h; } FeSlot;       // h 0: empty
typedef struct { const uint8_t *p, *end; } Span;

static PlatMutex qo = PLAT_MUTEX_INIT;
static struct { volatile uint32_t *val; uint32_t v; } ev[EV_MAX];
static Run runs[RUNS_MAX];
static int nruns;
static FeSlot *slots[SLOTS_MAX];
static int nslots;
static struct { uint8_t *b; size_t n, cap; const void *queue; } out;   // the episode's stream
static uint64_t st_waits, st_unmet, st_episodes, st_held, st_forced, st_cpu, st_bytes;

static int mode(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("QUEUE_WAITS", "1");
        rt_log("nvn: the queues' work ordered by their event waits%s (MKW_QUEUE_WAITS)",
               !m ? ": off, a census only" : m == 2 ? " in the B blocks" : "");
    }
    return m;
}

static int ev_slot(volatile uint32_t *val) {   // the event's slot (taken if new); -1: the table is full
    uint32_t k = (uint32_t)(((uintptr_t)val >> 2) * 0x9E3779B1u) % EV_MAX;
    for (int n = 0; n < EV_MAX; n++, k = (k + 1) % EV_MAX) {
        if (ev[k].val == val) return (int)k;
        if (!ev[k].val) return ev[k].val = val, ev[k].v = *val, (int)k;
    }
    return -1;
}

static int wait_met(const p_wait_event *w) {   // (an event the table has no room for: as before, not waited for)
    int k = ev_slot(w->val);
    if (k < 0) return 1;
    return w->mode == NVN_EVENT_WAIT_MODE_EQUAL ? ev[k].v == w->value : (int32_t)(ev[k].v - w->value) >= 0;
}

static void signal(const p_signal_event *e) {
    int k = ev_slot(e->val);
    if (k >= 0) ev[k].v = e->mode == NVN_EVENT_SIGNAL_MODE_ADD ? ev[k].v + e->value : e->value;
}

static const op_hdr *op_at(const uint8_t *p) { return (const op_hdr *)(const void *)p; }

// Follows the ops of [p, end) (no BEGIN or END among them): their signals reach the events; returns the first wait not
// met (stop), or end. stop 0: past every wait (the census), counting those not met.
static const uint8_t *follow(const uint8_t *p, const uint8_t *end, int stop) {
    for (; p < end; p += op_at(p)->size) {
        const op_hdr *h = op_at(p);
        if (h->op == OP_SIGNAL_EVENT) signal(nvn_op_payload(h));
        else if (h->op == OP_WAIT_EVENT) {
            if (!wait_met(nvn_op_payload(h))) {
                if (stop) return p;
                st_unmet++;
            }
            st_waits++;
        }
    }
    return end;
}

static Span handle_ops(uint64_t handle) {   // a recording's ops between its OP_BEGIN and OP_END
    const uint8_t *p = (const uint8_t *)nvn_op_next(nvn_op_first(handle)), *e = p;
    while (op_at(e)->op != OP_END) e += op_at(e)->size;
    return (Span){p, e};
}

static void out_put(const void *src, size_t n) {
    if (out.n + n > out.cap) {
        out.cap = (out.n + n) * 2 > 65536 ? (out.n + n) * 2 : 65536;
        if (!(out.b = realloc(out.b, out.cap))) rt_fatal("nvn: out of memory (queue order stream)");
    }
    memcpy(out.b + out.n, src, n);
    out.n += n, st_bytes += n;
}

static void out_ops(const void *queue, const uint8_t *p, const uint8_t *end) {   // the queue's ops [p, end)
    if (p == end) return;
    if (queue != out.queue) {
        struct { op_hdr h; p_queue q; } m = {{OP_QUEUE, sizeof m}, {(uintptr_t)queue}};
        out_put(&m, sizeof m);
        out.queue = queue;
    }
    out_put(p, (size_t)(end - p));
}

static Piece *piece_new(size_t n) {
    Piece *pc = malloc(sizeof *pc + n);
    if (!pc) rt_fatal("nvn: out of memory (held submission, %zu bytes)", n);
    pc->next = NULL, pc->n = 0;
    return pc;
}

static void piece_add(Piece *pc, const uint8_t *p, const uint8_t *end) {
    memcpy(pc->ops + pc->n, p, (size_t)(end - p));
    pc->n += (size_t)(end - p);
}

static void run_push(Run *r, Piece *pc) {
    if (r->tail) r->tail->next = pc;
    else r->head = pc;
    r->tail = pc;
}

static Run *run_of(const void *queue) {
    for (int i = 0; i < nruns; i++)
        if (runs[i].queue == queue) return &runs[i];
    return NULL;
}

// A submission of a queue that has no held work: its ops go into the stream as far as its waits allow; the rest is
// held in a new run. Only signals reach the events (follow), so handles before the stop are complete.
static void run_new_submission(const void *queue, const uint64_t *handles, int n) {
    for (int i = 0; i < n; i++) {
        Span s = handle_ops(handles[i]);
        const uint8_t *stop = follow(s.p, s.end, 1);
        out_ops(queue, s.p, stop);
        if (stop == s.end) continue;
        size_t bytes = (size_t)(s.end - stop);
        for (int j = i + 1; j < n; j++) {
            Span t = handle_ops(handles[j]);
            bytes += (size_t)(t.end - t.p);
        }
        Piece *pc = piece_new(bytes);
        piece_add(pc, stop, s.end);
        for (int j = i + 1; j < n; j++) {
            Span t = handle_ops(handles[j]);
            piece_add(pc, t.p, t.end);
        }
        runs[nruns++] = (Run){queue, pc, pc, 0};
        return;
    }
}

// A run goes on from its wait (past it: met now, or forced) as far as its waits allow; 0 when it has no work left.
static int run_resume(Run *r) {
    Piece *pc = r->head;
    const uint8_t *p = pc->ops + r->pos;
    p += op_at(p)->size, st_waits++;
    for (;;) {
        const uint8_t *end = pc->ops + pc->n, *stop = follow(p, end, 1);
        out_ops(r->queue, p, stop);
        if (stop != end) return r->pos = (size_t)(stop - pc->ops), 1;
        r->head = pc->next;
        free(pc);
        if (!(pc = r->head)) return r->tail = NULL, 0;
        p = pc->ops;
    }
}

static void run_drop(int i) {
    for (; i + 1 < nruns; i++) runs[i] = runs[i + 1];   // (in the order they were held: the oldest first)
    nruns--;
}

static void schedule(void) {
    for (int moved = 1; moved;) {
        moved = 0;
        for (int i = 0; i < nruns; i++)
            if (wait_met(nvn_op_payload(op_at(runs[i].head->ops + runs[i].pos)))) {
                if (!run_resume(&runs[i])) run_drop(i);
                moved = 1;
                break;
            }
    }
}

static void fill(FeSlot *s) {   // its content decided and its back-end slot known
    gfx_backend()->held_fill(s->slot, &s->h, s->h ? 1 : 0);
    free(s);
}

// No queue waits: the first held submission gets the stream, the others nothing.
static void close_episode(void) {
    uint64_t h = out.n ? nvn_stream_handle(out.b, out.n) : 0;
    for (int i = 0; i < nslots; i++) {
        FeSlot *s = slots[i];
        s->decided = 1, s->h = i ? 0 : h;
        if (s->attached) fill(s);
    }
    nslots = 0, out.n = 0, out.queue = NULL;
}

static int force(void) {   // qo held: every queue on, the oldest waiting one past its wait whenever all of them wait
    int n = 0;
    while (nruns) {
        schedule();
        if (!nruns) break;
        st_forced++, n++;
        if (!run_resume(&runs[0])) run_drop(0);
    }
    if (nslots) close_episode();
    return n;
}

int gfx_held_force(void) {
    plat_mutex_lock(&qo);
    int n = force();
    plat_mutex_unlock(&qo);
    return n;
}

static void log_line(void) {
    static uint64_t subs;
    if (++subs % 1500) return;
    rt_log("nvn: queue order in the last 1500 submits: %llu event waits, %llu not met in submit order (the census, "
           "MKW_QUEUE_WAITS=0); %llu episodes, %llu submissions held, %llu waits let go, %llu queues let on by the CPU, "
           "%.1f MiB of ops ordered", (unsigned long long)st_waits, (unsigned long long)st_unmet,
           (unsigned long long)st_episodes, (unsigned long long)st_held, (unsigned long long)st_forced,
           (unsigned long long)st_cpu, st_bytes / 1048576.0);
    st_waits = st_unmet = st_episodes = st_held = st_forced = st_cpu = st_bytes = 0;
}

int nvn_qorder_submit(const void *queue, const uint64_t *handles, int n, void **fe) {
    int m = mode(), held = 0;
    plat_mutex_lock(&qo);
    log_line();
    if (!RT_KNOB_ON(m)) {
        if (nslots) force();   // (the B block ended)
        if (nvn_sub_events || nvn_sub_waits)
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                follow(s.p, s.end, 0);
            }
    } else if (!nslots && !nvn_sub_waits) {   // (nothing held and no wait: as it is; its signals are followed)
        if (nvn_sub_events)
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                follow(s.p, s.end, 1);
            }
    } else {
        if (nslots == SLOTS_MAX || (!run_of(queue) && nruns == RUNS_MAX)) force();
        int was_open = nslots > 0;
        Run *r = run_of(queue);
        if (r) {   // (behind its queue's held work)
            size_t bytes = 0;
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                bytes += (size_t)(s.end - s.p);
            }
            Piece *pc = piece_new(bytes);
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                piece_add(pc, s.p, s.end);
            }
            run_push(r, pc);
        } else {
            size_t at = out.n;
            run_new_submission(queue, handles, n);
            schedule();
            if (!was_open && !nruns) st_bytes -= out.n - at, out.n = at, out.queue = NULL;   // (nothing waited: as it is)
        }
        held = was_open || nruns;
        if (held) {
            if (!was_open) st_episodes++;
            st_held++;
            FeSlot *s = calloc(1, sizeof *s);
            if (!s) rt_fatal("nvn: out of memory (held submission)");
            slots[nslots++] = s;
            *fe = s;
            if (!nruns) close_episode();
        }
    }
    plat_mutex_unlock(&qo);
    return held;
}

void nvn_qorder_attach(void *fe, void *slot) {
    FeSlot *s = fe;
    plat_mutex_lock(&qo);
    s->slot = slot, s->attached = 1;
    if (s->decided) fill(s);
    plat_mutex_unlock(&qo);
}

void nvn_event_cpu_signal(volatile uint32_t *val) {   // the event's memory is its value again
    plat_mutex_lock(&qo);
    for (int k = 0; k < EV_MAX; k++)
        if (ev[k].val == val) ev[k].v = *val;
    if (nruns) {
        int before = nruns;
        schedule();
        st_cpu += (uint64_t)(before - nruns);
        if (!nruns) close_episode();
    }
    plat_mutex_unlock(&qo);
}
