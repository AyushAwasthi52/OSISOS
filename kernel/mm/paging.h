#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Set up the Page Directory, Page Tables, and enable the MMU */
void init_paging(void);

#endif
