#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* 
 * This structure defines a single entry in the Global Descriptor Table.
 * It is packed to ensure the compiler doesn't add padding bytes, 
 * as the CPU expects this exact 8-byte format.
 */
struct gdt_entry_struct {
    uint16_t limit_low;   // The lower 16 bits of the segment limit
    uint16_t base_low;    // The lower 16 bits of the base address
    uint8_t  base_middle; // The next 8 bits of the base address
    uint8_t  access;      // Access flags (determines privilege ring and segment type)
    uint8_t  granularity; // Granularity and the upper 4 bits of the limit
    uint8_t  base_high;   // The last 8 bits of the base address
} __attribute__((packed));

typedef struct gdt_entry_struct gdt_entry_t;

/* 
 * This structure describes a pointer to the GDT array.
 * This is what we pass to the CPU's `lgdt` instruction.
 */
struct gdt_ptr_struct {
    uint16_t limit; // The upper 16 bits of all selector limits (size of GDT - 1)
    uint32_t base;  // The address of the first gdt_entry_t struct
} __attribute__((packed));

typedef struct gdt_ptr_struct gdt_ptr_t;

// Initialize the GDT
void init_gdt(void);

#endif
