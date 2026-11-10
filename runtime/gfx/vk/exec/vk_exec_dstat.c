// Diagnostics of the draw replay (vk_draw_internal.h): the last programs drawn (logged at a device loss), MKW_IND_TRACE,
// MKW_BIND_STATS's repeat census and the MKW_TRACE=perf report of prepare_draw and bind_resources by stage.
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
#include "platform/platform.h"

DrawRecent recent[RECENT_N];   // recent_note (vk_draw_internal.h)
volatile uint32_t recent_at;

// What the driver knows of the loss (VK_EXT_device_fault): on RADV the faulting GPU address and its kind, which a
// player's log lacked (issues #4 and #5: "upload submit failed (-4)" and nothing else). Once per process.
static void fault_log(void) {
    static int done;
    VkCtx *c = vk_ctx();
    if (!c || !c->has_device_fault || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(c->device, "vkGetDeviceFaultInfoEXT");
    VkDeviceFaultCountsEXT n = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (!get || get(c->device, &n, NULL) != VK_SUCCESS) {
        rt_log("vk: device fault: no information from the driver");
        return;
    }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vendor[16];
    if (n.addressInfoCount > 16) n.addressInfoCount = 16;
    if (n.vendorInfoCount > 16) n.vendorInfoCount = 16;
    n.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr,
                                 .pVendorInfos = vendor};
    VkResult r = get(c->device, &n, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        rt_log("vk: device fault: no information from the driver (%d)", (int)r);
        return;
    }
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "instruction pointer unknown",
                                       "instruction pointer invalid", "instruction pointer fault"};
    rt_log("vk: device fault: %s; %u addresses, %u vendor records", info.description, n.addressInfoCount, n.vendorInfoCount);
    for (uint32_t i = 0; i < n.addressInfoCount; i++)
        rt_log("vk: device fault: %s at %#llx (precision %#llx)",
               addr[i].addressType < sizeof kind / sizeof *kind ? kind[addr[i].addressType] : "?",
               (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < n.vendorInfoCount; i++)
        rt_log("vk: device fault: vendor %s, code %#llx, data %#llx", vendor[i].description,
               (unsigned long long)vendor[i].vendorFaultCode, (unsigned long long)vendor[i].vendorFaultData);
}

void vk_exec_recent_log(void) {
    vk_crumbs_dump_active();
    fault_log();
    vk_crumbs_report();
    uint32_t n = __atomic_load_n(&recent_at, __ATOMIC_ACQUIRE), from = n > RECENT_N ? n - RECENT_N : 0;
    rt_log("vk_exec: the last %u programs drawn or dispatched with (oldest first; prepared SPIR-V keys):", n - from);
    for (uint32_t i = from; i < n; i++) {
        char a[65], b[65] = "";
        for (int k = 0; k < 32; k++) sprintf(a + 2 * k, "%02x", recent[i % RECENT_N].key[0][k]);
        if (recent[i % RECENT_N].has_fs)
            for (int k = 0; k < 32; k++) sprintf(b + 2 * k, "%02x", recent[i % RECENT_N].key[1][k]);
        rt_log("  %s %s", a, b);
    }
    rt_log_flush();
}

// The bound state a draw reads, compared with the previous draw's (MKW_BIND_STATS only).
void repeat_stats(const VkExec *ex, const VkPipeInfo *info) {
    static struct {
        const prog_t *prog;
        VkPipeState pipe;
        uint64_t tex[2][128], img[2][32];
        const VkPipeInfo *info;
        p_bind_buf ubo[2][18], ssbo[2][16];
        float vp[6], point_size;
    } last, cur;
    if (!bind_stats_on()) return;
    memset(&cur, 0, sizeof cur);
    cur.prog = ex->prog, cur.pipe = ex->pipe, cur.info = info;
    const int st[2] = {NVN_STAGE_VS, NVN_STAGE_FS};
    for (int k = 0; k < 2; k++) {
        memcpy(cur.tex[k], ex->tex[st[k]], sizeof cur.tex[k]), memcpy(cur.img[k], ex->img[st[k]], sizeof cur.img[k]);
        memcpy(cur.ubo[k], ex->ubo[st[k]], sizeof cur.ubo[k]), memcpy(cur.ssbo[k], ex->ssbo[st[k]], sizeof cur.ssbo[k]);
    }
    memcpy(cur.vp, ex->vp[0], sizeof cur.vp), cur.point_size = ex->point_size;
    size_t prog_part = offsetof(typeof(cur), info);
    if (!memcmp(&cur, &last, prog_part)) {
        BS.same_prog++;
        if (!memcmp((const char *)&cur + prog_part, (const char *)&last + prog_part, sizeof cur - prog_part)) BS.same_all++;
    }
    last = cur;
}

// MKW_IND_TRACE=<lines> (diagnosis, default 0): each indirect draw's VS (program key prefix), argument and count
// addresses and, from guest memory when the arguments are host-backed, the count and the first four draws' instance
// count and first instance as the CPU sees them while recording: GPU-written ones show the last completed write.
// Found for the title's flickering trees (06/11/2026).
void ind_trace(VkExec *ex, const p_draw_idx_ind *d, const VkMemSpan *ip, const VkMemSpan *cp, uint32_t max,
                      uint32_t stride) {
    static int left = -1;
    if (left < 0) {
        const char *e = cfg_get("MKW_IND_TRACE");
        left = e ? atoi(e) : 0;
    }
    if (!left) return;
    left--;
    int has_vs;
    const GfxShader *vs = shader_for(ex, NVN_STAGE_VS, &has_vs);
    const uint8_t *k = vs ? gfx_shader_input_id(vs) : NULL;
    char args[160] = "";
    int o = 0;
    uint32_t n = max < 4 ? max : 4, step = stride ? stride : (d->itype >= 0 ? 20 : 16);
    if (!ip->alloc && (!d->count || !cp->alloc)) {
        uint32_t cnt = d->count ? *(const volatile uint32_t *)(uintptr_t)d->count : 1;
        o += snprintf(args + o, sizeof args - (size_t)o, " count=%u", cnt);
        for (uint32_t i = 0; i < n && o < (int)sizeof args - 24; i++) {
            const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(d->ind + (uint64_t)i * step);
            // indexed: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance; else: vertexCount,
            // instanceCount, firstVertex, firstInstance
            o += snprintf(args + o, sizeof args - (size_t)o, " [%u x%u @%u]", w[0], w[1], w[d->itype >= 0 ? 4 : 3]);
        }
    } else {
        snprintf(args, sizeof args, " (device memory)");
    }
    rt_log("ind: f%llu vs %02x%02x%02x%02x itype %d ind %#llx count %#llx max %u stride %u%s",
           (unsigned long long)vk_exec_presented, k ? k[0] : 0, k ? k[1] : 0, k ? k[2] : 0, k ? k[3] : 0, d->itype,
           (unsigned long long)d->ind, (unsigned long long)d->count, max, stride, args);
}

int vk_exec_draw_timing(char *buf, int size, double us_per_tick) {
    static const char *const name[PD_N] = {"pass and scissor", "shaders", "feedback", "pipeline", "pipeline bind and state",
                                           "resources", "vertex buffers"};
    static const char *const brn[BR_N] = {"signature", "c0 and support", "uniform blocks", "storage buffers", "textures",
                                          "null check", "sets and pushes"};
    int k = 0;
    for (int i = 0; i < PD_N && pd_n && k < size; i++)
        k += snprintf(buf + k, (size_t)(size - k), "%s %s %.2f", i ? "," : "", name[i], (double)pd_tsc[i] / (double)pd_n * us_per_tick);
    for (int i = 0; i < BR_N && pd_n && k < size; i++)
        k += snprintf(buf + k, (size_t)(size - k), "%s %s %.2f", i ? "," : "; resources:", brn[i], (double)br_tsc[i] / (double)pd_n * us_per_tick);
    if (pd_n && k < size)
        k += snprintf(buf + k, (size_t)(size - k), "; textures per draw: memo hits %.2f (first in the submission %.2f), "
                      "misses %.2f (%.2f us each), not memoised %.2f; hit %.3f us, lock %.3f us per draw", td_stat[0] / (double)pd_n, td_stat[1] / (double)pd_n,
                      td_stat[2] / (double)pd_n, td_stat[2] ? (double)td_stat[4] / (double)td_stat[2] * us_per_tick : 0.0,
                      td_stat[3] / (double)pd_n, td_stat[0] ? (double)td_stat[5] / (double)td_stat[0] * us_per_tick : 0.0,
                      (double)td_stat[6] / (double)pd_n * us_per_tick);
    memset(td_stat, 0, sizeof td_stat);
    memset(pd_tsc, 0, sizeof pd_tsc), memset(br_tsc, 0, sizeof br_tsc), pd_n = 0;
    return k;
}
