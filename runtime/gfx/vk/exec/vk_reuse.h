// MKW_REUSE (vk_exec_reuse.c): a draw whose replay state equals one
// recorded in an earlier frame gets that draw's packets again instead of being replayed. The hooks the rest of the
// replay calls: the state hash's writers, the draw, and what a captured draw resolves.
#pragma once
#include <stdint.h>

#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"

extern int ru_on;   // MKW_REUSE: 0 off, 1 on, 2 check (every hit also replayed and compared); set at the first submission

void ru_submission(VkExec *ex);   // vk_exec_record, before the walk

// A draw op (DRAW or DRAW_IDX) about to be replayed: 1 when its packets came from the cache; 0 when the caller replays
// it, then calls ru_draw_end.
int ru_draw(VkExec *ex, uint32_t op, const void *payload);
void ru_draw_end(VkExec *ex);
void ru_log(void);   // MKW_TRACE=perf: hits, misses by cause

// What a draw being captured resolves (ru_cap is set only then, on the replay thread).
typedef struct RuCap RuCap;
extern _Thread_local RuCap *ru_cap;
enum { RUK_STABLE = 0, RUK_UBO, RUK_C0, RUK_SUP, RUK_C24 };   // a dynamic descriptor: what gives its buffer and offset
enum { RUB_VOLATILE = 1, RUB_WRITTEN, RUB_UBO_PTR, RUB_STORAGE_IMG, RUB_SCRATCH_INDEX, RUB_NO_TPL, RUB_TOO_MANY };
void ru_note_mem(uint64_t addr, uint64_t size, const VkMemSpan *sp, int ok);
void ru_note_tex(uint64_t h, VkImage image, int storage);
void ru_note_bad(int why);
void ru_note_fbl(uint32_t fbl);   // the attachments the draw samples (feedback_prepare)
void ru_note_slot(int kind, int st, int slot);   // a slot read: 0 texture, 1 storage block, 2 uniform block as storage
void ru_note_desc(uint32_t at, int kind, int st, int slot);   // descriptor at of the draw's packed array
void ru_note_set(const VkPipeInfo *info, uint32_t s, const VkDescSlot *ds, uint32_t n, uint32_t at, uint32_t i0, uint32_t i1);

// vk_exec_bind.c: binds a reused draw's set s again from its recorded descriptors, the ones of kind RUK_* other than
// stable made again for this submission; 0 when one cannot be (its uniform block is not in this submission's copy).
int ru_ubo_ok(VkExec *ex, int st, int slot);   // the uniform block at slot of stage st binds from this submission's copy
// The set a hit bound last, kept in its entry: the draw cache's set, a signature of the buffers it was bound with and
// the cache's emptyings then (vk_dcache_epoch). ru_bind_set returns 2 when it bound that set again.
typedef struct { VkDescriptorSet set; uint64_t sig, epoch; } RuSetMemo;
int ru_bind_set(VkExec *ex, const VkPipeInfo *info, uint32_t s, VkDescSlot *ds, uint32_t n, uint32_t i0, uint32_t i1,
                const uint8_t *kind, const uint8_t *kst, const uint8_t *kslot, uint32_t c0_binst_, RuSetMemo *rs);
