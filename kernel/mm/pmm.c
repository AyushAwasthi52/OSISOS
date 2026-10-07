#include "pmm.h"
#include <stddef.h>

/*
 * We need an array of bits to track up to 32MB of RAM.
 * 32 MB / 4 KB = 8192 total frames.
 * 8192 bits / 32 bits per integer = 256 array entries.
 */
#define BITMAP_SIZE 256
static uint32_t pmm_bitmap[BITMAP_SIZE];

static uint32_t max_frames = 0;

/* Helper to set a bit to 1 (USED) */
static inline void bitmap_set(uint32_t bit) {
    pmm_bitmap[bit / 32] |= (1 << (bit % 32));
}

/* Helper to clear a bit to 0 (FREE) */
static inline void bitmap_clear(uint32_t bit) {
    pmm_bitmap[bit / 32] &= ~(1 << (bit % 32));
}

/* Helper to check if a bit is 1 or 0 */
static inline uint32_t bitmap_test(uint32_t bit) {
    return pmm_bitmap[bit / 32] & (1 << (bit % 32));
}

void init_pmm(uint32_t total_memory_kb) {
    /* Calculate how many 4KB frames exist in the total memory */
    max_frames = (total_memory_kb * 1024) / PMM_FRAME_SIZE;
    
    /* Initially mark ALL memory as USED (1) as a safety precaution. */
    for (int i = 0; i < BITMAP_SIZE; i++) {
        pmm_bitmap[i] = 0xFFFFFFFF;
    }
    
    /* 
     * The first 4MB of physical RAM contains important hardware mappings (VGA)
     * and the actual code of our Kernel. We do not want to hand this memory out!
     * 4MB / 4KB = 1024 frames. 
     * So we only mark frames AFTER 1024 as FREE (0).
     */
    for (uint32_t i = 1024; i < max_frames; i++) {
        bitmap_clear(i);
    }
}

void* pmm_alloc_block(void) {
    /* Scan through the bitmap array */
    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        /* If the entry is not 0xFFFFFFFF, it means there is at least one '0' (FREE) bit here! */
        if (pmm_bitmap[i] != 0xFFFFFFFF) { 
            
            /* Scan the 32 individual bits in this entry to find the exact free one */
            for (int j = 0; j < 32; j++) {
                uint32_t bit = i * 32 + j;
                
                /* Did we find the free bit? */
                if (!bitmap_test(bit)) { 
                    bitmap_set(bit); /* Mark it as USED so no one else gets it */
                    
                    /* Calculate the physical memory address from the bit number */
                    uint32_t physical_address = bit * PMM_FRAME_SIZE;
                    return (void*) physical_address;
                }
            }
        }
    }
    return NULL; /* Out of Memory! */
}

void pmm_free_block(void* physical_address) {
    uint32_t addr = (uint32_t) physical_address;
    uint32_t bit = addr / PMM_FRAME_SIZE;
    bitmap_clear(bit); /* Mark it as FREE again */
}

uint32_t pmm_get_total_memory(void) {
    return max_frames * PMM_FRAME_SIZE;
}

uint32_t pmm_get_free_memory(void) {
    uint32_t free_frames = 0;
    for (uint32_t i = 1024; i < max_frames; i++) {
        if (!bitmap_test(i)) {
            free_frames++;
        }
    }
    return free_frames * PMM_FRAME_SIZE;
}
