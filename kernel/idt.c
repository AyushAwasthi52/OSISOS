#include "idt.h"

// External assembly functions
extern void idt_flush(uint32_t);
extern void isr0(void); // Our Divide-By-Zero handler from Assembly

idt_entry_t idt_entries[256];
idt_ptr_t   idt_ptr;

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_lo = base & 0xFFFF;
    idt_entries[num].base_hi = (base >> 16) & 0xFFFF;
    idt_entries[num].sel     = sel;
    idt_entries[num].always0 = 0;
    // We must uncomment the OR below when we get to user-mode.
    // For now, it stays at kernel privilege (ring 0).
    idt_entries[num].flags   = flags /* | 0x60 */;
}

void init_idt(void) {
    idt_ptr.limit = sizeof(idt_entry_t) * 256 - 1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    // Clear out the entire IDT initially (all 256 entries to zero)
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // Connect Exception 0 (Divide by Zero) to our Assembly function `isr0`
    // 0x08 is our Code Segment from the GDT.
    // 0x8E means: Present, Ring 0, 32-bit Interrupt Gate.
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);

    // Tell the CPU where the IDT is!
    idt_flush((uint32_t)&idt_ptr);
}
