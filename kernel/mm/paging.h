#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Set up the Page Directory, Page Tables, and enable the MMU */
void init_paging(void);

/* Map a specific physical page to a virtual page */
void map_page(uint32_t physical_addr, uint32_t virtual_addr);

/* Unmap virtual page 0 so a NULL-pointer dereference raises a page fault. */
void paging_guard_null_page(void);

#endif
