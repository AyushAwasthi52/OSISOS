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
            
            /* Attribute 7 = 0x111 in binary (Present = 1, Read/Write = 1, User Mode = 1) */
            page_table[j] = physical_address | 7; 
        }
        
        /* Put the Page Table into the Page Directory */
        kernel_page_directory[i] = ((uint32_t)page_table) | 7;
    }
    
    /* 3. Load the Page Directory into the CPU (CR3 register) */
    load_page_directory(kernel_page_directory);
    
    /* 4. Turn on Paging! (Set the highest bit in CR0) */
    enable_paging();
}

/* Map a single physical page (4KB) to a virtual page */
void map_page(uint32_t physical_addr, uint32_t virtual_addr) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x03FF;
    
    // Check if the page table exists
    if ((kernel_page_directory[pd_index] & 1) == 0) {
        // Allocate a new page table
        uint32_t* new_pt = (uint32_t*) pmm_alloc_block();
        memset32(new_pt, 0, 1024);
        kernel_page_directory[pd_index] = ((uint32_t)new_pt) | 7;
    }
    
    // Get the page table pointer
    uint32_t* page_table = (uint32_t*)(kernel_page_directory[pd_index] & ~0xFFF);
    
    // Map the page
    page_table[pt_index] = (physical_addr & ~0xFFF) | 7; // Present, R/W, User
    
    // Flush the TLB
    __asm__ volatile ("invlpg (%0)" : : "r" (virtual_addr) : "memory");
}
