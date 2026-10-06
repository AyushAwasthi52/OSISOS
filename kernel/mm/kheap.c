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

static heap_block_t* heap_head = NULL;

void init_kheap(void) {
    /* 
     * The Heap sits on top of the PMM! 
     * We ask the PMM for a raw 4KB chunk of physical memory to get started.
     */
    void* initial_block = pmm_alloc_block();
    
    /* Place our very first header at the very beginning of the 4KB chunk */
    heap_head = (heap_block_t*) initial_block;
    
    /* The usable space is 4096 bytes minus the 12 bytes taken by this header */
    heap_head->size = PMM_FRAME_SIZE - sizeof(heap_block_t);
    heap_head->is_free = 1;
    heap_head->next = NULL;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    heap_block_t* current = heap_head;
    
    /* Search the linked list for a free block that is large enough (First-Fit Algorithm) */
    while (current != NULL) {
        if (current->is_free && current->size >= size) {
            
            /* We found a block! Can we split it into two? 
             * (Only if there is enough space left over for a new header + at least 4 bytes of data) */
            if (current->size >= size + sizeof(heap_block_t) + 4) {
                
                /* Do the pointer math to find exactly where the new block header should go */
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)current + sizeof(heap_block_t) + size);
                
                new_block->size = current->size - size - sizeof(heap_block_t);
                new_block->is_free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->is_free = 0; /* Mark as USED */
            
            /* Return the memory address immediately AFTER the header */
            return (void*)((uint8_t*)current + sizeof(heap_block_t));
        }
        current = current->next;
    }
    
    /* 
     * If we reach here, our initial 4KB chunk is full. 
     * In a robust OS, we would call pmm_alloc_block() again to expand the heap.
     * For now, we just return NULL (Out of memory).
     */
    return NULL;
}

void kfree(void* ptr) {
    if (ptr == NULL) return;
    
    /* The header is located immediately BEFORE the pointer we gave the user */
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    block->is_free = 1;
    
    /* 
     * In a more advanced allocator, we would merge adjacent free blocks here 
     * to prevent fragmentation.
     */
}
