#include "kheap.h"
#include "pmm.h"

/*
 * A block header for our linked-list heap.
 * We use uint32_t instead of uint8_t for 'is_free' so the struct
 * is perfectly 12 bytes long, keeping memory aligned to 4 bytes!
 */
typedef struct heap_block {
    uint32_t size;              /* Size of the usable block (excluding header) */
    uint32_t is_free;           /* 1 if free, 0 if used */
    struct heap_block* next;    /* Pointer to the next block in the list */
} heap_block_t;

/*
 * The heap used to be a single 4 KB frame, which is far too small once the
 * network stack exists (the e1000 driver alone wants ~70 KB of DMA buffers and
 * every task needs a 4 KB stack). We now take 1 MB of physically contiguous frames.
 */
#define KHEAP_FRAMES 256

static heap_block_t* heap_head = NULL;

void init_kheap(void) {
    uint32_t bytes = KHEAP_FRAMES * PMM_FRAME_SIZE;
    void* region = pmm_alloc_contiguous(KHEAP_FRAMES);
    if (!region) {                       /* fall back to the old single frame */
        region = pmm_alloc_block();
        bytes = PMM_FRAME_SIZE;
    }

    heap_head = (heap_block_t*) region;
    heap_head->size = bytes - sizeof(heap_block_t);
    heap_head->is_free = 1;
    heap_head->next = NULL;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 3) & ~(size_t)3;      /* keep every header 4-byte aligned */

    heap_block_t* current = heap_head;

    /* First-fit search */
    while (current != NULL) {
        if (current->is_free && current->size >= size) {
            /* Split if there is room for another header + at least 4 bytes */
            if (current->size >= size + sizeof(heap_block_t) + 4) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)current + sizeof(heap_block_t) + size);
                new_block->size = current->size - size - sizeof(heap_block_t);
                new_block->is_free = 1;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }
            current->is_free = 0;
            return (void*)((uint8_t*)current + sizeof(heap_block_t));
        }
        current = current->next;
    }
    return NULL;                         /* out of memory */
}

void kfree(void* ptr) {
    if (ptr == NULL) return;

    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    block->is_free = 1;

    /* Merge runs of adjacent free blocks to limit fragmentation. */
    heap_block_t* cur = heap_head;
    while (cur != NULL && cur->next != NULL) {
        if (cur->is_free && cur->next->is_free &&
            (uint8_t*)cur + sizeof(heap_block_t) + cur->size == (uint8_t*)cur->next) {
            cur->size += sizeof(heap_block_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
