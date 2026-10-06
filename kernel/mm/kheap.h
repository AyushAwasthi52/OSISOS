#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>
#include <stddef.h>

/* Initialize the Kernel Heap by requesting an initial chunk from the PMM */
void init_kheap(void);

/* Allocate a specific number of bytes from the heap */
void* kmalloc(size_t size);

/* Free the allocated bytes back to the heap */
void kfree(void* ptr);

#endif
