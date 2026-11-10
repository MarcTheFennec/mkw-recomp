// Physical pool memory for runtime/gfx/cap: the chunks of POOL_PHYS records hold what only the GPU wrote (buffers a
// compute pass fills one frame and reads the next: lighting, exposure). A capture reads every backed chunk out, a replay
// writes it back at the same GPU address. Without them the race capture replayed with its scene dark (07/11/2026).
#include <stdlib.h>
#include <string.h>

#include "vk_mem_internal.h"

// Copies [off, off + n) of buf through a staging buffer, in either direction, and waits for it.
static void chunk_copy(VkBuffer buf, uint64_t off, void *host, uint64_t n, int to_gpu) {
    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *map = vk_staging(n, &sb, &sa);
    if (to_gpu) memcpy(map, host, n);
    VkCommandBuffer cb = vk_upload_begin();
    VkBufferCopy cp = {to_gpu ? 0 : off, to_gpu ? off : 0, n};
    vkCmdCopyBuffer(cb, to_gpu ? sb : buf, to_gpu ? buf : sb, 1, &cp);
    vk_upload_end();
    vk_upload_flush(1);
    if (!to_gpu) {
        vmaInvalidateAllocation(vk_ctx()->vma, sa, 0, VK_WHOLE_SIZE);
        memcpy(host, map, n);
    }
    vmaDestroyBuffer(vk_ctx()->vma, sb, sa);
}

uint64_t vk_mem_phys_dump(void (*emit)(uint64_t addr, const void *p, uint64_t n, void *ctx), void *ctx) {
    vk_sync_fence_wait(vk_sync_issued(), UINT64_MAX);
    plat_mutex_lock_shared(&pool_lock);
    int n = pool_index.n;
    PoolRec **recs = malloc(sizeof *recs * (size_t)(n + 1));
    if (!recs) rt_fatal("vk: out of memory");
    int k = 0;
    for (int i = 0; i < n; i++) {
        PoolRec *r = pool_index.e[i].buf;
        if (r && r->kind == POOL_PHYS && r->chunks) recs[k++] = r;
    }
    plat_mutex_unlock_shared(&pool_lock);
    uint64_t bytes = 0;
    uint8_t *tmp = NULL;
    uint64_t cap = 0;
    for (int i = 0; i < k; i++) {
        PoolRec *r = recs[i];
        for (uint32_t c = 0; c < r->nchunks; c++) {
            if (!r->chunks[c].buf) continue;
            uint64_t len = chunk_len(r, c);
            if (len > cap && !(tmp = realloc(tmp, cap = len))) rt_fatal("vk: out of memory for a chunk");
            chunk_copy(r->chunks[c].buf, 0, tmp, len, 0);
            emit(r->lo + (uint64_t)c * phys_chunk(), tmp, len, ctx);
            bytes += len;
        }
    }
    free(tmp);
    free(recs);
    return bytes;
}

int vk_mem_phys_load(uint64_t addr, const void *p, uint64_t n) {
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, n);
    PoolRec *r = e ? e->buf : NULL;
    plat_mutex_unlock_shared(&pool_lock);
    if (!r || r->kind != POOL_PHYS || !r->chunks) return 0;
    uint64_t off = addr - r->lo;
    uint32_t c = (uint32_t)(off / phys_chunk());
    if (off % phys_chunk() || n > chunk_len(r, c)) return 0;
    phys_back(r, c, c);
    if (!r->chunks[c].buf) return 0;
    chunk_copy(r->chunks[c].buf, 0, (void *)p, n, 1);
    return 1;
}
