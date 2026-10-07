#ifndef PMM_H
#define PMM_H

#include <stdint.h>

/* Every block of memory we hand out will be exactly 4096 bytes (4 KB) */
#define PMM_FRAME_SIZE 4096

/* Initialize the Physical Memory Manager with the total RAM available */
void init_pmm(uint32_t total_memory_kb);

/* Ask the OS for a free 4KB chunk of physical RAM */
void* pmm_alloc_block(void);

/* Give the 4KB chunk back to the OS */
void pmm_free_block(void* physical_address);

uint32_t pmm_get_total_memory(void);
uint32_t pmm_get_free_memory(void);

#endif
