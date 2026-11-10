// The draw, dispatch and bind ops of the draw replay (vk_draw_internal.h): vk_exec_op_draw, the strong hook behind
// vk_exec.c's weak one, and vk_exec_bind_state, the bind ops' effect on the replay state alone. UPDATE_UB is a buffer
// copy from transient memory between full barriers.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "tex.h"
#include "vk.h"
#include "vk_draw_internal.h"
#include "vk_exec.h"
#include "vk_reuse.h"
#include "platform/platform.h"

static int prepare_draw(VkExec *ex, int prim, int32_t count, int restart) { return prepare_draw_ex(ex, prim, count, restart, 0); }

static void bind_index(VkExec *ex, const void *data, uint64_t bytes, VkIndexType type) {
    if (ru_cap) ru_note_bad(RUB_SCRATCH_INDEX);   // indices made at record time into this command buffer's scratch
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, bytes, &b, &o), data, bytes);
    vkCmdBindIndexBuffer(ex->cb, b, o, type);
    ex->recorded++;
}

static void op_draw(VkExec *ex, const p_draw *d) {
    c0_binst = (uint32_t)d->base_instance;
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;
    vk_exec_frame.draws++;
    uint32_t inst = d->instances > 0 ? (uint32_t)d->instances : 1, first = (uint32_t)d->first, n = (uint32_t)d->count;
    if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4) {   // quads become two triangles each
        uint32_t q = n / 4;
        uint32_t *ix = malloc(sizeof *ix * 6 * q);
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < q; i++) {
            uint32_t v = first + 4 * i, k[6] = {0, 1, 2, 0, 2, 3};
            for (int j = 0; j < 6; j++) ix[6 * i + j] = v + k[j];
        }
        bind_index(ex, ix, sizeof *ix * 6 * q, VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, 6 * q, inst, 0, 0, (uint32_t)d->base_instance);
    } else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2) {   // a strip that returns to its first vertex
        uint32_t *ix = malloc(sizeof *ix * (n + 1));
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < n; i++) ix[i] = first + i;
        ix[n] = first;
        bind_index(ex, ix, sizeof *ix * (n + 1), VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, n + 1, inst, 0, 0, (uint32_t)d->base_instance);
    } else {
        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);
    }
    ex->recorded++;
}

// DrawMeshTasks(first, count): count task (or mesh) workgroups. NVN's first has no Vulkan counterpart; the game's
// calls are logged when it is not 0 (it would have to reach the shader as an offset of the workgroup index).
static void op_draw_mesh(VkExec *ex, const p_draw_mesh *d) {
    static PFN_vkCmdDrawMeshTasksEXT draw_mesh;
    if (!draw_mesh) draw_mesh = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(vk_ctx()->device, "vkCmdDrawMeshTasksEXT");
    if (!draw_mesh || d->count <= 0) return;
    static int firsts;
    if (d->first && firsts < 8) firsts++, rt_log("vk_exec: DrawMeshTasks with first %d (count %d): drawn from group 0", d->first, d->count);
    if (!prepare_draw_ex(ex, NVN_DRAW_PRIMITIVE_TRIANGLES, d->count, 0, 1)) return;
    vk_exec_frame.draws++;
    rec_draw_mesh(draw_mesh, ex->cb, (uint32_t)d->count, 1, 1);   // through the recorder thread, as every command
    ex->recorded++;
}

// Primitive restart of an indexed draw. NVN restarts strips wherever an index equals the SetPrimitiveRestart value;
// Vulkan restarts only at the all-ones value of the bound index type, and only strips and fans without
// primitiveTopologyListRestart. Byte indices are widened to 16 bits, so their restart value becomes 0xffff
// (op_draw_idx). Without restart the 0xffff entries of TotK's terrain and cliff strips fetch vertex 65535 + base and
// the triangles around them drop out.
static int restart_for(const VkExec *ex, int prim, int itype) {
    if (!ex->prim_restart.enable) return 0;
    switch (vk_pipeline_topology(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: break;
    default: return 0;   // lists: a restart index only drops the unfinished primitive
    }
    static const uint32_t all_ones[3] = {0xffu, 0xffffu, 0xffffffffu};
    if (ex->prim_restart.index == all_ones[itype]) return 1;
    warn_once(W_RESTART, "primitive restart index %#x with %d-byte indices is not replayed: strips drawn without restart",
              ex->prim_restart.index, 1 << itype);
    return 0;
}

static void op_draw_idx(VkExec *ex, const p_draw_idx *d) {
    c0_binst = (uint32_t)d->base_instance;
    static const uint32_t isize[3] = {1, 2, 4};
    if (d->itype < 0 || d->itype > 2) return warn_once(W_INDEX, "index type %d is unknown: draw skipped", d->itype);
    int restart = restart_for(ex, d->prim, d->itype);
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, restart)) return;
    vk_exec_frame.draws++;
    uint64_t bytes = (uint64_t)d->count * isize[d->itype];
    VkMemSpan sp;
    if (!vk_mem_resolve(d->ibuf, bytes, &sp)) return warn_once(W_INDEX, "index buffer %#llx is not backed: draws with it are skipped", (unsigned long long)d->ibuf);
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {   // no uint8 index type here: widen (host-readable backing only)
        if (sp.alloc) return warn_once(W_INDEX, "byte indices in device-local memory: draw skipped");
        vk_mem_count(sp.cls, VK_MUSE_CPU_READ, bytes);
        vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, 2 * bytes);
        uint16_t *ix = malloc(sizeof *ix * (size_t)d->count);
        if (!ix) rt_fatal("vk: out of memory");
        for (int i = 0; i < d->count; i++) {
            uint8_t v = ((const uint8_t *)(uintptr_t)d->ibuf)[i];
            ix[i] = restart && v == 0xff ? 0xffff : v;
        }
        bind_index(ex, ix, sizeof *ix * (size_t)d->count, VK_INDEX_TYPE_UINT16);
        free(ix);
    } else if (sp.offset % isize[d->itype]) {
        return warn_once(W_INDEX, "index buffer %#llx is not aligned to its index size: draw skipped", (unsigned long long)d->ibuf);
    } else {
        VK_MEM_USE(sp);
        vkCmdBindIndexBuffer(ex->cb, sp.buffer, sp.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        ex->recorded++;
        vk_mem_count(sp.cls, VK_MUSE_IB, bytes);
        VK_MEM_BOUND(sp);
    }
    vkCmdDrawIndexed(ex->cb, (uint32_t)d->count, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex,
                     (uint32_t)d->base_instance);
    ex->recorded++;
}

static int lost_on_draw(void) {   // MKW_SHADOW_LOST read once
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_SHADOW_LOST");
        on = e && atoi(e) > 0;
    }
    return on;
}

// MKW_IND_SNAP (a measurement, default 0; "ab"): an indirect draw without a count whose arguments were copied at the game's submit
// (p_draw_idx_ind.snap) draws with that copy, unless a GPU write not copied back may touch them (then the GPU's bytes
// in the pool buffer are the right ones). The replay records ~3 submissions behind the game, which by then has written
// the next frame's arguments at the same address: a tree drew 35 instances from another first instance and read other
// trees' matrix and material, one dark frame (lf5-lf6, 06/11/2026). One "snap:" line per frame where a copy differs
// from guest memory at record time.
// With MKW_IND_SNAP off the count is not made unless MKW_IND_SNAP_COUNT=1 ("ab"): vk_mem_gpu_may_write takes
// shadow_lock, which the shadow's early thread holds while it syncs a pool, and the replay waited there 40 ms a second
// of race for a count nobody read (lp1).
static int snap_mode = -1, snap_count;
static uint64_t snap_frame, snap_n, snap_diff, snap_gpu;

static int use_snap(const p_draw_idx_ind *d, int indexed) {
    if (snap_mode < 0) {
        snap_mode = rt_knob("IND_SNAP", "0");
        snap_count = snap_mode ? 1 : rt_knob("IND_SNAP_COUNT", "0");
        rt_log("vk: indirect arguments as at the game's submit %s (MKW_IND_SNAP)",
               snap_mode == 2 ? "in the B blocks" : snap_mode ? "on" : "off");
    }
    if (!d->snapped || d->count || !RT_KNOB_ON(snap_count)) return 0;
    if (snap_frame != vk_exec_presented) {
        if (snap_diff) rt_log("snap: f%llu %llu draws, %llu changed since the submit, %llu with a GPU write pending",
                              (unsigned long long)snap_frame, (unsigned long long)snap_n, (unsigned long long)snap_diff,
                              (unsigned long long)snap_gpu);
        snap_frame = vk_exec_presented, snap_n = snap_diff = snap_gpu = 0;
        static int last = -1;   // the A/B edges, to count flicker per block (tools/imagen/pops.py)
        if (snap_mode == 2 && RT_KNOB_ON(snap_mode) != last)
            last = RT_KNOB_ON(snap_mode), rt_log("indb: f%llu snap %s", (unsigned long long)snap_frame, last ? "on" : "off");
    }
    uint32_t n = indexed ? 20 : 16;
    snap_n++;
    if (vk_mem_gpu_may_write(d->ind, n)) return snap_gpu++, 0;
    snap_diff += memcmp(d->snap, (const void *)(uintptr_t)d->ind, n) != 0;
    return RT_KNOB_ON(snap_mode);
}

// DrawElementsIndirect: the arguments are read by the GPU from the pool buffer (they are often written by an earlier
// dispatch), as nxnative nvn_mtl.m does. The base instance stays out of c0 (0x644): c0 cannot be patched by a transfer
// inside a pass; the translator reads a VS's c0[0x644] as gl_BaseInstance, the argument's (sm86 core.py const). Byte
// indices need the count to be widened: skipped.
static void op_draw_idx_ind(VkExec *ex, const p_draw_idx_ind *d) {
    c0_binst = 0;
    int indexed = d->itype >= 0;   // -1: DrawArraysIndirect, VkDrawIndirectCommand arguments
    if (indexed && d->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && d->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return warn_once(W_INDEX, "indirect draw with index type %d: skipped", d->itype);
    VkMemSpan ib = {0}, ip, cp = {0};
    uint32_t max = d->count ? (uint32_t)(d->max > 0 ? d->max : 0) : 1, stride = d->count ? (uint32_t)d->stride : 0;
    if (!max) return;
    uint32_t one = indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    if (d->count && (stride < one || stride % 4))
        return warn_once(W_INDIRECT, "indirect count draw with stride %d: skipped", d->stride);
    uint64_t span = (uint64_t)(max - 1) * stride + one;
    if (!vk_mem_resolve(d->ind, span, &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (d->count && (!vk_mem_resolve(d->count, 4, &cp) || cp.offset % 4))
        return warn_once(W_INDIRECT, "indirect draw count at %#llx is not backed or not aligned: draws skipped", (unsigned long long)d->count);
    if (indexed && (!vk_mem_resolve(d->ibuf, 1, &ib) || ib.offset % (d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4)))
        return warn_once(W_INDEX, "index buffer %#llx is not backed or not aligned: indirect draws skipped", (unsigned long long)d->ibuf);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, indexed ? restart_for(ex, d->prim, d->itype) : 0)) return;
    vk_exec_frame.draws++;
    vk_ind_note_pass();
    ind_trace(ex, d, &ip, &cp, max, stride);
    if (lost_on_draw()) {
        int has_vs;
        const GfxShader *vs = shader_for(ex, NVN_STAGE_VS, &has_vs);
        const uint8_t *k = vs ? gfx_shader_input_id(vs) : NULL;
        uint32_t tag = k ? (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | k[3] : 0;
        vk_mem_lost_watch(ip.pool, d->ind, span, tag);
        if (d->count) vk_mem_lost_watch(cp.pool, d->count, 4, tag);
    }
    {
        int has_vs;
        const GfxShader *vs = shader_for(ex, NVN_STAGE_VS, &has_vs);
        const uint8_t *k = vs ? gfx_shader_input_id(vs) : NULL;
        uint32_t tag = k ? (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | k[3] : 0;
        vk_ind_read_note(ip.buffer, ip.offset, stride ? stride : one, max, cp.buffer, cp.offset, tag, d->count != 0, indexed,
                         !ip.alloc ? d->ind : 0, ex);
    }
    VK_MEM_USE(ip);
    if (indexed) {
        VK_MEM_USE(ib);
        vkCmdBindIndexBuffer(ex->cb, ib.buffer, ib.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
    if (d->count) VK_MEM_USE(cp);
    if (use_snap(d, indexed)) {
        if (indexed) vkCmdDrawIndexed(ex->cb, d->snap[0], d->snap[1], d->snap[2], (int32_t)d->snap[3], d->snap[4]);
        else vkCmdDraw(ex->cb, d->snap[0], d->snap[1], d->snap[2], d->snap[3]);
    } else if (indexed && d->count) vkCmdDrawIndexedIndirectCount(ex->cb, ip.buffer, ip.offset, cp.buffer, cp.offset, max, stride);
    else if (indexed) vkCmdDrawIndexedIndirect(ex->cb, ip.buffer, ip.offset, 1, one);
    else if (d->count) vkCmdDrawIndirectCount(ex->cb, ip.buffer, ip.offset, cp.buffer, cp.offset, max, stride);
    else vkCmdDrawIndirect(ex->cb, ip.buffer, ip.offset, 1, one);
    ex->recorded += 2;
    vk_mem_count(ip.cls, VK_MUSE_IND, span);
    VK_MEM_BOUND(ip);
    if (d->count) VK_MEM_BOUND(cp);
    if (indexed) VK_MEM_BOUND(ib);
}

// ---- dispatch and uniform buffer updates

// MKW_DISPATCH_NOBARRIER (default 0; "ab": in the B blocks): no barrier between a dispatch and the one right after
// it. A measurement only, of what the full barrier before every dispatch costs (~110 a race frame, rt-y5); without it
// a dispatch may read what the previous one has not written yet.
static void dispatch_barrier(VkExec *ex) {
    static int mode = -1;
    if (mode < 0) {
        mode = rt_knob("DISPATCH_NOBARRIER", "0");
        if (mode) rt_log("vk: no barrier between consecutive dispatches%s (MKW_DISPATCH_NOBARRIER, a measurement)",
                         mode == 2 ? " in the B blocks" : "");
    }
    if (mode && RT_KNOB_ON(mode) && !ex->in_pass && ex->last_dispatch == ex->recorded) return;
    vk_exec_barrier(ex);
}

static void op_dispatch(VkExec *ex, const p_dispatch *d) {
    if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    cb_res(ex);
    dummies_init();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present);   // synchronous: a skipped dispatch changes results
    if (!cs) return;
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    if (ex->render_disabled && vk_cond_trace > 0) {   // MKW_COND_TRACE: dispatches run while a condition says skip
        const uint8_t *k = gfx_shader_input_id(cs);
        vk_cond_trace--;
        rt_log("cond: f%llu dispatch while disabled cs %02x%02x%02x%02x %dx%dx%d", (unsigned long long)vk_exec_presented,
               k[0], k[1], k[2], k[3], d->x, d->y, d->z);
    }
    dispatch_barrier(ex);   // ends the pass; earlier writes are visible to the shader
    vk_ubo_watch(ex, NVN_STAGE_CS);   // MKW_UBO_WATCH (diag/vk_ubo_watch.c)
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    recent_note(cs, NULL);
    vk_crumbs_set_state((uint64_t)(uintptr_t)p, gfx_shader_key(cs), NULL, "Dispatch", 0);
    c0_dim[0] = (uint32_t)d->x, c0_dim[1] = (uint32_t)d->y, c0_dim[2] = (uint32_t)d->z, c0_cs = cs;
    bind_resources(ex, info);
    int pt = vk_pt_enabled();
    if (vk_spec_census_on()) spec_census(ex, cs, NVN_STAGE_CS);
    if (pt && vk_pt_draw_mode()) vk_pt_draw(ex->cb, cs, NULL);
    else if (pt) vk_pt_mark(ex->cb, 1u << 31 | (uint32_t)(ex->prog->serial & 0x7fffffff));
    vk_cs_hash(ex, d->caller, 0, 0);   // MKW_CS_HASH (diag/vk_cs_hash.c)
    uint32_t crumb = vk_crumb_begin(ex->cb, VK_CRUMB_DISPATCH, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z, 0, NULL, 0);
    vkCmdDispatch(ex->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_crumb_end(ex->cb, VK_CRUMB_DISPATCH, crumb);
    vk_cs_hash(ex, d->caller, 0, 1);
    if (pt) vk_pt_mark(ex->cb, 0);
    ex->recorded++;
    ex->last_dispatch = ex->recorded;
    vk_trace_3d_writes(ex, cs, info, d);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, d);
}

// DispatchComputeIndirect: vkCmdDispatchIndirect from the pool buffer. The shader reads the group counts from c0 0x4,
// so they are copied there on the GPU, into a c0 range no other dispatch uses, before the dispatch.
static void op_dispatch_ind(VkExec *ex, const p_dispatch_ind *d) {
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDispatchIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect dispatch arguments at %#llx are not backed or not aligned: skipped", (unsigned long long)d->ind);
    cb_res(ex);
    dummies_init();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present);
    if (!cs) return;
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    if (ex->render_disabled && vk_cond_trace > 0) {   // MKW_COND_TRACE
        const uint8_t *k = gfx_shader_input_id(cs);
        vk_cond_trace--;
        rt_log("cond: f%llu indirect dispatch while disabled cs %02x%02x%02x%02x", (unsigned long long)vk_exec_presented,
               k[0], k[1], k[2], k[3]);
    }
    vk_exec_barrier(ex);
    VK_MEM_USE(ip);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    recent_note(cs, NULL);
    vk_crumbs_set_state((uint64_t)(uintptr_t)p, gfx_shader_key(cs), NULL, "DispatchIndirect", 0);
    c0_dim[0] = c0_dim[1] = c0_dim[2] = 0, c0_cs = cs;
    B.c0_valid[2] = 0;   // a fresh c0 range: the copy below must not change one an earlier dispatch reads
    bind_resources(ex, info);
    if (B.c0_valid[2]) {   // the shader binds c0
        VkBufferCopy cp = {ip.offset, B.c0_off[2] + VK_C0_DISPATCH, sizeof(VkDispatchIndirectCommand)};
        vkCmdCopyBuffer(ex->cb, ip.buffer, B.c0_buf[2], 1, &cp);
        ex->recorded++;
        vk_exec_barrier(ex);
        B.c0_valid[2] = 0;   // the range no longer holds B.c0[2]
    }
    vk_ubo_watch(ex, NVN_STAGE_CS);   // MKW_UBO_WATCH: uw1/uw2 missed the culling dispatch, an indirect one
    vk_cs_hash(ex, d->caller, d->ind, 0);   // MKW_CS_HASH (diag/vk_cs_hash.c)
    uint32_t crumb = vk_crumb_begin(ex->cb, VK_CRUMB_DISPATCH, 0, 0, 0, 0, NULL, 0);
    vkCmdDispatchIndirect(ex->cb, ip.buffer, ip.offset);
    vk_crumb_end(ex->cb, VK_CRUMB_DISPATCH, crumb);
    vk_cs_hash(ex, d->caller, 0, 1);
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDispatchIndirectCommand));
    VK_MEM_BOUND(ip);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, &(p_dispatch){0, 0, 0, d->caller});
}

static void op_update_ub(VkExec *ex, const p_update_ub *u) {
    if (!u->size) return;
    VkMemSpan sp;
    if (!vk_mem_resolve(u->dst, u->size, &sp)) return warn_once(W_UB, "UpdateUniformBuffer to %#llx is not backed: dropped", (unsigned long long)u->dst);
    cb_res(ex);
    vk_exec_barrier(ex);   // earlier draws finished with the old contents
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, u->size, &b, &o), u->data, u->size);
    VK_MEM_PICK(sp);
    VkBufferCopy cp = {o, sp.offset, u->size};
    vkCmdCopyBuffer(ex->cb, b, sp.buffer, 1, &cp);
    vk_mem_gpu_wrote(&sp, u->size);
    vk_usnap_drop(u->dst, u->size);
    ex->recorded++;
    vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
    note_written(ex, u->dst, u->size);
    upd_note(ex, u);
}

// ---- bind ops

static void bind_handles(uint64_t *dst, int cap, const p_bind_handles *b) {
    if (b->stage < 0 || b->stage >= NVN_BIND_STAGES || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) {
        warn_once(W_BIND, "a bind of %d handles at %d of stage %d is out of range: ignored", b->n, b->first, b->stage);
        return;
    }
    memcpy(&dst[b->first], b->h, sizeof(uint64_t) * (size_t)b->n);
}

// DrawMeshTasks without VK_EXT_mesh_shader: the task program (if any) and the mesh
// program run as compute shaders, the task's into records, the mesh's into vertices and triangles (device-local
// scratch, vk_mesh_cs.c), and a plain indexed draw of those with the fragment shader of the program. A task workgroup
// launches MESH_CS_PER_TASK mesh workgroups whether it uses them or not: the unused ones write degenerate triangles.
#define MESH_CS_PER_TASK 32
#define MESH_CS_TASK_REC 1025   // words a task record: the payload, then its mesh count (head.py TASK_REC)
#define MESH_CS_PRIMS 64

static void op_draw_mesh_cs(VkExec *ex, const p_draw_mesh *d) {
    static int skip_mc = -1;
    if (skip_mc < 0) {
        skip_mc = rt_knob("SKIP_MESH_COMPUTE", "0");
        if (skip_mc) rt_log("vk: mesh compute dispatches skipped (MKW_SKIP_MESH_COMPUTE)");
    }
    if (RT_KNOB_ON(skip_mc)) return;
    if (d->count <= 0) return;
    int present;
    const GfxShader *msh = NULL, *first = mesh_stages(ex, &msh, &present);
    if (!first) return;
    const GfxShader *task = msh ? first : NULL, *mesh = msh ? msh : first;
    const GfxShader *tcs = task ? gfx_shader_cs_variant(task) : NULL, *mcs = gfx_shader_cs_variant(mesh);
    if (!mcs || (task && !tcs)) return warn_once(W_PROGRAM, "a task/mesh draw without its compute form is skipped");
    const VkPipeInfo *ti = NULL, *mi = NULL;
    VkPipelineLayout cl = VK_NULL_HANDLE;
    VkPipeline tp = tcs ? vk_pipeline_compute(tcs, &ti) : VK_NULL_HANDLE, mp = vk_pipeline_compute(mcs, &mi);
    VkPipeline cp = task ? vk_mesh_cs_compact_pipeline(&cl) : VK_NULL_HANDLE;
    if (!mp || !mi || (tcs && (!tp || !ti || !cp)))
        return warn_once(W_PIPELINE, "a task/mesh compute pipeline could not be created: those draws are skipped");
    // room for every mesh workgroup the tasks could ask for; the compaction launches only those they did.
    // Clamp emitted vertex and primitive capacity to a single mesh scratch chunk (64 MiB).
    uint32_t max_tasks = (uint32_t)d->count;
    uint32_t max_chunk_groups = (uint32_t)((64ull << 20) / (32ull * MESH_CS_VWORDS * 4ull));
    uint32_t groups = task ? max_tasks * MESH_CS_PER_TASK : max_tasks;
    if (groups > max_chunk_groups) {
        groups = max_chunk_groups;
        if (task && max_tasks > groups / MESH_CS_PER_TASK) max_tasks = groups / MESH_CS_PER_TASK;
    }
    if (!groups || (task && !max_tasks)) return;
    uint64_t nidx = (uint64_t)groups * MESH_CS_PRIMS * 3;
    struct { VkDeviceAddress trec, mvo, mto, map; uint32_t has_task, max_groups; } pc = {0, 0, 0, 0, task != NULL, groups};
    VkBuffer tb, vb, ib, mb, ab = VK_NULL_HANDLE;
    VkDeviceSize to, vo, io, mo, ao = 0;
    VkDeviceAddress aa = 0;
    if (!vk_mesh_cs_alloc(ex->fence, task ? (VkDeviceSize)max_tasks * MESH_CS_TASK_REC * 4 : 256, &tb, &to, &pc.trec) ||
        !vk_mesh_cs_alloc(ex->fence, (VkDeviceSize)groups * 32 * MESH_CS_VWORDS * 4, &vb, &vo, &pc.mvo) ||
        !vk_mesh_cs_alloc(ex->fence, nidx * 4, &ib, &io, &pc.mto) ||
        !vk_mesh_cs_alloc(ex->fence, (VkDeviceSize)groups * 4, &mb, &mo, &pc.map) ||
        (task && !vk_mesh_cs_alloc(ex->fence, 32, &ab, &ao, &aa)))
        return warn_once(W_PIPELINE, "no device memory for a task/mesh draw as compute: skipped");
    if (vk_guard_on()) {
        vk_guard_address("mesh as compute: task records", pc.trec), vk_guard_address("mesh as compute: vertices", pc.mvo);
        vk_guard_address("mesh as compute: triangles", pc.mto), vk_guard_address("mesh as compute: map", pc.map);
        if (task) vk_guard_address("mesh as compute: indirect arguments", aa);
    }
    cb_res(ex);
    dummies_init();
    vk_crumbs_set_state((uint64_t)(uintptr_t)mp, tcs ? gfx_shader_key(tcs) : gfx_shader_key(mcs),
                        tcs ? gfx_shader_key(mcs) : NULL, "DrawMeshTasks (MKW_MESH_COMPUTE)", 1);
    dispatch_barrier(ex);   // ends the pass; earlier writes are visible to the passes
    if (task) {
        pc.max_groups = max_tasks;
        bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, tp);
        bind_resources(ex, ti);
        vkCmdPushConstants(ex->cb, ti->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(ex->cb, max_tasks, 1, 1);
        ex->recorded += 2;
        vk_exec_barrier(ex);
        struct { VkDeviceAddress trec, map, args; uint32_t tasks, pad; } cpc = {pc.trec, pc.map, aa, max_tasks, 0};
        bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
        vkCmdPushConstants(ex->cb, cl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cpc, &cpc);
        vkCmdDispatch(ex->cb, 1, 1, 1);
        ex->recorded += 2;
        vk_exec_barrier(ex);   // the map and the indirect arguments
        pc.max_groups = groups;
    }
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, mp);
    bind_resources(ex, mi);
    vkCmdPushConstants(ex->cb, mi->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    if (task) vkCmdDispatchIndirect(ex->cb, ab, ao);
    else vkCmdDispatch(ex->cb, groups, 1, 1);
    ex->recorded += 2;
    ex->last_dispatch = ex->recorded;
    vk_exec_barrier(ex);   // ends the pass if any; full barrier
    VkMemoryBarrier2 mb2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                         VK_ACCESS_2_INDEX_READ_BIT |
                         VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb2,
    };
    vkCmdPipelineBarrier2(ex->cb, &dep);
    if (nidx > UINT32_MAX || !prepare_draw_ex(ex, NVN_DRAW_PRIMITIVE_TRIANGLES, (int32_t)nidx, 0, 2)) {
        vk_crumbs_set_mesh_compute(0);
        return;
    }
    vk_exec_frame.draws++;
    VkDeviceSize stride = MESH_CS_VWORDS * 4, size = (VkDeviceSize)groups * 32 * stride;
    vkCmdBindVertexBuffers2(ex->cb, 0, 1, &vb, &vo, &size, vk_pipeline_vertex_input_dynamic() ? NULL : &stride);
    B.vb_valid = 0;   // binding 0 is ours now: the next draw binds its streams again
    vkCmdBindIndexBuffer(ex->cb, ib, io, VK_INDEX_TYPE_UINT32);
    if (task) vkCmdDrawIndexedIndirect(ex->cb, ab, ao + 12, 1, 20);   // args[3..7] (mesh_compact.comp)
    else vkCmdDrawIndexed(ex->cb, (uint32_t)nidx, 1, 0, 0, 0);
    vk_crumbs_set_mesh_compute(0);
    ex->recorded += 3;
}

// The bind ops' effect on the replay state alone: 1 when op is one. The light walk (vk_exec.c, MKW_REPLAY_WALK)
// follows them without touching what the recording caches (bind_gen).
int vk_exec_bind_state(VkExec *ex, uint32_t op, const void *payload) {
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        ex->prog = b->prog, ex->prog_stages = b->stages;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) ex->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&ex->pipe.color, b->bytes, sizeof ex->pipe.color); break;
        case 2: memcpy(&ex->pipe.chmask, b->bytes, sizeof ex->pipe.chmask); break;
        case 3: memcpy(&ex->pipe.ms, b->bytes, sizeof ex->pipe.ms); break;
        case 4: memcpy(&ex->pipe.poly, b->bytes, sizeof ex->pipe.poly); break;
        case 5: memcpy(&ex->pipe.ds, b->bytes, sizeof ex->pipe.ds); break;
        case 6: memcpy(&ex->point_size, b->bytes, sizeof ex->point_size); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vattr, 0, sizeof ex->pipe.vattr);   // MNG: the new list replaces the old one
        memcpy(&ex->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vstream, 0, sizeof ex->pipe.vstream);
        memcpy(&ex->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memcpy(&ex->vbuf[b->first], b->r, sizeof(NVNbufferRange) * (size_t)b->n);
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < NVN_BIND_STAGES && b->index >= 0 && b->index < 18) ex->ubo[b->stage][b->index] = *b;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < NVN_BIND_STAGES && b->index >= 0 && b->index < 16) ex->ssbo[b->stage][b->index] = *b;
        break;
    }
    case OP_BIND_TEX: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < NVN_BIND_STAGES ? ex->tex[b->stage] : NULL, 128, b);
        ex->tex_serial++;
        break;
    }
    case OP_BIND_SAMP: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < NVN_BIND_STAGES ? ex->samp[b->stage] : NULL, 128, b);
        break;
    }
    case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < NVN_BIND_STAGES ? ex->img[b->stage] : NULL, 32, b);
        ex->tex_serial++;   // feedback_scan reads only textures: a rescan at most
        break;
    }
    default: return 0;
    }
    return 1;
}

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    if (!dtl_b) vk_exec_draw_thread();
    if (op == OP_DRAW_MESH) {
        // MKW_MESH_SKIP (diagnosis, default 0; "ab": in the B blocks): task/mesh draws left out, to measure what they
        // cost the GPU within one run (the mesh-as-compute form on a GTX 1050 Ti).
        static int skip = -1;
        if (skip < 0) skip = rt_knob("MESH_SKIP", "0");
        if (RT_KNOB_ON(skip)) return;
        if (vk_mesh_cs_on()) op_draw_mesh_cs(ex, payload);
        else op_draw_mesh(ex, payload);
        return;
    }
    if (vk_exec_bind_state(ex, op, payload)) {
        if (op == OP_BIND_PROGRAM || op == OP_BIND_UBO || op == OP_BIND_SSBO || op == OP_BIND_TEX || op == OP_BIND_IMG)
            bind_gen++;
        return;
    }
    switch (op) {
    case OP_UPDATE_UB: op_update_ub(ex, payload); break;
    case OP_DRAW: op_draw(ex, payload); break;
    case OP_DRAW_IDX: op_draw_idx(ex, payload); break;
    case OP_DISPATCH: op_dispatch(ex, payload); break;
    case OP_DRAW_IDX_IND: op_draw_idx_ind(ex, payload); break;
    case OP_DISPATCH_IND: op_dispatch_ind(ex, payload); break;
    default: rt_fatal("vk: op %s is not a draw op", nvn_op_name(op));
    }
}
