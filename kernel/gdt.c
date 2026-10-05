#include "gdt.h"

// Let's declare an external assembly function that actually loads the GDT pointer
extern void gdt_flush(uint32_t);

// Our GDT, with 3 entries, and our GDT pointer
gdt_entry_t gdt_entries[3];
gdt_ptr_t   gdt_ptr;

// A helper function to easily set up a GDT entry
static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

void init_gdt(void) {
    // Set the limits and base for our GDT pointer
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 3) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    // Entry 0: The Null Segment (Required by the CPU)
    gdt_set_gate(0, 0, 0, 0, 0);

    // Entry 1: The Code Segment
    // Base: 0, Limit: 4GB, Access: 0x9A (Kernel level, Code, Executable, Readable)
    // Granularity: 0xCF (Page granularity, 32-bit opcodes)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // Entry 2: The Data Segment
    // Base: 0, Limit: 4GB, Access: 0x92 (Kernel level, Data, Read/Write)
    // Granularity: 0xCF
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // Tell the CPU where our new GDT is located and reload segment registers
    gdt_flush((uint32_t)&gdt_ptr);
}
