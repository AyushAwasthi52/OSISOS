#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* 
 * A single entry in the Interrupt Descriptor Table.
 * It points to the memory address of the Assembly function that handles the interrupt.
 */
struct idt_entry_struct {
    uint16_t base_lo;             // The lower 16 bits of the handler's memory address
    uint16_t sel;                 // Kernel segment selector (our Code Segment from the GDT)
    uint8_t  always0;             // This must always be zero
    uint8_t  flags;               // Flags determining privilege level and if the interrupt is present
    uint16_t base_hi;             // The upper 16 bits of the handler's memory address
} __attribute__((packed));

typedef struct idt_entry_struct idt_entry_t;

/* 
 * A pointer to the array of IDT entries, passed to the `lidt` instruction.
 */
struct idt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct idt_ptr_struct idt_ptr_t;

void init_idt(void);

#endif
