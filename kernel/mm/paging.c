#include "paging.h"
#include "pmm.h"
#include <stddef.h>

/* Helper to zero out a 4KB block of memory */
static void memset32(uint32_t* dest, uint32_t val, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dest[i] = val;
    }
}

/* External assembly functions to load CR3 and enable Paging in CR0 */
extern void load_page_directory(uint32_t*);
extern void enable_paging(void);

uint32_t* kernel_page_directory = NULL;

void init_paging(void) {
    /* 1. Allocate a 4KB aligned block for the Page Directory */
    kernel_page_directory = (uint32_t*) pmm_alloc_block();
    
    /* We must zero it out so the CPU doesn't interpret garbage as valid page tables! */
    /* A page directory has 1024 entries (4 bytes each = 4096 bytes) */
    memset32(kernel_page_directory, 0, 1024);
    
    /* 
     * 2. Identity Map the entire 32MB of RAM.
     * We have 32MB of RAM. Each Page Table covers 4MB (1024 entries * 4KB).
     * 32MB / 4MB = 8 Page Tables needed to map everything.
     */
    for (uint32_t i = 0; i < 8; i++) {
        /* Allocate a 4KB aligned block for a Page Table */
        uint32_t* page_table = (uint32_t*) pmm_alloc_block();
        memset32(page_table, 0, 1024);
        
        for (uint32_t j = 0; j < 1024; j++) {
            /* Calculate the exact physical address this entry should point to */
            uint32_t physical_address = (i * 4194304) + (j * 4096);
            
            /* Attribute 3 = 0x011 in binary (Present = 1, Read/Write = 1) */
            page_table[j] = physical_address | 3; 
        }
        
        /* Put the Page Table into the Page Directory */
        kernel_page_directory[i] = ((uint32_t)page_table) | 3;
    }
    
    /* 3. Load the Page Directory into the CPU (CR3 register) */
    load_page_directory(kernel_page_directory);
    
    /* 4. Turn on Paging! (Set the highest bit in CR0) */
    enable_paging();
}
