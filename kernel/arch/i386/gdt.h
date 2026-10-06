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

/* A struct describing a Task State Segment (TSS). */
struct tss_entry_struct {
    uint32_t prev_tss;   // The previous TSS (if we used hardware task switching)
    uint32_t esp0;       // The stack pointer to load when changing to kernel mode
    uint32_t ss0;        // The stack segment to load when changing to kernel mode
    uint32_t esp1;       // Unused
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;         
    uint32_t cs;        
    uint32_t ss;        
    uint32_t ds;        
    uint32_t fs;       
    uint32_t gs;         
    uint32_t ldt;      
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

typedef struct tss_entry_struct tss_entry_t;

/* Update the TSS to point to a new kernel stack when context switching */
void set_kernel_stack(uint32_t stack);

#endif
