#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

VkBufferUsageFlags pool_usage(const VkCtx *c) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (c->has_transform_feedback)
        u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (c->has_conditional_rendering) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    return u;
}

static int memory_type(const VkCtx *c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if (bits >> i & 1) return (int)i;
    return -1;
}

// Placed pool (VK_EXT_map_memory_placed): the pool's guest range becomes a cached host-visible allocation of the
// driver's, mapped by the driver at that very address, so the game's pages ARE the buffer's memory: no import (amdgpu
// revalidates imported userptrs at each submit) and no shadow copy (which doubles the pool in RAM and copies writes at
// every submit). The idea of smo-recomp perf/heap-placed (whole heap there; MKW's heap of ~8 GiB fits no Vulkan heap,
// so pool by pool). What the range holds is copied in before the placed map replaces its pages; a write another thread
// makes in between is lost (pools are set up before their contents are used). Undone in placed_release.
static PFN_vkGetMemoryHostPointerPropertiesEXT p_host_props;
static PFN_vkMapMemory2KHR p_map2;
int64_t placed_n, placed_bytes;   // live placed pools (vram_log)
static PFN_vkUnmapMemory2KHR p_unmap2;

static int placed_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    if (!p_map2) {
        p_map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(c->device, "vkMapMemory2KHR");
        p_unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(c->device, "vkUnmapMemory2KHR");
        if (!p_map2 || !p_unmap2) return 0;
    }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull, n = hi - lo;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    const char *why = NULL;
    VkMemoryRequirements req = {0};
    int type = -1;
    if (vkCreateBuffer(c->device, &bi, NULL, &buf) != VK_SUCCESS) why = "no buffer";
    else {
        vkGetBufferMemoryRequirements(c->device, buf, &req);
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT;   // the game's CPU reads this memory too
        for (uint32_t i = 0; i < c->mem_props.memoryTypeCount && type < 0; i++)
            if ((req.memoryTypeBits >> i & 1) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want) type = (int)i;
        if (type < 0) why = "no cached host-visible memory type";
        else if (req.size != n) why = "the allocation would not be the pool's size";
    }
    if (!why) {
        VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buf};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = n,
                                   .memoryTypeIndex = (uint32_t)type};
        if (vkAllocateMemory(c->device, &ai, NULL, &mem) != VK_SUCCESS) why = "no memory";
        else if (vkBindBufferMemory(c->device, buf, mem, 0) != VK_SUCCESS) why = "bind failed";
    }
    if (!why) {   // fill it through an ordinary map, then put it in place over the guest pages
        VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .memory = mem, .size = VK_WHOLE_SIZE};
        VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem};
        void *p = NULL;
        if (p_map2(c->device, &mi, &p) != VK_SUCCESS) why = "map failed";
        else {
            memcpy(p, (const void *)(uintptr_t)lo, n);
            p_unmap2(c->device, &ui);
            VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
                                           .pPlacedAddress = (void *)(uintptr_t)lo};
            mi.pNext = &pl, mi.flags = VK_MEMORY_MAP_PLACED_BIT_EXT, p = NULL;
            if (p_map2(c->device, &mi, &p) != VK_SUCCESS || p != (void *)(uintptr_t)lo) {
                // The guest range may be gone: put fresh pages back with the copy that was taken
                if (!plat_mem_replace_anon(lo, n)) rt_fatal("vk: pool %#llx: placed map failed and the range is lost",
                                                            (unsigned long long)lo);
                why = "placed map failed";
            }
        }
    }
    if (why) {
        static volatile int32_t said;
        if (plat_inc32(&said) <= 4) rt_log("vk: pool %#llx+%#llx not placed (%s): shadow copy instead",
                                           (unsigned long long)addr, (unsigned long long)size, why);
        if (buf) vkDestroyBuffer(c->device, buf, NULL);
        if (mem) vkFreeMemory(c->device, mem, NULL);
        return 0;
    }
    r->buf = buf, r->mem = mem, r->placed = 1, r->import_lo = lo, r->import_hi = hi;
    __atomic_add_fetch(&placed_n, 1, __ATOMIC_RELAXED), __atomic_add_fetch(&placed_bytes, (int64_t)n, __ATOMIC_RELAXED);
    plat_inc32(&n_imports);
    return 1;
}

// Before the allocation goes: the driver's map leaves a PROT_NONE reservation (unmap reserve), fresh anonymous
// pages replace it and get the pool's last contents back, so the game's memory outlives the pool as on the console.
static void placed_release(VkCtx *c, PoolRec *r) {
    uint64_t n = r->import_hi - r->import_lo;
    void *keep = malloc(n);
    if (!keep) rt_fatal("vk: out of memory releasing a placed pool");
    mem_thaw(r->import_lo, n);
    memcpy(keep, (const void *)(uintptr_t)r->import_lo, n);
    VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
                               .memory = r->mem};
    if (p_unmap2(c->device, &ui) != VK_SUCCESS || !plat_mem_replace_anon(r->import_lo, n))
        rt_fatal("vk: pool %#llx: placed map could not be undone", (unsigned long long)r->import_lo);
    memcpy((void *)(uintptr_t)r->import_lo, keep, n);
    free(keep);
    __atomic_sub_fetch(&placed_n, 1, __ATOMIC_RELAXED), __atomic_sub_fetch(&placed_bytes, (int64_t)n, __ATOMIC_RELAXED);
}

// CPU-visible storage: the guest pages themselves become the buffer's memory (VK_EXT_external_memory_host).
int import_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    if (c->has_placed && placed_pool(c, r, addr, size)) return 1;
    // A pool whose copy does not fit (MKW: one of 2.4 GB, flags 0x24, over MKW_GPU_MEM_LIMIT; run77-run78 died on it)
    // is imported instead: the other paths check each pool's smap, so imported and shadowed pools mix.
    // (Importing every CPU_NO_ACCESS pool too was tried, run82-run86: the pinned pages count in the process's memory
    // and it died sooner, frame 97-103 against 130.)
    if (shadow_on && shadow_pool(c, r, addr, size)) return 1;
    if (shadow_on) rt_log("vk: pool %#llx+%#llx: imported instead", (unsigned long long)addr, (unsigned long long)size);
    if (!p_host_props)
        p_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(c->device, "vkGetMemoryHostPointerPropertiesEXT");
    uint64_t al = c->min_imported_host_alignment ? c->min_imported_host_alignment : 0x1000;
    r->import_lo = addr & ~(al - 1);
    r->import_hi = (addr + size + al - 1) & ~(al - 1);
    uint64_t n = r->import_hi - r->import_lo;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult res = p_host_props ? p_host_props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               (void *)(uintptr_t)r->import_lo, &hp)
                                : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (res != VK_SUCCESS || !hp.memoryTypeBits) {
        rt_log("vk: pool %#llx+%#llx: host pointer not importable (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        return 0;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n,
                             .usage = pool_usage(c), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if ((res = vkCreateBuffer(c->device, &bi, NULL, &r->buf)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx: vkCreateBuffer for the import failed (%d)", (unsigned long long)addr, (int)res);
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, r->buf, &req);
    int type = -1;
    if (!vk_mem_is_discrete()) {
        VkMemoryPropertyFlags uma_want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (uint32_t i = 0; i < c->mem_props.memoryTypeCount && type < 0; i++)
            if (((req.memoryTypeBits & hp.memoryTypeBits) >> i & 1) &&
                (c->mem_props.memoryTypes[i].propertyFlags & uma_want) == uma_want)
                type = (int)i;
    }
    if (type < 0)
        type = memory_type(c, req.memoryTypeBits & hp.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0 || req.size > n) {
        rt_log("vk: pool %#llx: no memory type for the import (buffer %#x, host %#x, size %#llx of %#llx)",
               (unsigned long long)addr, req.memoryTypeBits, hp.memoryTypeBits, (unsigned long long)req.size,
               (unsigned long long)n);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = (void *)(uintptr_t)r->import_lo};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = n,
                               .memoryTypeIndex = (uint32_t)type};
    if ((res = vkAllocateMemory(c->device, &ai, NULL, &r->mem)) != VK_SUCCESS ||
        (res = vkBindBufferMemory(c->device, r->buf, r->mem, 0)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: host memory import failed (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->mem = VK_NULL_HANDLE;
        return 0;
    }
    plat_inc32(&n_imports);
    return 1;
}

// Physical pool: device-local memory that both buffers and optimal-tiling images can be bound to, in chunks of
// phys_chunk() bytes made the first time a MapVirtual request (or a direct resolve) reaches them. MKW reserves 12 pools
// of 1.1 GB with flags 0x18c (run93) and the whole of each did not fit: 46 pools failed, their MapVirtual requests were
// refused and those textures had no memory. MKW_PHYS_CHUNK=<MiB> (default 0 = the whole pool, at its first use). The
// game maps nearly all of it (run94-run95: 1 131 MiB), so chunks saved nothing, and an image larger than a chunk or
// across two could not be bound into the pool and got memory of its own: textures with own memory went from
// 427-643 MiB to 1 496-1 600 MiB (run104), which filled the heap.
static uint64_t phys_chunk_bytes;

uint64_t phys_chunk(void) {
    if (!phys_chunk_bytes) {
        const char *e = cfg_get("MKW_PHYS_CHUNK");
        long mib = e && *e ? atol(e) : phys_lazy() ? 4 : 0;
        phys_chunk_bytes = mib > 0 ? (uint64_t)mib << 20 : UINT64_MAX;
    }
    return phys_chunk_bytes;
}

// MKW_PHYS_LAZY (default 1; 0 off): a chunk gets memory when the GPU first resolves a virtual address over it, not at the
// MapVirtual that names it. The 1.1 GB the game maps backs its sparse textures, whose tiles get slots of their own here
// (vk_sparse.c): run114 still had 1 156 MiB of chunks with a full heap. With it the chunk size defaults
// to 4 MiB.
int phys_lazy(void) {
    static int v = -1;
    if (v < 0) v = !(cfg_get("MKW_PHYS_LAZY") && !atoi(cfg_get("MKW_PHYS_LAZY")));   // default on
    return v;
}

uint64_t chunk_len(const PoolRec *r, uint32_t i) {
    uint64_t ch = phys_chunk(), lo = (uint64_t)i * ch;
    return r->size - lo < ch ? r->size - lo : ch;
}

int64_t phys_backed_n, phys_backed_bytes, phys_failed_n;   // phys_lock

// Serializes chunk allocation: made outside pool_lock (the zero fill records an upload) and published under it.
PlatMutex phys_lock = PLAT_MUTEX_INIT;

// On UMA devices supporting VK_EXT_external_memory_host with HOST_VISIBLE | HOST_COHERENT | DEVICE_LOCAL memory types,
// guest physical pools (e.g. flags 0x18c) directly import the host mmap pointer via VkImportMemoryHostPointerInfoEXT
// instead of allocating duplicate VMA chunks.
static int import_phys_chunk(VkCtx *c, const PoolRec *r, uint32_t idx, uint64_t size, PhysChunk *out) {
    if (vk_mem_is_discrete() || !r->lo) return 0;
    uint64_t addr = r->lo + (uint64_t)idx * phys_chunk();
    uint64_t al = c->min_imported_host_alignment ? c->min_imported_host_alignment : 0x1000;
    if ((addr & (al - 1)) || (size & (al - 1))) return 0;
    if (!p_host_props)
        p_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
            c->device, "vkGetMemoryHostPointerPropertiesEXT");
    if (!p_host_props) return 0;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (p_host_props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)addr, &hp) !=
            VK_SUCCESS ||
        !hp.memoryTypeBits)
        return 0;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .pNext = &eb,
                             .size = size,
                             .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(c->device, &bi, NULL, &out->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, out->buf, &rb);
    uint32_t bits = rb.memoryTypeBits & hp.memoryTypeBits;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = -1;
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount && type < 0; i++)
        if ((bits >> i & 1) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want) type = (int)i;
    if (type < 0 || rb.size > size) {
        vkDestroyBuffer(c->device, out->buf, NULL);
        out->buf = VK_NULL_HANDLE;
        return 0;
    }
    mem_thaw(addr, size);
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = (void *)(uintptr_t)addr};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &im,
                               .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)type};
    if (vkAllocateMemory(c->device, &ai, NULL, &out->mem) != VK_SUCCESS ||
        vkBindBufferMemory(c->device, out->buf, out->mem, 0) != VK_SUCCESS) {
        if (out->mem) vkFreeMemory(c->device, out->mem, NULL);
        vkDestroyBuffer(c->device, out->buf, NULL);
        *out = (PhysChunk){0};
        return 0;
    }
    plat_inc32(&n_imports);
    return 1;
}

static int alloc_chunk(VkCtx *c, const PoolRec *r, uint32_t idx, uint64_t size, PhysChunk *out) {
    if (import_phys_chunk(c, r, idx, size, out)) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult res = vkCreateBuffer(c->device, &bi, NULL, &out->buf);
    if (res != VK_SUCCESS) { rt_log("vk: physical pool: vkCreateBuffer failed (%d)", (int)res); return 0; }
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, out->buf, &rb);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {256, 256, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkDeviceImageMemoryRequirements di = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ii};
    VkMemoryRequirements2 ri = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(c->device, &di, &ri);
    uint32_t bits = rb.memoryTypeBits & ri.memoryRequirements.memoryTypeBits;
    if (!bits) bits = rb.memoryTypeBits;
    VkDeviceSize align = NVN_VPOOL_PAGE;
    if (rb.alignment > align) align = rb.alignment;
    if (ri.memoryRequirements.alignment > align) align = ri.memoryRequirements.alignment;
    VkMemoryRequirements req = {.size = (rb.size + align - 1) & ~(align - 1), .alignment = align, .memoryTypeBits = bits};
    // Chunks share VMA blocks; a pool taken whole (MKW_PHYS_CHUNK=0) gets memory of its own as before.
    VmaAllocationCreateInfo aci = {.flags = size > (64ull << 20) ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0,
                                   .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if ((res = vmaAllocateMemory(c->vma, &req, &aci, &out->alloc, NULL)) != VK_SUCCESS ||
        (res = vmaBindBufferMemory(c->vma, out->alloc, out->buf)) != VK_SUCCESS) {
        rt_log("vk: physical pool chunk of %#llx bytes: device-local allocation failed (%d)", (unsigned long long)size,
               (int)res);
        if (out->alloc) vmaFreeMemory(c->vma, out->alloc);
        vkDestroyBuffer(c->device, out->buf, NULL);
        *out = (PhysChunk){0};
        return 0;
    }
    if (vk_fix_zero()) {   // Switch pool memory starts zeroed; once per chunk, not per image placed in it
        VkCommandBuffer cb = vk_upload_begin();
        vkCmdFillBuffer(cb, out->buf, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &dep);
        vk_upload_end();
    }
    return 1;
}

// Gives chunks [first, last] of r memory, pool_lock not held. A chunk that does not fit stays without memory: the
// virtual pages over it resolve as unmapped, and a later request tries again.
void phys_back(PoolRec *r, uint32_t first, uint32_t last) {
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&phys_lock);
    for (uint32_t i = first; i <= last && i < r->nchunks; i++) {
        if (r->chunks[i].buf) continue;
        PhysChunk ch = {0};   // use 0: no part names it yet (MKW_GPU_GUARD read garbage there: --probe-vkmem stopped)
        if (!alloc_chunk(c, r, i, chunk_len(r, i), &ch)) {
            if (++phys_failed_n <= 8) vram_log(0);
            continue;
        }
        plat_mutex_lock(&pool_lock);   // resolves read the chunk under the shared lock
        r->chunks[i] = ch;
        __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
        plat_mutex_unlock(&pool_lock);
        int64_t n = ++phys_backed_n;
        phys_backed_bytes += (int64_t)chunk_len(r, i);
        if (n <= 4 || !(n & (n - 1)))
            rt_log("vk: %lld physical pool chunks backed, %.0f MiB (last: chunk %u of %u of pool %#llx+%#llx flags %#x%s)",
                   (long long)n, phys_backed_bytes / 1048576.0, i, r->nchunks, (unsigned long long)r->lo,
                   (unsigned long long)r->size, r->flags, ch.mem ? ", imported host memory" : "");
    }
    plat_mutex_unlock(&phys_lock);
}

int vk_fix_zero(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("MKW_FIX_ZERO");
    }
    return on;
}

void destroy_rec(VkCtx *c, PoolRec *r) {
    if (vk_guard_on()) {   // a virtual pool's record holds no GPU object: its pages are the physical pools'
        if (r->kind != POOL_VIRT) vk_guard_dead("pool", (uint64_t)(uintptr_t)r->buf, r->lo, r->use);
        for (uint32_t i = 0; i < r->nchunks; i++)
            if (r->chunks[i].buf) vk_guard_dead("physical pool chunk", (uint64_t)(uintptr_t)r->chunks[i].buf,
                                                r->lo + i * phys_chunk(), r->chunks[i].use);
    }
    if (r->mbuf) vmaDestroyBuffer(c->vma, r->mbuf, r->malloc_);
    if (r->smap) mem_untrap(r->import_lo, r->import_hi - r->import_lo);   // trapped pages take their bytes from it
    if (r->salloc) vmaDestroyBuffer(c->vma, r->buf, r->salloc), r->buf = VK_NULL_HANDLE;
    if (r->placed) placed_release(c, r);
    if (r->buf) vkDestroyBuffer(c->device, r->buf, NULL);
    if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
    for (uint32_t i = 0; i < r->nchunks; i++) {
        if (r->chunks[i].buf) vkDestroyBuffer(c->device, r->chunks[i].buf, NULL);
        if (r->chunks[i].alloc) vmaFreeMemory(c->vma, r->chunks[i].alloc);
        if (r->chunks[i].mem) vkFreeMemory(c->device, r->chunks[i].mem, NULL);
    }
    free(r->chunks);
    free(r->pseen);
    free(r->pt);
    free(r);
}

// Lazy pools (MKW_LAZY_POOLS, on by default): a CPU-visible pool gets its GPU storage (shadow copy or import) when
// the GPU first resolves an address in it. MKW sets up ~3.5 GB of such pools while loading a course (run82), the
// 2.4 GB one among them, and the copies did not fit with the rest (run77-run89 died between frames 86 and 155).
int lazy_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = cfg_get("MKW_LAZY_POOLS");
        v = !e || !*e || strcmp(e, "0");
    }
    return v;
}

static long n_lazy_backed;
static int64_t lazy_backed_bytes;

// pool_lock held exclusively. The storage is made on a copy of the record and published whole, so a resolve under
// the shared lock never sees a buffer whose contents are not in it yet.
void lazy_back(PoolRec *r) {
    PoolRec tmp = *r;
    mem_thaw(r->lo, r->size);
    if (import_pool(vk_ctx(), &tmp, r->lo, r->size)) {
        tmp.lazy = 0;
        *r = tmp;
        long n = ++n_lazy_backed;
        lazy_backed_bytes += (int64_t)r->size;
        if (n <= 4 || !(n & (n - 1)))
            rt_log("vk: %ld lazy pools backed, %.0f MiB (last %#llx+%#llx flags %#x)", n, lazy_backed_bytes / 1048576.0,
                   (unsigned long long)r->lo, (unsigned long long)r->size, r->flags);
    } else {
        r->lazy = 0;   // no storage: as a pool whose import failed at init (its addresses resolve as unbacked)
        rt_log("vk: lazy pool %#llx+%#llx flags %#x: no GPU storage", (unsigned long long)r->lo,
               (unsigned long long)r->size, r->flags);
    }
}
